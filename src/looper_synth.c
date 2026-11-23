/*
 * Looper 606 -- 6-track live looper as a Schwung sound_generator.
 *
 * Shim around our fork of the Mark looper engine (mark_core.c, modified for
 * Looper 606; native MARK_TRACKS=6, undoable clear, varispeed). This file
 * translates
 *   MIDI notes (Movy drum-rack pads) -> mark_set_param() actions,
 *   hardware audio input (host mailbox) -> mark_process(),
 * and adds what the core does not have: MUTE, ARM with input threshold,
 * count-in, fixed recording length, input gain, per-track "state" enum.
 *
 * Mark engine: (c) Tim Cox, MIT (https://github.com/timncox/schwung-mark),
 * see LICENSE-mark. Ideas from Boss RC-505 / Loopex only; no Loopex code.
 */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "plugin_api_v1.h"
#include "mark_core.h"
#include "perform.h"
#include "perform_ui.h"

/* ------------------------------------------------------------------ */
/*  PAD <-> ACTION TABLE (the one place to change the pad layout)      */
/* ------------------------------------------------------------------ */
/*
 * Physical 4x8 grid, listed TOP row first (row 4), columns 1..8.
 * Movy seats a "drums" rack as 4x4 banks from the bottom-left: rack pad p
 * (0-based) sits at row-from-bottom (p%16)/4, column (p%4)+4*(p/16) and
 * plays the note declared for its voice in the hierarchy (NOTE_BASE + p).
 * Raw Move pad notes 68..99 are also accepted, in case pads reach the DSP
 * un-remapped.
 */
#define SHIFT_TAP_FRAMES ((long)MARK_SR * 3 / 10)   /* SHIFT tap = press+release under 300 ms */
#define NOTE_BASE      36
#define RAW_PAD_FIRST  68

enum act {
    A_NONE = 0,
    A_REC,        /* arg = track: REC->PLAY->DUB (per Rec Action)      */
    A_STOP,       /* arg = track: row 2 PLAY/STOP                         */
    A_MUTE,       /* arg = track: mute (keeps running silently)         */
    A_SEL,        /* arg = track: row 4 TRACK SELECTOR (PISTA view)     */
    A_ALL,        /* all start/stop                                     */
    A_STOPALL,    /* stop every track                                   */
    A_SHIFT,      /* looper SHIFT (hold), top pad of column 1           */
    A_ARM,        /* arm auto-rec toggle; held = reverse modifier       */
    A_COUNTIN,    /* count-in on/off                                    */
    A_RECACT,     /* rec action: rec->play / rec->dub                   */
    A_FIXLEN,     /* fixed length: free/1/2/4/8 bars (cycles)           */
    A_QUANT       /* quantize on/off                                    */
};

typedef struct { uint8_t act, arg; const char *label; } pad_def_t;
#define P(a, n, l) { (a), (n), (l) }

static const pad_def_t PAD_GRID[4][8] = {
  /* row 4 (top) */ { P(A_SHIFT,0,"Shift"), P(A_SEL,1,"Select 1"), P(A_SEL,2,"Select 2"),
                      P(A_SEL,3,"Select 3"), P(A_SEL,4,"Select 4"), P(A_SEL,5,"Select 5"),
                      P(A_SEL,6,"Select 6"), P(A_COUNTIN,0,"Count-in") },
  /* row 3       */ { P(A_ARM,0,"Arm/Reverse"), P(A_MUTE,1,"Mute 1"), P(A_MUTE,2,"Mute 2"),
                      P(A_MUTE,3,"Mute 3"), P(A_MUTE,4,"Mute 4"), P(A_MUTE,5,"Mute 5"),
                      P(A_MUTE,6,"Mute 6"), P(A_RECACT,0,"Rec Action") },
  /* row 2       */ { P(A_ALL,0,"All Start/Stop"), P(A_STOP,1,"Play/Stop 1"), P(A_STOP,2,"Play/Stop 2"),
                      P(A_STOP,3,"Play/Stop 3"), P(A_STOP,4,"Play/Stop 4"), P(A_STOP,5,"Play/Stop 5"),
                      P(A_STOP,6,"Play/Stop 6"), P(A_FIXLEN,0,"Fixed Length") },
  /* row 1 (bot) */ { P(A_STOPALL,0,"Stop All"), P(A_REC,1,"Rec/Dub 1"), P(A_REC,2,"Rec/Dub 2"),
                      P(A_REC,3,"Rec/Dub 3"), P(A_REC,4,"Rec/Dub 4"), P(A_REC,5,"Rec/Dub 5"),
                      P(A_REC,6,"Rec/Dub 6"), P(A_QUANT,0,"Quantize") },
};

#define DEFAULT_THRESHOLD (-42)      /* dBFS (~260/32768): above line-in noise (~-66), below quiet playing */
#define NTRK              MARK_TRACKS
#define MAXBLK            128        /* core block size */

/* Fixed recording length is a number of bars, 0 = free, 1..MAX_BARS. The pad in
 * column 8 cycles free -> 1 -> 2 -> 4 -> 8 -> 16 -> 32 -> free; the 16 step buttons pick
 * any of 1..16 directly (pressing the chosen one again = free). */
#define MAX_BARS 32
#define NSTEP_BTN 16                   /* the step row picks 1..16 bars directly */
#define NCYCLE 6
static const int CYCLE_BARS[NCYCLE] = { 1, 2, 4, 8, 16, 32 };

/* Knob view. MIX (sel = 0): the knobs are the volumes (Movy's root page). PISTA
 * (sel = 1..6, chosen with the row-4 selector pads): Movy hands the knob turns
 * to knob_delta (knob_capture = 1) and the 8 knobs edit the selected track:
 * K1 IN, K2 OUT, K3 vol, K4 pan, K5 FX type, K6 FX amount, K7 speed, K8 reverse.
 * Enum knobs need DETENTS turn clicks per step. */
#define DETENTS 3
static const int THR_CYCLE[5] = { -54, -48, -42, -36, -30 };

/* Shim-level track state enum (tN_state), read-only for the UI. */
enum { S_EMPTY = 0, S_ARMED, S_REC, S_PLAY, S_DUB, S_STOP, S_MUTE };

#define WAVE_BINS 128

static const host_api_v1_t *g_host;

typedef struct {
    mark_t *m;
    int cur_track;                 /* 1..NTRK, target of the Track page */
    int arm;                       /* auto-rec armed mode */
    int  arm_held;                 /* ARM pad is down: modifier for REVERSE */
    int  arm_used;                 /* a track pad was tapped while ARM was held */
    int  pedal_held[7];            /* Blackstar Live Logic switch n (1..6) is down */
    int  pedal_fired[7];           /* its 1 s hold action already fired (the tap must not) */
    long pedal_at[7];              /* clock_frames at its press */
    int  dub_after[NTRK];          /* row-1 on a stopped track: dub as soon as it is playing */
    int  offs_logged;
    int armed[NTRK];               /* waiting for the input threshold */
    int muted[NTRK];
    int saved_level[NTRK];
    long countin[NTRK];            /* frames left, -1 = none */
    long rec_frames[NTRK];         /* frames spent in REC */
    int  rec_fired[NTRK];
    int  prev_st[NTRK];
    int  threshold;                /* -70..-6 dBFS */
    int  count_in;
    int  rec_len;                  /* fixed recording length in bars, 0 = free */
    int  sel;                      /* PISTA view: selected track 1..NTRK, 0 = MIX */
    int  shift_held;               /* looper SHIFT pad is down */
    int  shift_used;               /* a combo was used while SHIFT was held: its release does not toggle PERFORM */
    long shift_at;                 /* clock_frames at the SHIFT press (a tap is < 300 ms) */
    int  perf_mode;                /* PERFORM mode (SHIFT tap toggles; not persisted) */
    int  auto_hold;                /* track whose REC pad is held for knob automation (PERFORM), 0 = none */
    long auto_hold_at;
    int  auto_turned;              /* a knob was turned during this hold */
    int  grid_view;                /* PERFORM: show the effect grid even though an effect is on (arrow tap; a knob turn goes back to the scene) */
    int  perf_bank;                /* PERFORM step-button page (< / > flip it): 0 = steps 1-16, 1 = effects 17+ */
    int  down_fx[PF_BANK];         /* effect bound to each step button while it is down (a page flip must not orphan the release) */
    perf_t *perf;                  /* PERFORM effects (perform.c), attached to the core */
    perf_ui_t pui;                 /* PERFORM graphics state (get_param path only) */
    int  knob_acc[9];              /* detent accumulators of the enum knobs */
    int  last_knob;                /* last PISTA knob touched (display view) */
    char pista_lab[24];            /* readout of the last PISTA knob ("IN +8/16") */
    /* RESET undo: settings snapshot taken by reset_track; valid while the core's
     * undo entry is still that track's CLEAR (see undo_cmd). */
    struct {
        int valid, track;
        int lvl[NTRK], pan, fx, fx_on, fxp, spd, rev, shot;
        char fx_module[64];
        int muted[NTRK], saved_level[NTRK], solo, solo_saved[NTRK];
    } rsnap;
    int  host_mute;                /* Movy muted this track (track_mute_param): the whole output is silenced */
    float mute_g;                  /* the gain applied to it, ramped (audio thread) */
    unsigned play_mask;            /* tracks playing at the last transport stop (bit i = track i+1), 0 = none */
    int  solo;                     /* soloed track, 0 = none */
    int  solo_saved[NTRK];         /* mute flags before the solo */
    int  count_bars;               /* count-in length in bars, 1 or 2 */
    int  end_t;                    /* track whose END was last edited (undo coalescing) */
    long end_until;                /* clock_frames until a new END edit opens a new undo entry */
    int  in_gain;                  /* 0..200 percent */
    int  clock_logged, audio_logged, notes_logged;
    long in_frames;                /* frames rendered so far (diagnostics) */
    int  in_peak_1s;               /* raw input peak over the first second */
    int  in_diag_done;             /* the "peak after 1 s" line was logged */
    int  arm_diag_frames, arm_diag_peak;  /* armed: 1 s input-peak log */
    int  pend_seen[NTRK];          /* pending was logged for this track */
    int  in_peak_now;              /* raw peak of the last block (get_param in_peak) */
    int  bpm_src_logged;           /* last tempo source logged (-1 = none yet) */
    long pend_frames[NTRK];        /* frames a track has been waiting on the grid */
    int  pend_logged;
    unsigned status_seq;           /* bumps on every status_text change */
    char status_text[40];          /* last action, for Movy's toast */
    /* display_param cache: the peaks are rescanned only when the clip can have
     * changed (a pad was pressed, the state/length moved, or a refresh timer
     * ran); the playhead is read fresh every call. */
    int      start_t;              /* track whose clip start was just moved (0 = none) */
    long     start_until;          /* clock_frames until the START readout fades */
    long     clock_frames;         /* frames rendered since create */
    uint8_t  wave[WAVE_BINS];
    int      wave_track, wave_st, wave_dirty, wave_whole;
    uint32_t wave_span;
    uint32_t wave_lo, wave_hi;     /* buffer window of the cached peaks (0,0 = legacy REC/empty view) */
    int      zoom_t, zoom_pt, zoom_fine;   /* zoom on track zoom_t's IN (0) or OUT (1) point */
    long     zoom_until;           /* clock_frames until the zoom ends */
    long     focus_until;          /* clock_frames until the last PISTA knob stops showing its value on the page */
    long     wave_at;
    unsigned wave_seq;
    uint8_t  ticks[WAVE_BINS];     /* bar / beat ticks of the display (bin | 0x80 = beat) */
    int      nticks;
    uint8_t  phrase[WAVE_BINS];    /* 4-bar (phrase) tick bins */
    int      nphrase;
    /* Persistence (see "Loop persistence" below). The thread does all file I/O. */
    char     loop_id[2][16];       /* double buffer: the audio thread may read it */
    _Atomic int loop_id_ix;
    char     persist_base[240];
    long     persist_delay_ms;
    pthread_t persist_thr;
    int      persist_thr_valid;
    _Atomic int persist_stop;
    _Atomic int persist_load_req;
    _Atomic int persist_error;
    int      persist_off;          /* persist thread only: this id's folder is unreadable, hands off */
    uint64_t saved_sig[NTRK];      /* persist thread only */
    char hierarchy[32768];
} synth_t;

static void hlog(const char *fmt, ...) {
    char b[200];
    va_list ap;
    if (!g_host || !g_host->log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_host->log(b);
}

/* Status line for Movy: get_param status_text = "<seq>:<text>". Movy toasts
 * the text whenever seq changes. */
static void say(synth_t *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->status_text, sizeof s->status_text, fmt, ap);
    va_end(ap);
    s->status_seq = (s->status_seq + 1) % 100000u;
}

/* ---- core access helpers ----------------------------------------- */

static void set_i(synth_t *s, const char *key, int v) {
    char b[16]; snprintf(b, sizeof b, "%d", v);
    mark_set_param(s->m, key, b);
}
static int get_i(synth_t *s, const char *key) {
    char b[32] = "0";
    if (mark_get_param(s->m, key, b, sizeof b) < 0) return 0;
    return atoi(b);
}
static void trig(synth_t *s, const char *key) { mark_set_param(s->m, key, "1"); }
static void trig_t(synth_t *s, int t, const char *k) {
    char key[24]; snprintf(key, sizeof key, "t%d_%s", t, k); trig(s, key);
}
static int get_t(synth_t *s, int t, const char *k) {
    char key[24]; snprintf(key, sizeof key, "t%d_%s", t, k); return get_i(s, key);
}
static void set_t(synth_t *s, int t, const char *k, int v) {
    char key[24]; snprintf(key, sizeof key, "t%d_%s", t, k); set_i(s, key, v);
}
static void toggle_t(synth_t *s, int t, const char *k) { set_t(s, t, k, !get_t(s, t, k)); }

static int peak_db(int p) { return p < 1 ? -99 : (int)floor(20.0 * log10(p / 32768.0) + 0.5); }
static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int core_state(synth_t *s, int t) { return get_t(s, t, "state"); }

static int core_pending(synth_t *s, int t) {
    char b[64] = "";
    if (mark_get_param(s->m, "tpend", b, sizeof b) < 0) return 0;
    const char *p = b;
    for (int i = 1; i < t; i++) { p = strchr(p, ','); if (!p) return 0; p++; }
    return atoi(p);
}

static const char *const BPM_SRC_NAME[4] = { "default 120", "host get_bpm()", "MIDI clock", "override" };

/* Tempo the engine itself is using: override > running clock > host
 * get_bpm() > 120. Always a sane value, so count-in / fixed length never
 * stall waiting for a clock that Movy may not deliver. */
static double cur_bpm(synth_t *s) {
    char b[32] = "";
    double v = 0.0;
    if (mark_get_param(s->m, "tempo_bpm", b, sizeof b) > 0) v = atof(b);
    if (v < 20.0 || v > 999.0) v = 120.0;
    return v;
}

static int shim_state(synth_t *s, int t) {
    int st = core_state(s, t);
    if (st == MK_EMPTY)
        return (s->armed[t - 1] || s->countin[t - 1] >= 0 || core_pending(s, t))
               ? S_ARMED : S_EMPTY;
    if (s->muted[t - 1]) return S_MUTE;
    switch (st) {
    case MK_REC:  return S_REC;
    case MK_PLAY: return S_PLAY;
    case MK_DUB:  return S_DUB;
    case MK_STOP: return S_STOP;
    default:      return S_EMPTY;
    }
}

static void set_mute(synth_t *s, int t, int on) {
    int i = t - 1;
    if (on == s->muted[i]) return;
    if (on) { s->saved_level[i] = get_t(s, t, "level"); set_t(s, t, "level", 0); }
    else    { set_t(s, t, "level", s->saved_level[i]); }
    s->muted[i] = on;
}

static void clear_flags(synth_t *s, int t) {
    int i = t - 1;
    s->armed[i] = 0; s->countin[i] = -1; s->rec_fired[i] = 0; s->rec_frames[i] = 0;
    s->dub_after[i] = 0;
}

/* ---- pads --------------------------------------------------------- */

static const pad_def_t *pad_for_note(int note) {
    int row_b, col;
    if (note >= NOTE_BASE && note < NOTE_BASE + 32) {
        int p = note - NOTE_BASE;
        row_b = (p % 16) / 4;
        col   = (p % 4) + 4 * (p / 16);
    } else if (note >= RAW_PAD_FIRST && note < RAW_PAD_FIRST + 32) {
        int idx = note - RAW_PAD_FIRST;
        row_b = idx / 8;
        col   = idx % 8;
    } else return NULL;
    return &PAD_GRID[3 - row_b][col];
}

/* What the next REC/PLAY/DUB press on track t will do, as a status line. */
static void say_rec(synth_t *s, int t) {
    switch (core_state(s, t)) {
    case MK_EMPTY: say(s, "REC %d", t); break;
    case MK_REC:   say(s, get_i(s, "rec_action") ? "DUB %d" : "PLAY %d", t); break;
    case MK_PLAY:  say(s, "DUB %d", t); break;
    default:       say(s, "PLAY %d", t); break;   /* DUB -> PLAY, STOP -> PLAY */
    }
}

/* Fixed recording length for the next recordings, with its status line. */
/* Whole bars that fit a track buffer at the current tempo (the buffer holds track_secs seconds). */
static int fit_bars(synth_t *s);
static int fit_cycle(synth_t *s) {                    /* the largest cycle value (1,2,4..32) that fits */
    int fit = fit_bars(s), m = 1;
    for (int i = 0; i < NCYCLE; i++) if (CYCLE_BARS[i] <= fit) m = CYCLE_BARS[i];
    return m;
}
static void set_rec_bars(synth_t *s, int bars) {
    bars = clampi_(bars, 0, MAX_BARS);
    int fit = fit_bars(s);
    if (bars > fit) {                                  /* too long for the buffer at this tempo: the largest cycle value that fits */
        int m = fit_cycle(s);
        say(s, "LEN %d > MAX %d", bars, m);
        s->rec_len = m;
        return;
    }
    s->rec_len = bars;
    if (s->rec_len == 0) say(s, "LEN FREE");
    else if (s->rec_len == 1) say(s, "LEN 1 BAR");
    else say(s, "LEN %d BARS", s->rec_len);
}

/* Frames of one bar (4 beats) at the current tempo. */
static double bar_frames(synth_t *s) { return (double)MARK_SR * 60.0 / cur_bpm(s) * 4.0; }

static int clip_state(int st) { return st == MK_PLAY || st == MK_DUB || st == MK_STOP; }

/* Whole bars the clip of track t is (within a small tolerance), 0 = not a whole
 * number of bars (a free length) or longer than the step row. */
static int clip_bars(synth_t *s, int t) {
    long len = get_t(s, t, "len");
    double b = bar_frames(s);
    long n = lround((double)len / b);
    if (len <= 0 || n < 1 || n > MAX_BARS || fabs((double)len - (double)n * b) > b / 64.0) return 0;
    return (int)n;
}

/* Max loop length the track buffers hold, in frames. */
static long cap_frames(synth_t *s) { return (long)get_i(s, "track_secs") * (long)MARK_SR; }
static int fit_bars(synth_t *s) { long n = (long)((double)cap_frames(s) / bar_frames(s) + 1e-6); return n < 1 ? 1 : n > MAX_BARS ? MAX_BARS : (int)n; }

/* Step button N (0-based). On the SELECTED track (last touched track pad):
 *  - with a clip: its length becomes N+1 bars (shorter = non-destructive trim,
 *    longer = the content repeats to fill), one undo level;
 *  - otherwise: the fixed length of the next recordings; the chosen one again = free. */
static void step_press(synth_t *s, int button) {
    if (button < 0 || button >= NSTEP_BTN) return;
    int t = s->cur_track, bars = button + 1;
    if (clip_state(core_state(s, t))) {
        long nl = lround(bar_frames(s) * bars), len = get_t(s, t, "len");
        if (nl > cap_frames(s)) { say(s, "LEN %d: TOO LONG", t); return; }
        if (labs(nl - len) > (long)(bar_frames(s) / 64.0)) {
            s->end_until = 0;                       /* a new undo entry */
            set_t(s, t, "setlen", (int)nl);
        }
        s->wave_dirty = 1;
        say(s, bars == 1 ? "LEN %d: 1 BAR" : "LEN %d: %d BARS", t, bars);
        return;
    }
    set_rec_bars(s, s->rec_len == bars ? 0 : bars);
}

/* Loop button + knob N (1-based): move the clip start of track N by `delta`
 * grid units (a 1/16 note at the current tempo; fine = 1/64 note). The start
 * is non-destructive: the engine plays buffer frames [start, len) only (the
 * audio left of the start never sounds) and dubs at the same place; the knob
 * clamps at 0 and at the end. */
static void sixteenths_label(synth_t *s, long off, char *out, size_t n) {
    double u = (double)MARK_SR * 60.0 / cur_bpm(s) / 4.0;
    long k = lround((double)off / u);
    if (fabs((double)off - (double)k * u) < 2.0) {
        if (k < 16) snprintf(out, n, "+%ld/16", k);
        else if (k % 16 == 0) snprintf(out, n, "+%ldB", k / 16);
        else snprintf(out, n, "+%ldB %ld/16", k / 16, k % 16);
    } else {
        snprintf(out, n, "+%ld MS", lround((double)off * 1000.0 / (double)MARK_SR));
    }
}

/* Readout of a PISTA knob: toast "T3 IN +8/16" and the display header suffix. */
static void pista_say(synth_t *s, int t, const char *name, const char *fmt, ...) {
    char v[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    snprintf(s->pista_lab, sizeof s->pista_lab, "%s %s", name, v);
    say(s, "T%d %s", t, s->pista_lab);
}

/* Zoom the display on the IN (pt 0) or OUT (pt 1) point of track t for ~2.5 s. */
static void zoom_on(synth_t *s, int t, int pt, int fine) {
    s->zoom_t = t; s->zoom_pt = pt; s->zoom_fine = fine;
    s->zoom_until = s->clock_frames + (long)MARK_SR * 5 / 2;
    s->wave_dirty = 1;
}

static void start_knob(synth_t *s, int n, int d, int fine, int pista) {
    int t = n;
    s->cur_track = t;
    s->wave_dirty = 1;
    int st = core_state(s, t);
    if (st == MK_EMPTY) { if (pista) pista_say(s, t, "IN", "EMPTY"); else say(s, "START %d EMPTY", t); return; }
    s->start_t = t;
    s->start_until = s->clock_frames + (long)MARK_SR * 5 / 2;
    if (st == MK_REC || st == MK_DUB) { if (pista) pista_say(s, t, "IN", "LOCKED"); else say(s, "START %d LOCKED", t); return; }
    long len = get_t(s, t, "len");
    if (len <= 0) return;
    d = clampi_(d, -16, 16);
    double u = (double)MARK_SR * 60.0 / cur_bpm(s) / (fine ? 16.0 : 4.0);
    long k = lround((double)get_t(s, t, "off") / u) + d;
    long off = lround((double)k * u);
    if (off > len - 256) off = len - 256;   /* clamp: the clip start never passes the end (the core keeps >= 1/16 note) */
    if (off < 0) off = 0;
    set_t(s, t, "off", (int)off);
    off = get_t(s, t, "off");               /* the core may clamp further */
    char lab[32];
    sixteenths_label(s, off, lab, sizeof lab);
    zoom_on(s, t, 0, fine);
    if (pista) pista_say(s, t, "IN", "%s", lab); else say(s, "START %d %s", t, lab);
}

static void loop_knob(synth_t *s, const char *val) {
    int n = 0, d = 0, fine = 0;
    if (sscanf(val, "%d:%d:%d", &n, &d, &fine) < 2) return;
    if (n < 1 || n > NTRK || d == 0) return;
    if (s->shift_held || s->perf_mode) return;        /* v0.14.4: PERFORM never moves IN / OUT */
    start_knob(s, n, d, fine, 0);
}

/* Readout of a loop length: "4 BARS", "3B 5/16", "5/16", or seconds off the grid. */
static void len_label(synth_t *s, long len, char *out, size_t n) {
    double u = (double)MARK_SR * 60.0 / cur_bpm(s) / 4.0;
    long k = lround((double)len / u);
    if (k > 0 && fabs((double)len - (double)k * u) < 2.0) {
        long bars = k / 16, r = k % 16;
        if (r == 0) snprintf(out, n, bars == 1 ? "%ld BAR" : "%ld BARS", bars);
        else if (bars == 0) snprintf(out, n, "%ld/16", r);
        else snprintf(out, n, "%ldB %ld/16", bars, r);
    } else {
        snprintf(out, n, "%.2f S", (double)len / (double)MARK_SR);
    }
}

/* PISTA K2 (OUT): moves the end of track N's loop by `d` grid steps (1/16
 * note; fine = 1/64). Non-destructive when shortening (the audio stays), the
 * content repeats when going past what was recorded. Never below one 1/16 note.
 * A burst of turns is one undo step. */
static void end_knob(synth_t *s, int t, int d, int fine) {
    int st = core_state(s, t);
    s->cur_track = t;
    s->wave_dirty = 1;
    if (!clip_state(st)) { pista_say(s, t, "OUT", st == MK_EMPTY ? "EMPTY" : "LOCKED"); return; }
    long len = get_t(s, t, "len");
    double u = (double)MARK_SR * 60.0 / cur_bpm(s) / (fine ? 16.0 : 4.0);
    double umin = (double)MARK_SR * 60.0 / cur_bpm(s) / 4.0;
    d = clampi_(d, -16, 16);
    long k = lround((double)len / u) + d;
    long nl = lround((double)k * u);
    /* OUT stops one 1/16 after IN: it never pushes the clip start. */
    long floor_ = get_t(s, t, "off") + (long)ceil(umin);
    if (nl < floor_) nl = floor_;
    if (nl > cap_frames(s)) nl = cap_frames(s);
    if (nl != len) {
        int co = s->end_t == t && s->clock_frames < s->end_until;
        char v[24]; snprintf(v, sizeof v, "%ld:%d", nl, co);
        char key[24]; snprintf(key, sizeof key, "t%d_setlen", t);
        mark_set_param(s->m, key, v);
        s->end_t = t;
        s->end_until = s->clock_frames + (long)MARK_SR * 3;
    }
    char lab[32];
    len_label(s, get_t(s, t, "len"), lab, sizeof lab);
    zoom_on(s, t, 1, fine);
    pista_say(s, t, "OUT", "%s", lab);
}

/* Speed readouts, indexed like the core's spd (1/4 1/2 3/4 1 1.5 2 4 x). */
static const char *const SPD_LABEL[MK_SPD_N] = { "x1/4", "x1/2", "x3/4", "x1", "x1.5", "x2", "x4" };

static const char *const FX_NAME[7] = { "OFF", "LPF", "HPF", "CRUSH", "DELAY", "PHASER", "RING" };

static void pan_label(int pan, char *out, size_t n) {
    if (pan == 50) snprintf(out, n, "C");
    else if (pan < 50) snprintf(out, n, "L%d", (50 - pan) * 2);
    else snprintf(out, n, "R%d", (pan - 50) * 2);
}

/* Track volume; a muted track keeps its level in saved_level until unmuted. */
static int track_level_get(synth_t *s, int t) { return s->muted[t - 1] ? s->saved_level[t - 1] : get_t(s, t, "level"); }
static void track_level_set(synth_t *s, int t, int v) {
    v = clampi_(v, 0, 200);
    if (s->muted[t - 1]) s->saved_level[t - 1] = v; else set_t(s, t, "level", v);
}

static void fx_set_type(synth_t *s, int t, int ty) {
    ty = clampi_(ty, 0, 6);
    set_t(s, t, "fx", ty);
    set_t(s, t, "fx_on", ty ? 1 : 0);
}

/* Enum knob: DETENTS clicks make one step; returns -1/0/+1. */
static int knob_step(synth_t *s, int n, int d) {
    if ((d > 0) != (s->knob_acc[n] > 0) && s->knob_acc[n] != 0) s->knob_acc[n] = 0;   /* turned back: restart */
    s->knob_acc[n] += d;
    if (s->knob_acc[n] >= DETENTS) { s->knob_acc[n] = 0; return 1; }
    if (s->knob_acc[n] <= -DETENTS) { s->knob_acc[n] = 0; return -1; }
    return 0;
}

static void pista_select(synth_t *s, int t) {
    s->sel = t;
    if (t) s->cur_track = t;
    s->last_knob = 0; s->pista_lab[0] = 0;
    memset(s->knob_acc, 0, sizeof s->knob_acc);
    s->wave_dirty = 1;
}

/* ---- PERFORM (perform.c): the 16 step buttons are the 16 effects --------
 * The target is frozen at the press: the selected track in PISTA, the whole mix in MIX
 * (an effect that is on keeps its own target when the selection changes).
 * PERFORM mode on: a tap (< 300 ms) switches the effect on and it stays, another tap
 * switches it off; held longer it is momentary (off on release). SHIFT held with the
 * mode off: always momentary. Up to 4 effects at once, run in a fixed chain order;
 * the newest is the current one (scene + knobs). Releasing SHIFT while the step is
 * still down keeps the effect. */
static void perf_step_down(synth_t *s, int button) {
    if (button < 0 || button >= PF_BANK || !s->perf) return;
    int fx = s->perf_bank * PF_BANK + button;
    s->down_fx[button] = -1;
    if (fx >= PF_NFX) return;                                  /* empty cell of the last page */
    if (fx == PF_TEMPO && s->sel == 0) { say(s, "TEMPO: PICK A TRACK"); return; }   /* it stretches one track's clip, not the MIX */
    s->down_fx[button] = fx;
    int r = perf_press(s->perf, fx, s->sel, s->clock_frames, s->perf_mode);
    if (r == PF_P_REFUSED) { if (perf_count(s->perf) >= PF_MAXSTACK) say(s, "PERFORM: MAX %d", PF_MAXSTACK); }
    else if (r == PF_P_OFF) say(s, "%s OFF", perf_name(fx));
    else if (s->perf_mode) say(s, "%s ON", perf_name(fx));
}
static void perf_step_up(synth_t *s, int button) {
    if (button < 0 || button >= PF_BANK || !s->perf || s->down_fx[button] < 0) return;
    perf_release(s->perf, s->down_fx[button], s->clock_frames);
    s->down_fx[button] = -1;
}
/* < / > (Movy forwards them as step_bank "+1" / "-1"): flip the PERFORM step-button page. */
static void perf_bank_step(synth_t *s, int dir) {
    int nb = s->perf_bank + (dir > 0 ? 1 : -1);
    if (nb < 0 || nb >= PF_NBANK) {                       /* no page that way: the arrow toggles the grid view */
        s->grid_view = !s->grid_view;
        say(s, s->grid_view ? "FX GRID %d/%d" : "FX SCENE", s->perf_bank + 1, PF_NBANK);
        return;
    }
    s->perf_bank = nb;
    s->grid_view = 1;                                     /* a flip always shows the grid of the new page */
    say(s, "FX PAGE %d/%d", nb + 1, PF_NBANK);
}
/* the knobs belong to the most recent effect while a step is down, or while SHIFT is held over a latched one */
static int perf_knob_fx(const synth_t *s) {
    int top = perf_top_fx(s->perf);
    return top >= 0 && (perf_held_count(s->perf) > 0 || s->shift_held || s->perf_mode) ? top : -1;
}
/* the PERFORM grid page / step buttons as effects: SHIFT held, or PERFORM mode on */
static int perf_page(const synth_t *s) { return s->shift_held || s->perf_mode; }

/* Movy's knob capture (knob_capture = 1 while PISTA is active): "N:delta[:fine]".
 * Knobs act on the selected track. No fine mode here (SHIFT is reserved for a
 * second page of knobs): IN/OUT always move on the 1/16 grid. Fine survives
 * only on Loop + knob (loop_knob). */
static void knob_delta(synth_t *s, const char *val) {
    int n = 0, d = 0, fine = 0;
    if (sscanf(val, "%d:%d:%d", &n, &d, &fine) < 2) return;
    int pfx = perf_knob_fx(s);
    if (pfx >= 0) {                               /* a PERFORM effect is held: the knobs are its parameters */
        if (n >= 1 && n <= PF_NKNOB && d != 0 && n <= perf_nparams(pfx)) {
            if (fine == 2 || s->arm_held) {            /* Delete (or ARM held) + knob: wipe that knob's automation (every track) */
                if (s->arm_held) s->arm_used = 1;
                if (perf_auto_has(s->perf, -1, pfx, n - 1)) { perf_auto_clear(s->perf, -1, pfx, n - 1); say(s, "AUTO CLR %s %s", perf_short(pfx), perf_plabel(pfx, n - 1)); }
                else say(s, "NO AUTO %s %s", perf_short(pfx), perf_plabel(pfx, n - 1));
                return;
            }
            s->grid_view = 0;                          /* turning a knob goes back to the scene of the effect */
            int before = perf_pct(s->perf, pfx, n - 1);
            perf_knob_ex(s->perf, pfx, n - 1, d, s->auto_hold != 0);   /* 0.5 % clicks while the REC pad is held */
            if (perf_knob_steps(pfx, n - 1) && perf_pct(s->perf, pfx, n - 1) != before) {   /* a stepped knob names the option it landed on */
                char tx[16]; perf_ptext(s->perf, pfx, n - 1, tx, sizeof tx);
                say(s, "%s %s", perf_plabel(pfx, n - 1), tx);
            }
            if (pfx == PF_TEMPO && n == 1)                                  /* K1 is the target BPM */
                say(s, "TEMPO %d BPM (%d%%)", (int)lround(perf_pval(s->perf, pfx, 0) * cur_bpm(s)), (int)lround(perf_pval(s->perf, pfx, 0) * 100.0));
            if (s->auto_hold) { s->auto_turned = 1; say(s, "AUTO %d %s %s", s->auto_hold, perf_short(pfx), perf_plabel(pfx, n - 1)); }
        }
        return;
    }
    if (perf_page(s)) return;                      /* v0.14.4: PERFORM mode / page showing and no effect to steer: the knobs do nothing to the clip */
    if (n < 1 || n > 8 || d == 0 || s->sel == 0) return;
    int t = s->sel;
    fine = 0;
    if (n != s->last_knob) s->wave_dirty = 1;
    s->last_knob = n;
    s->focus_until = s->clock_frames + (long)MARK_SR * 3 / 2;
    s->cur_track = t;
    switch (n) {
    case 1: start_knob(s, t, d, fine, 1); break;
    case 2: end_knob(s, t, d, fine); break;
    case 3: { int v = clampi_(track_level_get(s, t) + d * 5, 0, 200); track_level_set(s, t, v); pista_say(s, t, "VOL", "%d", v); break; }
    case 4: {
        int p = clampi_(get_t(s, t, "pan") + d * 2, 0, 100);
        set_t(s, t, "pan", p);
        char lab[8]; pan_label(p, lab, sizeof lab);
        pista_say(s, t, "PAN", "%s", lab);
        break;
    }
    case 5: {
        int k = knob_step(s, n, d);
        int ty = get_t(s, t, "fx_on") ? clampi_(get_t(s, t, "fx"), 0, 6) : 0;   /* FX off reads as OFF */
        if (k) { ty = clampi_(ty + k, 0, 6); fx_set_type(s, t, ty); }
        pista_say(s, t, "FX", "%s", FX_NAME[ty]);
        break;
    }
    case 6: {
        int a = clampi_(get_t(s, t, "fxp") + d * 2, 0, 100);
        set_t(s, t, "fxp", a);
        pista_say(s, t, "AMT", "%d", a);
        break;
    }
    case 7: {
        int k = knob_step(s, n, d);
        if (k) set_t(s, t, "spd", clampi_(get_t(s, t, "spd") + k, 0, MK_SPD_N - 1));
        pista_say(s, t, "SPD", "%s", SPD_LABEL[clampi_(get_t(s, t, "spd"), 0, MK_SPD_N - 1)]);
        break;
    }
    default: {
        int k = knob_step(s, n, d);
        if (k) set_t(s, t, "rev", k > 0 ? 1 : 0);
        pista_say(s, t, "REV", "%s", get_t(s, t, "rev") ? "ON" : "OFF");
        break;
    }
    }
}

/* Per-track settings back to what a fresh module has; the audio is cleared
 * separately. With save, snapshot the previous settings for RESET's undo. */
static void reset_settings(synth_t *s, int t, int save) {
    int i = t - 1;
    if (save) {
        char b[64] = "";
        s->rsnap.valid = 1; s->rsnap.track = t;
        for (int k = 1; k <= NTRK; k++) {
            s->rsnap.lvl[k - 1] = get_t(s, k, "level");
            s->rsnap.muted[k - 1] = s->muted[k - 1];
            s->rsnap.saved_level[k - 1] = s->saved_level[k - 1];
            s->rsnap.solo_saved[k - 1] = s->solo_saved[k - 1];
        }
        s->rsnap.solo = s->solo;
        s->rsnap.pan = get_t(s, t, "pan"); s->rsnap.fx = get_t(s, t, "fx");
        s->rsnap.fx_on = get_t(s, t, "fx_on"); s->rsnap.fxp = get_t(s, t, "fxp");
        s->rsnap.spd = get_t(s, t, "spd"); s->rsnap.rev = get_t(s, t, "rev");
        s->rsnap.shot = get_t(s, t, "shot");
        char key[24]; snprintf(key, sizeof key, "t%d_fx_module", t);
        if (mark_get_param(s->m, key, b, sizeof b) <= 0) b[0] = 0;
        snprintf(s->rsnap.fx_module, sizeof s->rsnap.fx_module, "%s", b);
    }
    if (s->solo == t) {                    /* the soloed track leaves: the others get their old mutes back */
        for (int k = 1; k <= NTRK; k++) if (k != t) set_mute(s, k, s->solo_saved[k - 1]);
        s->solo = 0;
    }
    if (s->solo) s->solo_saved[i] = 0;     /* soloing another track: no longer muted when that ends */
    set_mute(s, t, 0);
    s->solo_saved[i] = 0;
    set_t(s, t, "level", 100);
    set_t(s, t, "pan", 50);
    set_t(s, t, "fx", 1);                  /* LPF, the module default once switched on */
    set_t(s, t, "fx_on", 0);
    set_t(s, t, "fxp", 50);
    set_t(s, t, "spd", MK_SPD_1X);
    set_t(s, t, "rev", 0);
    set_t(s, t, "shot", 0);
    char key[24]; snprintf(key, sizeof key, "t%d_fx_module", t);
    mark_set_param(s->m, key, "");
}

static void restore_settings(synth_t *s) {
    int t = s->rsnap.track;
    for (int k = 1; k <= NTRK; k++) {
        s->muted[k - 1] = s->rsnap.muted[k - 1];
        s->saved_level[k - 1] = s->rsnap.saved_level[k - 1];
        s->solo_saved[k - 1] = s->rsnap.solo_saved[k - 1];
        set_t(s, k, "level", s->rsnap.lvl[k - 1]);
    }
    s->solo = s->rsnap.solo;
    set_t(s, t, "pan", s->rsnap.pan);
    set_t(s, t, "fx", s->rsnap.fx);
    set_t(s, t, "fx_on", s->rsnap.fx_on);
    set_t(s, t, "fxp", s->rsnap.fxp);
    set_t(s, t, "spd", s->rsnap.spd);
    set_t(s, t, "rev", s->rsnap.rev);
    set_t(s, t, "shot", s->rsnap.shot);
    if (s->rsnap.fx_module[0]) {
        char key[24]; snprintf(key, sizeof key, "t%d_fx_module", t);
        mark_set_param(s->m, key, s->rsnap.fx_module);
    }
}

/* Movy's Undo / Shift+Undo (undo_param). One level: undo only when there is
 * something to undo, redo only right after an undo. */
static void undo_cmd(synth_t *s, const char *val) {
    int av = get_i(s, "undo_avail");
    int redo = !strcmp(val, "redo");
    if (!redo && strcmp(val, "undo")) return;
    s->wave_dirty = 1;
    if (av != (redo ? 2 : 1)) { say(s, redo ? "NOTHING TO REDO" : "NOTHING TO UNDO"); return; }
    int owned = 0;                         /* this undo entry is the CLEAR made by a RESET */
    if (s->rsnap.valid) {
        char b[32] = ""; int tr = 0, kind = 0, rd = 0;
        if (mark_get_param(s->m, "undo_info", b, sizeof b) > 0 && sscanf(b, "%d:%d:%d", &tr, &kind, &rd) == 3
            && tr == s->rsnap.track && kind == 2) owned = 1;
        else s->rsnap.valid = 0;           /* superseded by a newer undo entry */
    }
    say(s, redo ? "REDO" : "UNDO");
    trig(s, "undo");
    if (owned) {
        if (redo) reset_settings(s, s->rsnap.track, 0);
        else      restore_settings(s);
    }
}

static void rec_pad(synth_t *s, int t) {
    int i = t - 1;
    if (s->armed[i])        { s->armed[i] = 0; say(s, "CANCEL %d", t); return; }
    if (s->countin[i] >= 0) { s->countin[i] = -1; say(s, "CANCEL %d", t); return; }
    if (core_state(s, t) == MK_EMPTY && !core_pending(s, t)) {
        if (s->arm) { s->armed[i] = 1; say(s, "ARMED %d", t); return; }
        if (s->count_in) {
            s->countin[i] = (long)((double)MARK_SR * 60.0 / cur_bpm(s) * 4.0 * s->count_bars);
            say(s, "COUNT-IN %d", t);
            return;
        }
    }
    s->rec_fired[i] = 0;
    if (core_state(s, t) == MK_STOP && !core_pending(s, t)) {
        s->dub_after[i] = 1;               /* stopped: start playing straight into DUB */
        say(s, "DUB %d", t);
    } else {
        s->dub_after[i] = 0;
        say_rec(s, t);
    }
    trig_t(s, t, "btn");
}

static void arm_reverse(synth_t *s, int t) {
    toggle_t(s, t, "rev");
    say(s, get_t(s, t, "rev") ? "REV %d ON" : "REV %d OFF", t);
}

/* SOLO: mute every other track; again on the same track restores the mutes
 * it found. Switching to another track keeps the original saved mutes. */
static void solo_toggle(synth_t *s, int t) {
    if (s->solo == t) {
        for (int k = 1; k <= NTRK; k++) set_mute(s, k, s->solo_saved[k - 1]);
        s->solo = 0;
        say(s, "UNSOLO %d", t);
        return;
    }
    if (!s->solo) for (int k = 0; k < NTRK; k++) s->solo_saved[k] = s->muted[k];
    s->solo = t;
    for (int k = 1; k <= NTRK; k++) set_mute(s, k, k != t);
    say(s, "SOLO %d", t);
}

/* SHIFT + column 8: secondary settings (row 1 = MONITOR). Row 2 is free for now. */
static void shift_setting(synth_t *s, int act) {
    if (act == A_COUNTIN) {
        s->count_bars = s->count_bars == 1 ? 2 : 1;
        say(s, s->count_bars == 1 ? "COUNT-IN 1 BAR" : "COUNT-IN 2 BARS");
    } else if (act == A_RECACT) {
        int next = THR_CYCLE[0];
        for (int i = 0; i < 5; i++) if (THR_CYCLE[i] > s->threshold) { next = THR_CYCLE[i]; break; }
        s->threshold = next;
        say(s, "THR %d DB", next);
    } else if (act == A_QUANT) {
        int on = !get_i(s, "monitor");
        set_i(s, "monitor", on);
        say(s, on ? "MONITOR ON" : "MONITOR OFF");
    }
}

/* PERFORM automation hold: REC pad of a PLAYING track while PERFORM mode has an effect on. */
static int auto_hold_ok(synth_t *s, int t) {
    int st = core_state(s, t);
    return s->perf_mode && !s->shift_held && perf_top_fx(s->perf) >= 0 && (st == MK_PLAY || st == MK_DUB);
}
static void auto_hold_end(synth_t *s, int t_rec_tap) {
    int t = s->auto_hold;
    if (!t) return;
    s->auto_hold = 0;
    perf_auto_hold(s->perf, 0);
    if (!s->auto_turned && s->clock_frames - s->auto_hold_at < SHIFT_TAP_FRAMES && t_rec_tap) rec_pad(s, t);   /* a plain tap keeps its normal REC action */
    else if (s->auto_turned) say(s, "AUTO %d SAVED", t);
}

/* Native Move SHIFT + a pad (module declares shift_pad_param; Movy forwards the rack pad index 0..31, the same value as delete_pad;
 * the log "note=88 rack=25" is v25 = idx 24 = row 3, column 5 = the MUTE pad of track 4). Row 4 selector: CLEAR ALL EFFECTS of that track and leave the clip as recorded. Audio, volume, pan and mute are untouched. */
static void fx_clear_track(synth_t *s, int t) {
    if (s->perf) { perf_clear_target(s->perf, t); perf_auto_clear(s->perf, t, -1, -1); }
    set_t(s, t, "fx_on", 0);
    set_t(s, t, "fx", 0);
    set_t(s, t, "rev", 0);
    set_t(s, t, "spd", MK_SPD_1X);
    s->wave_dirty = 1;
    say(s, "FX CLR %d", t);
}
static void shift_pad(synth_t *s, int idx) {
    const pad_def_t *p = pad_for_note(NOTE_BASE + idx);
    if (!p || p->arg < 1 || p->arg > NTRK) return;
    if (p->act == A_SEL) fx_clear_track(s, p->arg);          /* row 4: FX CLR */
    else if (p->act == A_MUTE) solo_toggle(s, p->arg);       /* row 3: SOLO (again = the previous mutes come back) */
}

static void pad_down(synth_t *s, const pad_def_t *p) {
    int t = p->arg;
    int sh = s->shift_held;
    s->wave_dirty = 1;
    if (sh && p->act != A_SHIFT) s->shift_used = 1;            /* SHIFT was a modifier, not a tap */
    if (t >= 1 && t <= NTRK && !s->sel) s->cur_track = t;     /* PISTA keeps the waveform on its track */
    if (sh && (p->act == A_COUNTIN || p->act == A_RECACT || p->act == A_FIXLEN || p->act == A_QUANT)) {
        shift_setting(s, p->act);
        return;
    }
    if (sh && p->act == A_REC)  { arm_reverse(s, t); return; }
    if (sh && p->act == A_MUTE) { solo_toggle(s, t); return; }
    switch (p->act) {
    case A_REC:                            /* row 1: REC / DUB (ARM held = reverse) */
        if (s->arm_held) { s->arm_used = 1; arm_reverse(s, t); break; }
        if (auto_hold_ok(s, t)) {          /* PERFORM + an effect on + a playing track: HOLD = write knob automation, no audio action */
            s->auto_hold = t; s->auto_hold_at = s->clock_frames; s->auto_turned = 0;
            perf_auto_hold(s->perf, t);
            say(s, "AUTO-REC %d", t);
            break;
        }
        rec_pad(s, t);
        break;
    case A_STOP: {                         /* row 2: PLAY/STOP toggle */
        int st = core_state(s, t);
        if (s->arm_held) { s->arm_used = 1; arm_reverse(s, t); break; }
        clear_flags(s, t);
        if (st == MK_EMPTY) break;
        if (st == MK_STOP) { say(s, "PLAY %d", t); trig_t(s, t, "btn"); }
        else               { say(s, "STOP %d", t); trig_t(s, t, "stop"); }
        break;
    }
    case A_MUTE:
        s->solo = 0;                       /* a manual mute ends the solo */
        set_mute(s, t, !s->muted[t - 1]);
        say(s, s->muted[t - 1] ? "MUTE %d" : "UNMUTE %d", t);
        break;
    case A_SEL:
        if (sh) {                          /* looper SHIFT + select = FX CLR (v0.14; the module-only twin of Move's Shift + selector) */
            fx_clear_track(s, t);
        } else if (s->sel == t) {
            pista_select(s, 0);
            say(s, "MIX");
        } else {
            pista_select(s, t);
            say(s, "PISTA %d", t);
        }
        break;
    case A_ALL: {
        int any = 0;
        for (int k = 1; k <= NTRK; k++) {
            int st = core_state(s, k);
            if (st == MK_REC || st == MK_PLAY || st == MK_DUB) any = 1;
        }
        say(s, any ? "ALL STOP" : "ALL PLAY");
        trig(s, "all_btn");
        break;
    }
    case A_STOPALL:
        say(s, "STOP ALL");
        for (int k = 1; k <= NTRK; k++) { clear_flags(s, k); trig_t(s, k, "stop"); }
        break;
    case A_SHIFT:   s->shift_held = 1; s->shift_used = 0; s->shift_at = s->clock_frames; break;
    case A_ARM:
        s->arm_held = 1; s->arm_used = 0;      /* toggles on release unless used as modifier */
        break;
    case A_COUNTIN:
        s->count_in = !s->count_in;
        say(s, s->count_in ? "COUNT-IN ON" : "COUNT-IN OFF");
        break;
    case A_RECACT:
        set_i(s, "rec_action", !get_i(s, "rec_action"));
        say(s, get_i(s, "rec_action") ? "REC>DUB" : "REC>PLAY");
        break;
    case A_FIXLEN: {                       /* cycle free -> 1 -> 2 -> 4 -> 8 -> 16 -> 32 -> free (clamped to what the buffer holds) */
        int next = 0, fit = fit_bars(s), fc = fit_cycle(s);
        for (int i = 0; i < NCYCLE; i++) if (CYCLE_BARS[i] > s->rec_len) { next = CYCLE_BARS[i]; break; }
        if (next > fit && s->rec_len >= fc) next = 0;             /* already at the largest that fits: back to free, never stuck */
        set_rec_bars(s, next);
        break;
    }
    case A_QUANT:
        set_i(s, "quantize", !get_i(s, "quantize"));
        say(s, get_i(s, "quantize") ? "QUANT ON" : "QUANT OFF");
        break;
    default: break;
    }
}

/* Gestures act on note-on; CLEAR comes from Movy's Delete + pad (delete_pad). */
/* Movy's Delete + pad (module declares delete_pad_param): CLEAR the track in
 * that pad's column, whatever row was tapped. Pad index 0..31 = rack pad. */
static void clear_track(synth_t *s, int t) {
    s->cur_track = t;
    s->wave_dirty = 1;
    clear_flags(s, t);
    s->rsnap.valid = 0;
    say(s, "CLEAR %d", t);
    trig_t(s, t, "clear");
}

/* Delete + the track's PISTA selector (top row): CLEAR plus every per-track
 * setting back to a fresh module's. Undo brings both back (while the core's
 * undo entry is still this CLEAR; an empty track has no audio undo, so no snapshot). */
static void reset_track(synth_t *s, int t) {
    s->cur_track = t;
    s->wave_dirty = 1;
    clear_flags(s, t);
    s->rsnap.valid = 0;
    if (s->perf) { perf_clear_target(s->perf, t); perf_auto_clear(s->perf, t, -1, -1); }       /* every PERFORM effect bound to this track goes off too */
    trig_t(s, t, "clear");
    {
        char b[32] = ""; int tr = 0, kind = 0, rd = 0;
        int own = mark_get_param(s->m, "undo_info", b, sizeof b) > 0 && sscanf(b, "%d:%d:%d", &tr, &kind, &rd) == 3
                  && tr == t && kind == 2 && !rd;
        reset_settings(s, t, own);
    }
    say(s, "RESET %d", t);
}

/* Delete + REC pad: erase the last recording. If this track's latest action is an overdub
 * pass the core still holds (one level), only that layer goes (an Undo of the dub);
 * otherwise the clip audio is cleared as with the other rows. */
static void erase_last(synth_t *s, int t) {
    char b[32] = ""; int tr = 0, kind = 0, rd = 0;
    if (get_i(s, "undo_avail") == 1 && core_state(s, t) != MK_DUB
        && mark_get_param(s->m, "undo_info", b, sizeof b) > 0 && sscanf(b, "%d:%d:%d", &tr, &kind, &rd) == 3
        && tr == t && kind == 0 && !rd) {
        s->cur_track = t;
        undo_cmd(s, "undo");
        if (get_i(s, "undo_avail") != 1) { say(s, "UNDO DUB %d", t); return; }   /* otherwise the dub held nothing: clear */
    }
    clear_track(s, t);
}

/* v0.14.2: Delete + selector = every setting of the track back to its factory value, EXCEPT the audio and IN / OUT:
 * FX off + default amount, pan centre, volume 100, speed 1x, reverse off, mute off, one-shot off, its PERFORM effects
 * (latched and held) off and its automation cleared. The old full RESET (looper SHIFT + Delete + selector) also clears audio. */
static void reset_fx_track(synth_t *s, int t) {
    if (s->perf) { perf_clear_target(s->perf, t); perf_auto_clear(s->perf, t, -1, -1); }
    for (int b = 0; b < PF_BANK; b++) if (s->down_fx[b] >= 0 && s->perf && !(perf_on_mask(s->perf) >> s->down_fx[b] & 1u)) s->down_fx[b] = -1;
    s->grid_view = 0;
    reset_settings(s, t, 0);
    set_t(s, t, "fx", 0);
    s->wave_dirty = 1;
    say(s, "RESET FX %d", t);
}

static void delete_pad(synth_t *s, int idx) {
    const pad_def_t *p = pad_for_note(NOTE_BASE + idx);
    if (!p || p->arg < 1 || p->arg > NTRK) return;
    /* v0.12: Delete + selector clears ONLY that track's audio (like Delete + any other pad of its column).
     * The full RESET (audio + level/pan/FX/mute + its PERFORM effects) moved to SHIFT + Delete + selector. */
    /* v0.14: Delete + selector now only wipes the track's knob automation. */
    if (p->act == A_SEL && s->shift_held) { s->shift_used = 1; reset_track(s, p->arg); }
    else if (p->act == A_SEL) {                                  /* v0.14: Delete + selector = wipe that track's knob automation (audio stays) */
        reset_fx_track(s, p->arg);
    }
    else if (p->act == A_REC) erase_last(s, p->arg);
    else clear_track(s, p->arg);
}

static void arm_toggle(synth_t *s) {
    s->arm = !s->arm;
    if (!s->arm) for (int k = 1; k <= NTRK; k++) s->armed[k - 1] = 0;
    say(s, s->arm ? "ARM ON" : "ARM OFF");
}

static void pad_up(synth_t *s, const pad_def_t *p) {
    if (p->act == A_REC && s->auto_hold == p->arg) { auto_hold_end(s, 1); return; }
    if (p->act == A_SHIFT) {
        int tap = s->shift_held && !s->shift_used && s->clock_frames - s->shift_at < SHIFT_TAP_FRAMES;
        s->shift_held = 0; s->shift_used = 0;
        if (tap) {                                  /* a short tap toggles PERFORM mode */
            s->perf_mode = !s->perf_mode;
            say(s, s->perf_mode ? "PERFORM ON" : "PERFORM OFF");
        }
        return;
    }
    if (p->act != A_ARM || !s->arm_held) return;
    s->arm_held = 0;
    if (s->arm_used) { s->arm_used = 0; return; }   /* was only the modifier */
    arm_toggle(s);
}

/* ---- pedal_param: Blackstar Live Logic (CUSTOM mode), "<n>:d" / "<n>:u" ----
 * Movy forwards the six switches; every action reuses the pad / undo / clear
 * paths above. Hold timing runs in process (clock_frames), 1 s = MARK_SR.
 * 1 REC/DUB/PLAY on the selected track   2 PLAY/STOP (hold: CLEAR)
 * 3 previous track (hold: ARM toggle)    4 next track
 * 5 UNDO                                 6 STOP ALL if playing, else ALL START
 * Switches 2 and 3 have a hold, so their tap fires on release (< 1 s). */
static void pedal_pad(synth_t *s, int act, int arg) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++)
            if (PAD_GRID[r][c].act == act && PAD_GRID[r][c].arg == arg) { pad_down(s, &PAD_GRID[r][c]); return; }
}

static void pedal_step(synth_t *s, int dir) {
    s->cur_track = (s->cur_track - 1 + dir + NTRK) % NTRK + 1;
    if (s->sel) pista_select(s, s->cur_track);               /* PISTA follows the pedal */
    s->wave_dirty = 1;
    say(s, "TRACK %d", s->cur_track);
}

static void pedal_set(synth_t *s, const char *val) {
    int n = val[0] - '0';
    if (n < 1 || n > 6 || val[1] != ':' || (val[2] != 'd' && val[2] != 'u')) return;
    if (val[2] == 'd') {
        s->pedal_held[n] = 1; s->pedal_fired[n] = 0; s->pedal_at[n] = s->clock_frames;
        switch (n) {
        case 1: pedal_pad(s, A_REC, s->cur_track); break;
        case 4: pedal_step(s, 1); break;
        case 5: undo_cmd(s, "undo"); break;
        case 6: pedal_pad(s, A_ALL, 0); break;
        default: break;
        }
        return;
    }
    if (!s->pedal_held[n]) return;
    s->pedal_held[n] = 0;
    if (s->pedal_fired[n]) return;
    if (n == 2) pedal_pad(s, A_STOP, s->cur_track);
    else if (n == 3) pedal_step(s, -1);
}

static void pedal_tick(synth_t *s) {
    for (int n = 2; n <= 3; n++) {
        if (!s->pedal_held[n] || s->pedal_fired[n] || s->clock_frames - s->pedal_at[n] < MARK_SR) continue;
        s->pedal_fired[n] = 1;
        s->wave_dirty = 1;
        if (n == 2) clear_track(s, s->cur_track); else arm_toggle(s);
    }
}

/* ------------------------------------------------------------------ */
/*  pad_leds: colours for Movy's drum grid (read-only get_param)        */
/* ------------------------------------------------------------------ */
/*
 * 64 hex digits = one byte per rack pad (pad 1 first, the order of the
 * v01..v32 voices). Bits 0-6 = Move LED palette index, bit 7 = blink.
 * Movy polls this only while this track is the visible one.
 */
#define LED_BLINK 0x80
enum {
    L_OFF = 0, L_RED = 127, L_ORANGE = 3, L_YELLOW = 7,
    L_BRIGHT_YELLOW = 8, L_CYAN = 14, L_PURPLE = 20,
    L_DIM_YELLOW = 74, L_DIM_CYAN = 92,
    L_ROYAL_BLUE = 17, L_DARK_ROYAL_BLUE = 97, L_GREEN_PLAY = 126,
    L_LIGHT_GREY = 118, L_DARK_GREY = 119, L_WHITE = 120, L_OFF_WHITE = 122,
    L_GREY_40 = 123
};
/* Colour rules (RC-505 style): GREEN = playing and RED = recording are
 * reserved for state, so no track identity colour is green or red.
 * Track identity: blue, cyan, yellow, lime (track 4, palette 9), light magenta/pink (track 5, palette 26), orange.
 * ARM / armed = orange blinking; DUB = red like REC (v0.12); mute = grey. Column 8 = white. */
static const uint8_t TRK_BRIGHT[NTRK] = { 125,  14,   7,   9,  26,   3 };
static const uint8_t TRK_DIM[NTRK]    = {  99,  91,  77,  81, 115,  72 };
static const uint8_t TRK_VDIM[NTRK]   = {  98,  92,  74,  82, 116,  69 };
/* One read of the core state per poll, shared by all 32 pads (each core
 * read is a string round trip, so it must not be repeated per pad). */
typedef struct {
    int st[NTRK], pend[NTRK], fx_on[NTRK];
    int any_run, quant, rec_action;
} snap_t;

static void take_snap(synth_t *s, snap_t *n) {
    n->any_run = 0;
    for (int t = 1; t <= NTRK; t++) {
        int st = core_state(s, t);
        n->st[t - 1]   = st;
        n->pend[t - 1] = core_pending(s, t);
        n->fx_on[t - 1] = get_t(s, t, "fx_on");
        if (st == MK_REC || st == MK_PLAY || st == MK_DUB) n->any_run = 1;
    }
    n->quant = get_i(s, "quantize");
    n->rec_action = get_i(s, "rec_action");
}

static uint8_t pad_color(synth_t *s, const snap_t *n, const pad_def_t *d) {
    int t = d->arg, i = t - 1;
    switch (d->act) {
    case A_REC: {                          /* row 1: rec red / dub violet / empty dim */
        int st = n->st[i];
        if (s->armed[i])        return LED_BLINK | L_ORANGE;
        if (s->countin[i] >= 0) return LED_BLINK | L_BRIGHT_YELLOW;
        if (st == MK_EMPTY)     return n->pend[i] ? (LED_BLINK | L_RED) : TRK_VDIM[i];  /* waiting on the grid: red blink, armed = orange */
        switch (st) {
        case MK_REC:  return L_RED;
        case MK_DUB:  return L_RED;           /* overdub is red like REC (v0.12; it was violet) */
        default:      return TRK_VDIM[i];  /* playing / stopped: row 2 shows it */
        }
    }
    case A_STOP: {                         /* row 2: play green / stopped track colour / empty off */
        int st = n->st[i];
        if (s->arm_held && st != MK_EMPTY)   /* ARM held: show reverse state */
            return get_t(s, t, "rev") ? L_WHITE : TRK_BRIGHT[i];
        if (st == MK_EMPTY) return L_OFF;
        if (s->muted[i])    return L_DARK_GREY;
        switch (st) {
        case MK_PLAY: case MK_DUB: return L_GREEN_PLAY;
        case MK_REC:  return TRK_VDIM[i];
        default:      return TRK_DIM[i];   /* stopped with a clip */
        }
    }
    case A_MUTE:
        if (s->solo == t) return LED_BLINK | TRK_BRIGHT[i];
        if (s->muted[i]) return L_DARK_GREY;
        return n->st[i] == MK_EMPTY ? TRK_VDIM[i] : TRK_BRIGHT[i];
    case A_SEL:                            /* selector: identity colour, selected = bright; SHIFT held shows FX state */
        if (s->shift_held) return n->fx_on[i] ? L_WHITE : TRK_VDIM[i];
        return s->sel == t ? TRK_BRIGHT[i] : TRK_VDIM[i];
    case A_ALL:     return n->any_run ? L_WHITE : L_GREY_40;
    case A_STOPALL: return n->any_run ? L_BRIGHT_YELLOW : L_OFF;   /* idle = unlit */
    case A_SHIFT:   return s->shift_held ? L_WHITE : s->perf_mode ? (LED_BLINK | L_WHITE) : L_GREY_40;
    case A_ARM:     return s->arm ? (LED_BLINK | L_ORANGE) : L_DARK_GREY;
    /* column 8 (REC SETTINGS): all white; bright = on / not the default, dim = off / default */
    case A_COUNTIN: return s->count_in ? L_WHITE : L_GREY_40;
    case A_RECACT:  return n->rec_action ? L_WHITE : L_GREY_40;
    case A_FIXLEN:  return s->rec_len > 0 ? L_WHITE : L_GREY_40;
    case A_QUANT:   return n->quant ? L_WHITE : L_GREY_40;
    default:        return L_OFF;
    }
}

static void build_pad_leds(synth_t *s, char *out) {
    snap_t n;
    take_snap(s, &n);
    for (int v = 0; v < 32; v++) {
        int row_b = (v % 16) / 4, col = (v % 4) + 4 * (v / 16);
        snprintf(out + v * 2, 3, "%02X", pad_color(s, &n, &PAD_GRID[3 - row_b][col]));
    }
}

/* step_leds: the 16 step buttons show the fixed recording length. Free = all
 * dark; N bars = steps 1..N-1 dim, step N white, the rest dark. 32 hex digits. */
static void build_step_leds(synth_t *s, char *out) {
    /* family colours: repeat blue, tape orange, tone/filter violet, rhythm cyan, space teal, kill yellow */
    static const uint8_t FAM[7] = { L_ROYAL_BLUE, L_ORANGE, L_PURPLE, L_CYAN, L_DIM_CYAN, L_YELLOW, 9 /* modulation: bright lime */ };
    unsigned latched = perf_latch_mask(s->perf), on = perf_on_mask(s->perf);
    if (perf_page(s) || perf_held_count(s->perf) > 0) {      /* PERFORM page: family colours, a held one white, a latched one blinking */
        for (int j = 0; j < PF_BANK; j++) {
            int i = s->perf_bank * PF_BANK + j, c = L_OFF;
            if (i < PF_NFX) {
                c = FAM[perf_family(i)];
                if ((latched >> i) & 1u) c |= LED_BLINK;
                if (((on >> i) & 1u) && !((latched >> i) & 1u)) c = L_WHITE;
            }
            snprintf(out + j * 2, 3, "%02X", c);
        }
        return;
    }
    int bars = s->rec_len;                       /* next recording... */
    if (clip_state(core_state(s, s->cur_track))) bars = clip_bars(s, s->cur_track);   /* ...or the selected clip */
    for (int i = 0; i < 16; i++) {
        int c = L_DARK_GREY;
        if (bars > 0) {
            if (i + 1 == bars) c = L_WHITE;
            else if (i + 1 < bars) c = L_GREY_40;
        }
        { int fi = s->perf_bank * PF_BANK + i;
          if (fi < PF_NFX && ((latched >> fi) & 1u)) c = FAM[perf_family(fi)] | LED_BLINK; }     /* a latched effect stays visible without SHIFT */
        snprintf(out + i * 2, 3, "%02X", c);
    }
}

/* ------------------------------------------------------------------ */
/*  display_param: waveform overview of the selected track              */
/* ------------------------------------------------------------------ */
/*
 * get_param display = "<seq>|<pos>|<peaks>|<header>"
 *   seq     bumps whenever the peaks/length change (Movy re-parses peaks then)
 *   pos     playhead 0..127
 *   peaks   128 x 2 hex digits (0..255), empty when the track has no audio
 *   header  free text, last so it may hold anything: "PLAY 1 - 4 BARS"
 * Peaks are cached; only the playhead is read on every call.
 */
static void header_text(synth_t *s, int t, uint32_t span, int zoomed, char *out, size_t n) {
    static const char *const NAME[7] = { "EMPTY", "ARMED", "REC", "PLAY", "DUB", "STOP", "MUTE" };
    int st = shim_state(s, t);
    const char *zs = zoomed ? " ZOOM" : "";
    if (!s->sel && s->start_t == t && s->clock_frames < s->start_until && span) {   /* Loop + knob readout */
        char lab[32];
        if (st == S_REC || st == S_DUB) snprintf(out, n, "START %d LOCKED", t);
        else { sixteenths_label(s, get_t(s, t, "off"), lab, sizeof lab); snprintf(out, n, "START %d %s%s", t, lab, zs); }
        return;
    }
    if (s->sel) {                              /* PISTA view: "PISTA 3 IN +3/16 ZOOM" */
        if (s->pista_lab[0]) snprintf(out, n, "PISTA %d %s%s", t, s->pista_lab, zs);
        else snprintf(out, n, "PISTA %d %s", t, NAME[st]);
        return;
    }
    if (span == 0) { snprintf(out, n, "%s %d", NAME[st], t); return; }
    double beat = (double)MARK_SR * 60.0 / cur_bpm(s);
    double bars = (double)span / (beat * 4.0);
    double r = (double)(long)(bars + 0.5);
    if (st != S_REC && r >= 1.0 && fabs(bars - r) < 0.03 * r)
        snprintf(out, n, "%s %d - %d %s", NAME[st], t, (int)r, (int)r == 1 ? "BAR" : "BARS");
    else
        snprintf(out, n, "%s %d - %.1f S", NAME[st], t, (double)span / (double)MARK_SR);
}

/* Bar / beat ticks over the view window [lo, hi) of the buffer (4/4 at the
 * current tempo), counted from `origin` (the clip start, 0 while recording) so
 * bar 1 begins there; bin = (frame - lo) * 128 / (hi - lo). Returns the tick
 * count. Beat ticks need >= 4 px per beat; else bars only (>= 3 px per bar);
 * else every 4th bar if that is >= 3 px apart; else none. Every 4th bar (bars
 * 1, 5, 9... = beats k % 16 == 0, also before the origin) is a phrase tick,
 * written to `ph` (count in *nph) and not repeated in `out`. */
static int build_ticks(synth_t *s, double origin, uint32_t lo, uint32_t hi, uint8_t *out, uint8_t *ph, int *nph) {
    *nph = 0;
    if (hi <= lo) return 0;
    double span = (double)(hi - lo);
    double beat = (double)MARK_SR * 60.0 / cur_bpm(s);
    if (!(beat > 0.0)) return 0;
    double px_beat = beat * WAVE_BINS / span;
    int step = 1, minor = 0;                         /* step in beats; minor = draw beat ticks */
    if (px_beat >= 4.0) minor = 1;
    else if (px_beat * 4.0 >= 3.0) step = 4;
    else if (px_beat * 16.0 >= 3.0) step = 16;
    else return 0;
    long k0 = (long)ceil(((double)lo - origin) / beat), k1 = (long)floor(((double)hi - beat * 0.02 - origin) / beat);   /* a tick on the very end is the next repeat's start */
    int n = 0;
    uint8_t seen[WAVE_BINS]; memset(seen, 0, sizeof seen);   /* 1 = beat, 2 = bar, 3 = phrase */
    for (long k = k0; k <= k1; k++) {
        long km = ((k % 4) + 4) % 4;
        int bar = km == 0;
        if (bar) { if (((k % step) + step) % step) continue; }
        else if (!minor) continue;
        int b = (int)((origin + (double)k * beat - (double)lo) * WAVE_BINS / span);
        if (b < 0 || b >= WAVE_BINS) continue;
        int lv = !bar ? 1 : (((k % 16) + 16) % 16 == 0 ? 3 : 2);
        if (seen[b] < lv) seen[b] = (uint8_t)lv;
    }
    for (int b = 0; b < WAVE_BINS; b++) {
        if (seen[b] == 3) ph[(*nph)++] = (uint8_t)b;
        else if (seen[b]) out[n++] = (uint8_t)(b | (seen[b] == 1 ? 0x80 : 0));
    }
    return n;
}

/* The view window [lo, hi) in buffer frames for track t, or 0 when the
 * legacy whole-clip view applies (recording, empty). Sets *zoomed.
 *  - IN/OUT being adjusted (and 2.5 s after): 2 bars (1/2 bar when fine)
 *    centred on the point, clamped to [0, total); the whole buffer when the
 *    clip is not longer than the window.
 *  - PISTA otherwise: the whole buffer; MIX: the playing region IN..OUT. */
static int view_window(synth_t *s, int t, uint32_t *lo, uint32_t *hi, int *zoomed) {
    *zoomed = 0;
    int st = core_state(s, t);
    long len = get_t(s, t, "len"), off = get_t(s, t, "off"), full = get_t(s, t, "full");
    if (st == MK_REC || len <= 0) return 0;
    long total = full > len ? full : len;
    if (s->zoom_t == t && s->clock_frames < s->zoom_until) {
        long w = lround(bar_frames(s) * (s->zoom_fine ? 0.5 : 2.0));
        if (w < 64) w = 64;
        if (w < total) {
            long c = s->zoom_pt ? len : off;
            long l = c - w / 2;
            if (l > total - w) l = total - w;
            if (l < 0) l = 0;
            *lo = (uint32_t)l; *hi = (uint32_t)(l + w); *zoomed = 1;
            return 1;
        }
        *lo = 0; *hi = (uint32_t)total;
        return 1;
    }
    if (s->sel) { *lo = 0; *hi = (uint32_t)total; return 1; }
    if (off < 0 || off >= len) off = 0;
    *lo = (uint32_t)off; *hi = (uint32_t)len;
    return 1;
}

static int bin_of(uint32_t f, uint32_t lo, uint32_t hi) {
    return (int)((uint64_t)(f - lo) * WAVE_BINS / (hi - lo));
}

static void build_display(synth_t *s, char *out, size_t n) {
    int t = s->cur_track, st = shim_state(s, t);
    uint32_t span = 0, lo = 0, hi = 0; int pos = 0, zoomed = 0;
    int win = view_window(s, t, &lo, &hi, &zoomed);
    if (!win) lo = hi = 0;
    int whole = !win && s->sel && s->last_knob == 2;       /* legacy view flag (REC/empty only) */
    if (win) { span = hi - lo; mark_track_wave_win(s->m, t - 1, WAVE_BINS, NULL, lo, hi, &pos); }
    else mark_track_wave_ex(s->m, t - 1, WAVE_BINS, NULL, &span, &pos, whole);
    int busy = (st == S_REC || st == S_DUB);
    long every = busy ? MARK_SR / 4 : MARK_SR * 2;
    if (s->wave_dirty || whole != s->wave_whole || t != s->wave_track || st != s->wave_st || span != s->wave_span
        || lo != s->wave_lo || hi != s->wave_hi || s->in_frames - s->wave_at >= every) {
        uint8_t w[WAVE_BINS];
        if (win) mark_track_wave_win(s->m, t - 1, WAVE_BINS, w, lo, hi, &pos);
        else mark_track_wave_ex(s->m, t - 1, WAVE_BINS, w, &span, &pos, whole);
        if (whole != s->wave_whole || t != s->wave_track || span != s->wave_span || lo != s->wave_lo || hi != s->wave_hi
            || memcmp(w, s->wave, sizeof w)) {
            memcpy(s->wave, w, sizeof w);
            s->wave_seq = (s->wave_seq + 1) % 100000u;
        }
        s->wave_track = t; s->wave_st = st; s->wave_span = span; s->wave_whole = whole;
        s->wave_lo = lo; s->wave_hi = hi;
        s->wave_at = s->in_frames; s->wave_dirty = 0;
    }
    char hdr[48], hx[WAVE_BINS * 2 + 1], posf[24], tk[WAVE_BINS * 4 + 4];
    header_text(s, t, span, zoomed, hdr, sizeof hdr);
    if (s->host_mute) { char h2[48]; snprintf(h2, sizeof h2, "MUTED %s", hdr); memcpy(hdr, h2, sizeof hdr); }
    for (char *c = hdr; *c; c++) if (*c == '|') *c = '/';      /* '|' separates the display fields */
    {
        uint8_t tb[WAVE_BINS], pb[WAVE_BINS];
        int np, nt;
        if (win) nt = build_ticks(s, (double)get_t(s, t, "off"), lo, hi, tb, pb, &np);
        else nt = build_ticks(s, 0.0, 0, span, tb, pb, &np);   /* recording: from the start */
        if (nt != s->nticks || np != s->nphrase || memcmp(tb, s->ticks, (size_t)nt) || memcmp(pb, s->phrase, (size_t)np)) {
            memcpy(s->ticks, tb, (size_t)nt); s->nticks = nt;
            memcpy(s->phrase, pb, (size_t)np); s->nphrase = np;
            s->wave_seq = (s->wave_seq + 1) % 100000u;
        }
        for (int i = 0; i < s->nticks; i++) snprintf(tk + i * 2, 3, "%02X", s->ticks[i]);
        tk[s->nticks * 2] = 0;
        if (s->nphrase) {
            char *q = tk + s->nticks * 2; *q++ = '/';
            for (int i = 0; i < s->nphrase; i++) { snprintf(q, 3, "%02X", s->phrase[i]); q += 2; }
            *q = 0;
        }
    }
    for (int i = 0; i < WAVE_BINS; i++) snprintf(hx + i * 2, 3, "%02X", s->wave[i]);
    /* Outside the zoom window the format has no "hidden" playhead value, so it
     * is pinned to the edge it left through (-1 = before, -2 = after). */
    if (pos == -1) pos = 0;
    else if (pos == -2) pos = WAVE_BINS - 1;
    /* pos field: "<pos>[,<start marker>[,<end marker>]]"; an unshown marker is -1.
     * Markers are in window coordinates; shown in PISTA view (whole buffer),
     * while zoomed, and in MIX once the start is moved (never: MIX starts at IN). */
    int mark = -1, end = -1;
    if (win) {
        long off = get_t(s, t, "off"), len = get_t(s, t, "len");
        int markers = s->sel || (s->zoom_t == t && s->clock_frames < s->zoom_until);
        if (markers && off >= (long)lo && off < (long)hi) mark = bin_of((uint32_t)off, lo, hi);
        if (markers && len > (long)lo && len <= (long)hi) { end = bin_of((uint32_t)len, lo, hi); if (end >= WAVE_BINS) end = WAVE_BINS - 1; }
        if (mark >= WAVE_BINS) mark = WAVE_BINS - 1;
    } else {
        mark = span ? mark_track_start_bin_ex(s->m, t - 1, WAVE_BINS, whole) : -1;
        int adjusting = s->start_t == t && s->clock_frames < s->start_until;
        if (!(adjusting || (s->sel && s->last_knob == 1) || get_t(s, t, "off") != 0)) mark = -1;
        end = whole && span ? mark_track_end_bin(s->m, t - 1, WAVE_BINS) : -1;
    }
    if (end >= 0) snprintf(posf, sizeof posf, "%d,%d,%d", pos, mark, end);
    else if (mark >= 0) snprintf(posf, sizeof posf, "%d,%d", pos, mark);
    else snprintf(posf, sizeof posf, "%d", pos);
    snprintf(out, n, "%u|%s|%s|%s|%s", s->wave_seq, posf, span ? hx : "", hdr, tk);
}

/* ------------------------------------------------------------------ */
/*  knob_page_param: the PISTA view as a Schwung-style parameter page   */
/* ------------------------------------------------------------------ */
/*
 * get_param knob_page = "<left>|<right>|<focus>|<e1>|...|<e8>", or "" when Movy
 * should show the waveform instead (MIX, or IN/OUT zoom). left = "PISTA 3",
 * right = the track state, focus = 0..8, the knob last turned (its value text
 * replaces the label for ~1.5 s) or 0. Entry: "LABEL;d;<0..1>;<text>" is a dial
 * (position + value text), "LABEL;b;<text>" a boxed word (<= 3 chars a line).
 */
static void build_page(synth_t *s, char *out, size_t n) {
    static const char *const NAME[7] = { "EMPTY", "ARMED", "REC", "PLAY", "DUB", "STOP", "MUTE" };
    static const char *const FX_SH[7] = { "OFF", "LPF", "HPF", "CRS", "DLY", "PHS", "RNG" };
    static const char *const SPD_SH[MK_SPD_N] = { "1/4", "1/2", "3/4", "X1", "1.5", "X2", "X4" };
    out[0] = 0;
    int t = s->sel;
    if (!t) return;
    if (s->zoom_t == t && s->clock_frames < s->zoom_until) return;      /* the zoomed waveform takes over */
    long len = get_t(s, t, "len"), off = get_t(s, t, "off"), full = get_t(s, t, "full");
    long total = full > len ? full : len;
    double pin = 0.0, pout = 0.0;
    char tin[24] = "--", tout[24] = "--", tpan[16];
    if (total > 0 && len > 0) {
        pin = (double)off / (double)total;
        pout = (double)len / (double)total;
        sixteenths_label(s, off, tin, sizeof tin);
        len_label(s, len, tout, sizeof tout);
    }
    int pan = get_t(s, t, "pan"), lvl = track_level_get(s, t), fxp = get_t(s, t, "fxp");
    int fx = get_t(s, t, "fx_on") ? clampi_(get_t(s, t, "fx"), 0, 6) : 0;
    int spd = clampi_(get_t(s, t, "spd"), 0, MK_SPD_N - 1);
    pan_label(pan, tpan, sizeof tpan);
    int focus = s->clock_frames < s->focus_until ? s->last_knob : 0;
    snprintf(out, n, "PISTA %d|%s|%d|IN;d;%.3f;%s|OUT;d;%.3f;%s|VOL;d;%.3f;%d|PAN;d;%.3f;%s|"
             "FX;b;%s|AMT;d;%.3f;%d|SPD;b;%s|REV;b;%s",
             t, NAME[shim_state(s, t)], focus, pin, tin, pout, tout, lvl / 200.0, lvl, pan / 100.0, tpan,
             FX_SH[fx], fxp / 100.0, fxp, SPD_SH[spd], get_t(s, t, "rev") ? "ON" : "OFF");
}

/* ------------------------------------------------------------------ */
/*  ui_hierarchy                                                       */
/* ------------------------------------------------------------------ */

#define APP(...) do { if (p < n) { int w = snprintf(b + p, n - p, __VA_ARGS__); \
                      if (w > 0) { p += (size_t)w; if (p > n) p = n; } } } while (0)

static void build_hierarchy(synth_t *s) {
    char *b = s->hierarchy;
    size_t n = sizeof s->hierarchy, p = 0;
    static const char *fxopts =
        "[\"Off\",\"LPF\",\"HPF\",\"Crush\",\"Delay\",\"Phaser\",\"Ring\"]";
    static const char *stopts =
        "[\"Empty\",\"Armed\",\"Rec\",\"Play\",\"Dub\",\"Stop\",\"Mute\"]";
    static const char *onoff = "[\"Off\",\"On\"]";
    static const char *lenopts =
        "[\"Free\",\"1 bar\",\"2 bars\",\"3 bars\",\"4 bars\",\"5 bars\",\"6 bars\",\"7 bars\","
        "\"8 bars\",\"9 bars\",\"10 bars\",\"11 bars\",\"12 bars\",\"13 bars\",\"14 bars\","
        "\"15 bars\",\"16 bars\",\"17 bars\",\"18 bars\",\"19 bars\",\"20 bars\",\"21 bars\",\"22 bars\","
        "\"23 bars\",\"24 bars\",\"25 bars\",\"26 bars\",\"27 bars\",\"28 bars\",\"29 bars\",\"30 bars\","
        "\"31 bars\",\"32 bars\"]";

    APP("{\"pad_layout\":\"drums\",\"pad_leds_param\":\"pad_leds\","
        "\"status_text_param\":\"status_text\",\"display_param\":\"display\",\"delete_pad_param\":\"delete_pad\","
        "\"step_buttons_param\":\"step\",\"step_up_param\":\"step_up\",\"step_bank_param\":\"step_bank\",\"shift_pad_param\":\"shift_pad\",\"screen_param\":\"screen\",\"step_leds_param\":\"step_leds\",\"loop_knob_param\":\"loop_knob\","
        "\"knob_capture_param\":\"knob_capture\",\"knob_page_param\":\"knob_page\",\"knob_delta_param\":\"knob_delta\",\"undo_param\":\"undo_cmd\","
        "\"pedal_param\":\"pedal\",\"track_mute_param\":\"track_mute\",\"transport_param\":\"transport\",\"levels\":{\"root\":{\"name\":\"Looper 606\",\"params\":[");
    for (int t = 1; t <= NTRK; t++)
        APP("{\"key\":\"t%d_level\",\"name\":\"Track %d Vol\",\"short_name\":\"T%d\","
            "\"type\":\"int\",\"min\":0,\"max\":200,\"default\":100,\"step\":5},", t, t, t);
    APP("{\"key\":\"in_gain\",\"name\":\"Input Vol\",\"short_name\":\"IN\",\"type\":\"int\","
        "\"min\":0,\"max\":200,\"default\":100,\"step\":5},"
        "{\"key\":\"master\",\"name\":\"Master\",\"short_name\":\"MST\",\"type\":\"int\","
        "\"min\":0,\"max\":200,\"default\":100,\"step\":5},"
        "{\"level\":\"track\",\"name\":\"Track\"},"
        "{\"level\":\"setup\",\"name\":\"Setup\"},"
        "{\"level\":\"status\",\"name\":\"Status\"}");
    for (int v = 0; v < 32; v++)
        APP(",{\"level\":\"v%02d\",\"label\":\"Pad %02d\"}", v + 1, v + 1);
    APP("],\"knobs\":[");
    for (int t = 1; t <= NTRK; t++) APP("\"t%d_level\",", t);
    APP("\"in_gain\",\"master\"]},");

    APP("\"track\":{\"name\":\"Track\",\"params\":["
        "{\"key\":\"fx_type\",\"name\":\"FX Type\",\"short_name\":\"FX\",\"type\":\"enum\","
        "\"options\":%s,\"default\":0},"
        "{\"key\":\"fx_amt\",\"name\":\"FX Amount\",\"short_name\":\"AMT\",\"type\":\"int\","
        "\"min\":0,\"max\":100,\"default\":50,\"step\":2},"
        "{\"key\":\"trk_pan\",\"name\":\"Pan\",\"short_name\":\"PAN\",\"type\":\"int\","
        "\"min\":0,\"max\":100,\"default\":50,\"step\":2},"
        "{\"key\":\"trk_rev\",\"name\":\"Reverse\",\"short_name\":\"REV\",\"type\":\"enum\","
        "\"options\":%s,\"default\":0},"
        "{\"key\":\"trk_shot\",\"name\":\"One-shot\",\"short_name\":\"1SHOT\",\"type\":\"enum\","
        "\"options\":%s,\"default\":0},"
        "{\"key\":\"trk_spd\",\"name\":\"Speed\",\"short_name\":\"SPD\",\"type\":\"enum\","
        "\"options\":[\"x1/4\",\"x1/2\",\"x3/4\",\"x1\",\"x1.5\",\"x2\",\"x4\"],\"default\":3}],"
        "\"knobs\":[\"fx_type\",\"fx_amt\",\"trk_pan\",\"trk_rev\",\"trk_shot\",\"trk_spd\"]},",
        fxopts, onoff, onoff);

    APP("\"setup\":{\"name\":\"Setup\",\"params\":["
        "{\"key\":\"threshold\",\"name\":\"Arm Threshold\",\"short_name\":\"THR\",\"type\":\"int\","
        "\"min\":-70,\"max\":-6,\"default\":%d,\"step\":1,\"unit\":\"dB\"},"
        "{\"key\":\"quantize\",\"name\":\"Quantize\",\"short_name\":\"QNT\",\"type\":\"enum\","
        "\"options\":%s,\"default\":1},"
        "{\"key\":\"rec_len\",\"name\":\"Fixed Length\",\"short_name\":\"LEN\",\"type\":\"enum\","
        "\"options\":%s,\"default\":0},"
        "{\"key\":\"rec_action\",\"name\":\"Rec Action\",\"short_name\":\"RECACT\",\"type\":\"enum\","
        "\"options\":[\"Rec>Play\",\"Rec>Dub\"],\"default\":0},"
        "{\"key\":\"count_in\",\"name\":\"Count-in\",\"short_name\":\"CNT\",\"type\":\"enum\","
        "\"options\":%s,\"default\":0},"
        "{\"key\":\"bpm_override\",\"name\":\"BPM Override (0=off)\",\"short_name\":\"BPM\","
        "\"type\":\"int\",\"min\":0,\"max\":200,\"default\":0,\"step\":1},"
        "{\"key\":\"monitor\",\"name\":\"Monitor\",\"short_name\":\"MON\",\"type\":\"enum\","
        "\"options\":%s,\"default\":1},"
        "{\"key\":\"arm\",\"name\":\"Arm Auto-rec\",\"short_name\":\"ARM\",\"type\":\"enum\","
        "\"options\":%s,\"default\":0}],"
        "\"knobs\":[\"threshold\",\"quantize\",\"rec_len\",\"rec_action\",\"count_in\","
        "\"bpm_override\",\"monitor\",\"arm\"]},",
        DEFAULT_THRESHOLD, onoff, lenopts, onoff, onoff, onoff);

    APP("\"status\":{\"name\":\"Status\",\"params\":[");
    for (int t = 1; t <= NTRK; t++)
        APP("%s{\"key\":\"t%d_state\",\"name\":\"Track %d\",\"short_name\":\"S%d\","
            "\"type\":\"enum\",\"access\":\"read\",\"options\":%s,\"default\":0}",
            t > 1 ? "," : "", t, t, t, stopts);
    APP("],\"knobs\":[");
    for (int t = 1; t <= NTRK; t++) APP("%s\"t%d_state\"", t > 1 ? "," : "", t);
    APP("]}");

    /* Pad rack: 32 voices in rack order (pad 1 = bottom-left), each
     * declaring its note so Movy's pad -> note map matches PAD_GRID. */
    for (int v = 0; v < 32; v++) {
        int row_b = (v % 16) / 4, col = (v % 4) + 4 * (v / 16);
        const pad_def_t *d = &PAD_GRID[3 - row_b][col];
        APP(",\"v%02d\":{\"name\":\"%s\",\"note\":%d,\"knobs\":[]}",
            v + 1, d->label, NOTE_BASE + v);
    }
    APP("}}");
    if (p >= n) p = n - 1;
    b[p] = '\0';
}
#undef APP

/* ------------------------------------------------------------------ */
/*  Loop persistence                                                    */
/* ------------------------------------------------------------------ */
/*
 * Movy keeps what a module publishes in get_param("state") per track in the
 * Set's chains.json and hands it back with set_param("state") when the Set is
 * loaded, but a module is told neither the Set nor the track it lives in. So
 * the state blob carries a random LOOP ID ("l6id"), created with the instance;
 * the loops of that instance live in
 *
 *     <base>/<loop id>/t1.wav .. t6.wav + loops.json
 *
 * (<base> = /data/UserData/schwung/looper-loops, user data on the data
 * partition, which survives power-off and Movy/Schwung updates). The id is
 * therefore per set AND per track: a Set that holds the module on two tracks
 * has two ids, and a copy of the Set made by Movy shares the ids of its source
 * until one of them records something new.
 *
 * ALL FILE I/O RUNS ON ONE BACKGROUND THREAD (persist_main), never on the
 * audio thread and never inside get_param/set_param (Movy reads "state" from
 * its audio callback). The thread polls a cheap revision signature of every
 * track (length, start, buffer, and a counter the engine bumps while a buffer
 * is written); a changed track is saved once its signature has been stable for
 * LOOPER606_PERSIST_DELAY_MS (default 1500), so nothing is written while a
 * recording or overdub is in progress. A cleared track deletes its file. Saves
 * go to <file>.tmp and are renamed, loops.json last, so a power cut leaves the
 * previous version. Restoring (set_param "state" with an id on a fresh, empty
 * instance) is requested to the same thread, which reads the WAVs back into
 * the track buffers and leaves the tracks STOPPED.
 */
#define PERSIST_POLL_MS 100

static const char *loop_id_now(const synth_t *s) {
    return s->loop_id[atomic_load(&s->loop_id_ix)];
}

static void loop_id_set(synth_t *s, const char *id) {
    int nx = 1 - atomic_load(&s->loop_id_ix);
    snprintf(s->loop_id[nx], sizeof s->loop_id[nx], "%s", id);
    atomic_store(&s->loop_id_ix, nx);
}

static void loop_id_make(synth_t *s) {
    unsigned char r[6] = {0};
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(r, 1, sizeof r, f) : 0;
    if (f) fclose(f);
    if (got < sizeof r) {                      /* no urandom: time + address */
        unsigned long long x = (unsigned long long)time(NULL) * 6364136223846793005ull
                             ^ (unsigned long long)(size_t)s;
        for (int i = 0; i < 6; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; r[i] = (unsigned char)(x >> 33); }
    }
    char id[16];
    snprintf(id, sizeof id, "%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
    loop_id_set(s, id);
}

static int valid_loop_id(const char *id) {         /* never build a path from junk */
    size_t n = strlen(id);
    if (n < 4 || n > 15) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return 1;
}

static void persist_dir_of(const synth_t *s, const char *id, char *out, size_t n) {
    snprintf(out, n, "%s/%s", s->persist_base, id);
}

/* One pass: save every track in `mask`. Returns 0 on success. */
static int persist_save(synth_t *s, unsigned mask) {
    char dir[300];
    mkdir(s->persist_base, 0755);
    persist_dir_of(s, loop_id_now(s), dir, sizeof dir);
    return mark_persist_save(s->m, dir, mask);
}

static void persist_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *persist_main(void *arg) {
    synth_t *s = (synth_t *)arg;
    uint64_t seen[NTRK];
    long stable[NTRK];
    for (int i = 0; i < NTRK; i++) {
        seen[i] = s->saved_sig[i] = mark_persist_sig(s->m, i);
        stable[i] = 0;
    }
    while (!atomic_load(&s->persist_stop)) {
        persist_sleep_ms(PERSIST_POLL_MS);
        if (atomic_exchange(&s->persist_load_req, 0)) {
            char dir[300];
            persist_dir_of(s, loop_id_now(s), dir, sizeof dir);
            int r = mark_persist_load(s->m, dir);
            char jp[320];
            snprintf(jp, sizeof jp, "%s/loops.json", dir);
            s->persist_off = (r < 0 && access(jp, F_OK) == 0);   /* there, but unusable: do not overwrite */
            hlog("looper-606: restore %s -> %d track(s)%s", loop_id_now(s), r,
                 s->persist_off ? " (UNREADABLE: not saving over it)" : "");
            for (int i = 0; i < NTRK; i++) {
                seen[i] = mark_persist_sig(s->m, i); stable[i] = 0;
                /* restored: what is on disk is what is live. No loops.json yet
                 * (or an unreadable one): nothing on disk, save what is live. */
                s->saved_sig[i] = r >= 0 ? seen[i] : 0;
            }
            s->wave_dirty = 1;
            continue;
        }
        unsigned mask = 0;
        uint64_t now[NTRK];
        for (int i = 0; i < NTRK; i++) {
            now[i] = mark_persist_sig(s->m, i);
            if (now[i] != seen[i]) { seen[i] = now[i]; stable[i] = 0; }
            else stable[i] += PERSIST_POLL_MS;
            if (now[i] != s->saved_sig[i] && stable[i] >= s->persist_delay_ms) mask |= 1u << i;
        }
        if (!mask || s->persist_off) continue;
        int r = persist_save(s, mask);
        if (r == 0) {
            for (int i = 0; i < NTRK; i++) if (mask & (1u << i)) s->saved_sig[i] = now[i];
            atomic_store(&s->persist_error, 0);
            hlog("looper-606: saved loops %s (mask 0x%02x)", loop_id_now(s), mask);
        } else if (!atomic_exchange(&s->persist_error, 1)) {
            hlog("looper-606: could not save loops to %s (will retry)", s->persist_base);
        }
    }
    return NULL;
}

/* Stop the thread, then write whatever is still unsaved (destroy runs on a
 * control thread, never the audio thread). */
static void persist_stop_and_flush(synth_t *s) {
    if (!s->persist_thr_valid) return;
    atomic_store(&s->persist_stop, 1);
    pthread_join(s->persist_thr, NULL);
    s->persist_thr_valid = 0;
    unsigned mask = 0;
    for (int i = 0; i < NTRK; i++)
        if (mark_persist_sig(s->m, i) != s->saved_sig[i]) mask |= 1u << i;
    if (mask && persist_save(s, mask) == 0) hlog("looper-606: flushed loops %s on close", loop_id_now(s));
}

static void persist_start(synth_t *s) {
    const char *b = getenv("LOOPER606_PERSIST_DIR");
    snprintf(s->persist_base, sizeof s->persist_base, "%s",
             b && b[0] ? b : "/data/UserData/schwung/looper-loops");
    const char *d = getenv("LOOPER606_PERSIST_DELAY_MS");
    s->persist_delay_ms = d && atoi(d) > 0 ? atoi(d) : 1500;
    loop_id_make(s);
    if (pthread_create(&s->persist_thr, NULL, persist_main, s) == 0) s->persist_thr_valid = 1;
    else hlog("looper-606: persistence thread failed to start; loops will not be saved");
}

/* "state": the engine's settings blob plus the looper's own fields. The extra
 * keys are flat and prefixed l6 so the engine's naive key search never sees them. */
static long jnum(const char *js, const char *key, long def) {
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atol(p + strlen(pat)) : def;
}
static int jstr(const char *js, const char *key, char *out, size_t n) {
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(js, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return *p == '"' ? 0 : -1;
}
static int jcsv(const char *js, const char *key, int idx, int def) {
    char b[96];
    if (jstr(js, key, b, sizeof b) != 0) return def;
    const char *p = b;
    for (int i = 0; i < idx; i++) { p = strchr(p, ','); if (!p) return def; p++; }
    return atoi(p);
}

/* knob automation in the state blob: "T.FX.K=<base64 of PF_AU_N bytes>;..." (byte 255 = empty step, no padding),
 * whole entries only, bounded by the room left in the buffer. 256 steps = 342 characters per curve. */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_enc(const uint8_t *in, int n, char *out) {
    int w = 0;
    for (int i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[w++] = B64[v >> 18 & 63]; out[w++] = B64[v >> 12 & 63];
        if (i + 1 < n) out[w++] = B64[v >> 6 & 63];
        if (i + 2 < n) out[w++] = B64[v & 63];
    }
    return w;
}
static int b64_val(char c) { const char *q = c ? strchr(B64, c) : NULL; return q ? (int)(q - B64) : -1; }
static int au_encode(synth_t *s, char *out, int n) {
    int w = 0; uint8_t b[PF_AU_N];
    out[0] = 0;
    for (int t = 1; t <= NTRK; t++)
        for (int fx = 0; fx < PF_NFX; fx++)
            for (int k = 0; k < 3; k++) {
                if (perf_auto_get(s->perf, t, fx, k, b) <= 0) continue;
                if (w + 16 + (PF_AU_N * 4 + 2) / 3 + 2 >= n) return w;
                w += snprintf(out + w, (size_t)(n - w), "%s%d.%d.%d=", w ? ";" : "", t, fx, k);
                w += b64_enc(b, PF_AU_N, out + w);
                out[w] = 0;
            }
    return w;
}
static void au_decode(synth_t *s, const char *val) {
    const char *p = strstr(val, "\"l6au\":\"");
    if (!s->perf) return;
    perf_auto_clear(s->perf, -1, -1, -1);
    if (!p) return;
    p += 8;
    while (*p && *p != '"') {
        int t, fx, k, n = 0;
        if (sscanf(p, "%d.%d.%d=%n", &t, &fx, &k, &n) < 3 || n <= 0) break;
        p += n;
        uint8_t b[PF_AU_N]; int nb = 0, acc = 0, bits = 0;
        while (b64_val(*p) >= 0 && nb < PF_AU_N) {
            acc = acc << 6 | b64_val(*p++); bits += 6;
            if (bits >= 8) { bits -= 8; b[nb++] = (uint8_t)(acc >> bits & 255); acc &= (1 << bits) - 1; }
        }
        if (nb != PF_AU_N) break;
        perf_auto_put(s->perf, t, fx, k, b);
        while (b64_val(*p) >= 0) p++;
        if (*p == ';') p++;
    }
}

static int state_get(synth_t *s, char *buf, int buf_len) {
    int n = mark_get_param(s->m, "state", buf, buf_len);
    if (n <= 0 || n >= buf_len) return n;
    while (n > 0 && buf[n - 1] != '}') n--;       /* back to the closing brace */
    if (n <= 0) return -1;
    n--;
    char sp[40] = "", mu[40] = "", lv[48] = "";
    for (int t = 1; t <= NTRK; t++) {
        char c[12];
        snprintf(c, sizeof c, "%s%d", t > 1 ? "," : "", get_t(s, t, "spd"));
        strncat(sp, c, sizeof sp - strlen(sp) - 1);
        snprintf(c, sizeof c, "%s%d", t > 1 ? "," : "", s->muted[t - 1]);
        strncat(mu, c, sizeof mu - strlen(mu) - 1);
        snprintf(c, sizeof c, "%s%d", t > 1 ? "," : "", s->muted[t - 1] ? s->saved_level[t - 1] : get_t(s, t, "level"));
        strncat(lv, c, sizeof lv - strlen(lv) - 1);
    }
    static char au[6000];                           /* get_param runs on one thread */
    { int room = buf_len - n - 400; au_encode(s, au, room < 1 ? 1 : room < (int)sizeof au ? room : (int)sizeof au); }
    int w = snprintf(buf + n, (size_t)(buf_len - n),
        ",\"l6id\":\"%s\",\"l6rl\":%d,\"l6ci\":%d,\"l6thdb\":%d,\"l6ig\":%d,"
        "\"l6sq\":\"%s\",\"l6mu\":\"%s\",\"l6lv\":\"%s\",\"l6au\":\"%s\"}",
        loop_id_now(s), s->rec_len, s->count_in, s->threshold, s->in_gain, sp, mu, lv, au);
    if (w < 0 || w >= buf_len - n) { buf[n] = '}'; buf[n + 1] = 0; return n + 1; }
    return n + w;
}

static void state_set(synth_t *s, const char *val) {
    int fresh = 1;                                 /* nothing recorded yet on this instance */
    for (int t = 1; t <= NTRK; t++)
        if (core_state(s, t) != MK_EMPTY || s->armed[t - 1] || s->countin[t - 1] >= 0) fresh = 0;
    mark_set_param(s->m, "state", val);
    if (strstr(val, "\"l6rl\":")) {                 /* written by state_get below */
        au_decode(s, val);
        s->rec_len   = clampi_((int)jnum(val, "l6rl", s->rec_len), 0, MAX_BARS);
        s->count_in  = jnum(val, "l6ci", s->count_in) ? 1 : 0;
        s->threshold = clampi_((int)jnum(val, "l6thdb", s->threshold), -70, -6);
        s->in_gain   = clampi_((int)jnum(val, "l6ig", s->in_gain), 0, 200);
        for (int t = 1; t <= NTRK; t++) {
            /* l6sq = 7-step index (0.6.1); l6sp = old 0/1/2 (1/2x,1x,2x) -> index 1/3/5 */
            static const int OLD_SPD[3] = { 1, 3, 5 };
            int sp = strstr(val, "\"l6sq\":") ? clampi_(jcsv(val, "l6sq", t - 1, MK_SPD_1X), 0, MK_SPD_N - 1)
                                              : OLD_SPD[clampi_(jcsv(val, "l6sp", t - 1, 1), 0, 2)];
            set_t(s, t, "spd", sp);
            if (jcsv(val, "l6mu", t - 1, 0)) {
                s->muted[t - 1] = 1;
                s->saved_level[t - 1] = clampi_(jcsv(val, "l6lv", t - 1, 100), 0, 200);
                set_t(s, t, "level", 0);
            }
        }
    }
    char id[16];
    if (fresh && jstr(val, "l6id", id, sizeof id) == 0 && valid_loop_id(id)) {
        if (strcmp(id, loop_id_now(s)) != 0) loop_id_set(s, id);
        atomic_store(&s->persist_load_req, 1);     /* the thread reads the WAVs */
    }
}

/* ------------------------------------------------------------------ */
/*  plugin_api_v2                                                      */
/* ------------------------------------------------------------------ */

static void *gen_create(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;
    synth_t *s = (synth_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = mark_create_in_dir(g_host, module_dir);
    if (!s->m) { free(s); return NULL; }
    s->cur_track = 1;
    s->perf = perf_create();
    mark_set_perf(s->m, s->perf);
    s->bpm_src_logged = -1;
    s->threshold = DEFAULT_THRESHOLD;
    s->count_bars = 1;
    s->in_gain = 100;
    s->mute_g = 1.0f;
    for (int i = 0; i < NTRK; i++) {
        s->countin[i] = -1; s->saved_level[i] = 100;
    }
    mark_set_param(s->m, "hw_input", "1");
    /* No Mark "session_dir" on purpose: Mark's state getter lists the session slots
     * with fopen(), and Movy reads "state" from its audio callback. Loops are
     * persisted by our own thread (see "Loop persistence"). */
    build_hierarchy(s);
    persist_start(s);
    hlog("looper-606: created, host get_bpm present=%d, track capacity %d s",
         (g_host && g_host->get_bpm) ? 1 : 0, get_i(s, "track_secs"));
    if (g_host && g_host->mapped_memory)
        hlog("looper-606: input source = host mailbox (mapped_memory + audio_in_offset=%d)",
             g_host->audio_in_offset);
    else
        hlog("looper-606: input source = NONE (host has no mapped_memory; output stays silent)");
    return s;
}

static void gen_destroy(void *inst) {
    synth_t *s = (synth_t *)inst;
    if (!s) return;
    persist_stop_and_flush(s);
    mark_set_perf(s->m, NULL);
    mark_destroy(s->m);
    perf_destroy(s->perf);
    free(s);
}

static void tick_block(synth_t *s, const int16_t *in, int16_t *out, int frames) {
    int16_t buf[MAXBLK * 2];
    int peak = 0, raw = 0;
    for (int i = 0; i < frames * 2; i++) {
        int v = in[i] * s->in_gain / 100;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
        if (abs(v) > peak) peak = abs(v);
        if (abs(in[i]) > raw) raw = abs(in[i]);
    }
    s->in_peak_now = raw;
    if (!s->audio_logged && raw > 0) {
        s->audio_logged = 1; hlog("looper-606: audio_in is live (peak %d)", raw);
    }
    if (!s->in_diag_done) {
        if (raw > s->in_peak_1s) s->in_peak_1s = raw;
        s->in_frames += frames;
        if (s->in_frames >= MARK_SR) {
            s->in_diag_done = 1;
            hlog("looper-606: input peak after 1 s = %d%s", s->in_peak_1s,
                 s->in_peak_1s ? "" : " (SILENT: the host mailbox carries no audio; "
                                      "check line-in routing / jack)");
        }
    }
    /* ARM: threshold crossing starts recording; count-in expiry too */
    int thr = (int)(32768.0 * pow(10.0, s->threshold / 20.0));
    {
        int any_armed = 0;
        for (int t = 0; t < NTRK; t++) any_armed |= s->armed[t];
        if (any_armed) {
            if (peak > s->arm_diag_peak) s->arm_diag_peak = peak;
            s->arm_diag_frames += frames;
            if (s->arm_diag_frames >= MARK_SR) {
                hlog("looper-606: armed, input peak (after gain %d%%) = %d, threshold = %d (%d dB), peak = %d dB",
                     s->in_gain, s->arm_diag_peak, thr, s->threshold, peak_db(s->arm_diag_peak));
                s->arm_diag_frames = 0; s->arm_diag_peak = 0;
            }
        } else { s->arm_diag_frames = 0; s->arm_diag_peak = 0; }
    }
    for (int t = 1; t <= NTRK; t++) {
        int i = t - 1;
        if (s->armed[i] && peak >= thr) {
            s->armed[i] = 0; s->rec_fired[i] = 0;
            say_rec(s, t);
            hlog("looper-606: armed track %d crossed threshold (peak %d >= %d), recording now", t, peak, thr);
            /* The input IS the timing: never wait for a bar boundary (a stale
             * or stopped clock would hold the track pending forever). */
            {
                int q = get_i(s, "quantize");
                set_i(s, "quantize", 0);
                trig_t(s, t, "btn");
                set_i(s, "quantize", q);
            }
        }
        if (s->countin[i] >= 0) {
            s->countin[i] -= frames;
            if (s->countin[i] < 0) {
                s->countin[i] = -1; s->rec_fired[i] = 0;
                say_rec(s, t);
                trig_t(s, t, "btn");
            }
        }
    }
    mark_process(s->m, buf, out, frames);
    /* Movy's track mute: a ~10 ms linear ramp to / from silence over the whole mix. The
     * tracks keep running in time underneath, like the looper's own MUTE. */
    {
        float tgt = s->host_mute ? 0.0f : 1.0f;
        if (s->mute_g != tgt || tgt == 0.0f) {
            const float step = 1.0f / (MARK_SR * 0.01f);
            for (int n = 0; n < frames; n++) {
                if (s->mute_g < tgt) { s->mute_g += step; if (s->mute_g > tgt) s->mute_g = tgt; }
                else if (s->mute_g > tgt) { s->mute_g -= step; if (s->mute_g < tgt) s->mute_g = tgt; }
                out[n * 2]     = (int16_t)(out[n * 2] * s->mute_g);
                out[n * 2 + 1] = (int16_t)(out[n * 2 + 1] * s->mute_g);
            }
        }
    }
    s->clock_frames += frames;
    pedal_tick(s);
    perf_set_bpm(s->perf, cur_bpm(s));
    if (s->auto_hold && (!s->perf_mode || perf_top_fx(s->perf) < 0)) auto_hold_end(s, 0);   /* the mode or the effect went away under the hold */
    for (int t = 1; t <= NTRK; t++) {
        int pnd = core_state(s, t) == MK_EMPTY && core_pending(s, t);
        if (pnd != s->pend_seen[t - 1]) {
            s->pend_seen[t - 1] = pnd;
            hlog("looper-606: track %d %s", t, pnd ? "pending (waiting for the grid)" : "pending resolved");
        }
    }

    /* Tempo source, logged once (and again only if it changes). */
    {
        int src = get_i(s, "tempo_src");
        if (src != s->bpm_src_logged && s->in_frames >= MARK_SR / 2 && s->bpm_src_logged < 100) {
            hlog("looper-606: bpm source = %s (%d bpm)",
                 BPM_SRC_NAME[src & 3], (int)(cur_bpm(s) + 0.5));
            s->bpm_src_logged = src;
        }
    }

    /* Quantize watchdog: a track must never wait on the grid forever (a
     * clock that was started and then stopped ticking would do that).
     * After 1.5 measures + 0.5 s, cancel the pending action and run it
     * unquantized. */
    {
        long lim = (long)((double)MARK_SR * 60.0 / cur_bpm(s) * 4.0 * 1.5) + MARK_SR / 2;
        for (int t = 1; t <= NTRK; t++) {
            int i = t - 1;
            if (core_pending(s, t)) {
                s->pend_frames[i] += frames;
                if (s->pend_frames[i] > lim) {
                    int q = get_i(s, "quantize");
                    s->pend_frames[i] = 0;
                    trig_t(s, t, "btn");              /* cancels the pending action */
                    set_i(s, "quantize", 0);
                    trig_t(s, t, "btn");              /* ...and applies it now */
                    set_i(s, "quantize", q);
                    if (!s->pend_logged) {
                        s->pend_logged = 1;
                        hlog("looper-606: quantize wait timed out on track %d, started immediately", t);
                    }
                }
            } else {
                s->pend_frames[i] = 0;
            }
        }
    }

    /* fixed length: close the recording after N bars */
    int bars = s->rec_len;
    for (int t = 1; t <= NTRK; t++) {
        int i = t - 1, st = core_state(s, t);
        if (st == MK_REC) {
            if (s->prev_st[i] != MK_REC) { s->rec_frames[i] = 0; s->rec_fired[i] = 0; }
            s->rec_frames[i] += frames;
            if (bars > 0 && !s->rec_fired[i] &&
                s->rec_frames[i] >= (long)((double)MARK_SR * 60.0 / cur_bpm(s) * 4.0 * bars)) {
                s->rec_fired[i] = 1;
                say_rec(s, t);
                trig_t(s, t, "btn");
            }
        }
        s->prev_st[i] = st;
    }
    for (int i = 0; i < NTRK; i++) {
        if (s->dub_after[i] && core_state(s, i + 1) == MK_PLAY) {
            s->dub_after[i] = 0;
            trig_t(s, i + 1, "btn");        /* PLAY -> DUB */
        }
    }

    /* Keep-alive. Movy parks a chain whose synth has been silent for ~1 s
     * (peak <= 4) and only renders it every ~0.5 s; its idle gate ignores a
     * synth's requires_continuous_processing. A looper must keep running
     * (loop positions, armed threshold, count-in, quantize), so while any
     * track is in use we add a +-8 LSB Nyquist dither (about -72 dBFS; the gate measures the raw synth
     * output, before FX/volume, and treats <= 4 as silence). */
    {
        int active = 0;
        for (int t = 1; t <= NTRK && !active; t++)
            if (core_state(s, t) != MK_EMPTY || s->armed[t - 1] || s->countin[t - 1] >= 0 ||
                core_pending(s, t))
                active = 1;
        if (s->arm) active = 1;     /* ARM on: stay awake before the pad is even pressed */
        if (active)
            for (int n = 0; n < frames; n++) {
                int d = (n & 1) ? -8 : 8, l = out[n * 2] + d, r = out[n * 2 + 1] + d;
                out[n * 2]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
                out[n * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            }
    }
}

static void gen_render(void *inst, int16_t *out_lr, int frames) {
    synth_t *s = (synth_t *)inst;
    static const int16_t no_input[MAXBLK * 2];     /* zeros: loops still play without a mailbox */
    const int16_t *in = NULL;
    if (g_host && g_host->mapped_memory)
        in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    for (int done = 0; done < frames; done += MAXBLK) {
        int n = frames - done < MAXBLK ? frames - done : MAXBLK;
        tick_block(s, in ? in + done * 2 : no_input, out_lr + done * 2, n);
    }
}

static void gen_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    synth_t *s = (synth_t *)inst;
    if (!s || len < 1) return;
    uint8_t st = msg[0];
    if (st == 0xF8 && !s->clock_logged) {
        s->clock_logged = 1; hlog("looper-606: first MIDI clock received (src %d)", source);
    }
    if (len >= 2 && source != MOVE_MIDI_SOURCE_FX_BROADCAST) {
        int hi = st & 0xF0;
        if (hi == 0x90 || hi == 0x80) {
            const pad_def_t *p = pad_for_note(msg[1]);
            int on = (hi == 0x90) && len >= 3 && msg[2] > 0;
            if (on && s->notes_logged < 8) {
                s->notes_logged++;
                hlog("looper-606: note-on %d", msg[1]);
            }
            if (!on && s->offs_logged < 8) {
                s->offs_logged++;
                hlog("looper-606: note-off %d (status 0x%02X vel %d)", msg[1], st, len >= 3 ? msg[2] : -1);
            }
            if (p && p->act != A_NONE) {
                if (on) pad_down(s, p); else pad_up(s, p);
            }
        }
    }
    mark_on_midi(s->m, msg, len, source);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int is_trk_key(const char *key, int *t, const char **rest) {
    if (key[0] == 't' && key[1] >= '1' && key[1] <= '0' + NTRK && key[2] == '_') {
        *t = key[1] - '0'; *rest = key + 3; return 1;
    }
    return 0;
}

/* Movy's transport (transport_param). "stop": like STOP ALL, but the tracks that were
 * playing or overdubbing are remembered (a second stop keeps what the first
 * remembered). "play": start exactly those from the loop start, quantized to the bar
 * when quantize is on; nothing remembered = every track holding audio (ALL START).
 * Tracks recording, playing or overdubbing are not touched by "play". */
static void transport_cmd(synth_t *s, const char *val) {
    if (!strcmp(val, "stop")) {
        unsigned mask = 0;
        for (int k = 1; k <= NTRK; k++) {
            int st = core_state(s, k);
            if (st == MK_PLAY || st == MK_DUB) mask |= 1u << (k - 1);
        }
        if (mask) s->play_mask = mask;
        say(s, "TRANSPORT STOP");
        for (int k = 1; k <= NTRK; k++) { clear_flags(s, k); trig_t(s, k, "stop"); }
    } else if (!strcmp(val, "play")) {
        char b[16]; snprintf(b, sizeof b, "%u", s->play_mask);
        say(s, "TRANSPORT PLAY");
        mark_set_param(s->m, "resume", b);
    }
}

static void gen_set_param(void *inst, const char *key, const char *val) {
    synth_t *s = (synth_t *)inst;
    int t; const char *k;
    if (!s) return;
    s->wave_dirty = 1;
    int v = atoi(val);
    if (!strcmp(key, "shift_pad")) { if (v >= 0 && v < 32) shift_pad(s, v); return; }
    if (!strcmp(key, "delete_pad")) { if (v >= 0 && v < 32) delete_pad(s, v); return; }
    if (!strcmp(key, "cur_track")) { s->cur_track = clampi(v, 1, NTRK); return; }
    if (!strcmp(key, "threshold")) { s->threshold = clampi(v, -70, -6); return; }
    if (!strcmp(key, "count_in"))  { s->count_in = v ? 1 : 0; return; }
    if (!strcmp(key, "rec_len"))   { set_rec_bars(s, v); return; }   /* the setup enum too: clamped to what the buffer holds */
    if (!strcmp(key, "step"))      {
        if (perf_page(s)) { if (s->shift_held) s->shift_used = 1; perf_step_down(s, v); } else step_press(s, v);
        return;
    }
    if (!strcmp(key, "step_up"))   { perf_step_up(s, v); return; }
    if (!strcmp(key, "step_bank")) { if (perf_page(s)) perf_bank_step(s, v); return; }
    if (!strcmp(key, "loop_knob")) { loop_knob(s, val); return; }
    if (!strcmp(key, "knob_delta")) { if (s->shift_held) s->shift_used = 1; knob_delta(s, val); return; }
    if (!strcmp(key, "undo_cmd"))  { undo_cmd(s, val); return; }
    if (!strcmp(key, "pedal"))     { pedal_set(s, val); return; }
    if (!strcmp(key, "track_mute")) { s->host_mute = v ? 1 : 0; return; }
    if (!strcmp(key, "transport")) { transport_cmd(s, val); return; }
    if (!strcmp(key, "state"))     { state_set(s, val); return; }
    if (!strcmp(key, "in_gain"))   { s->in_gain = clampi(v, 0, 200); return; }
    if (!strcmp(key, "arm")) {
        s->arm = v ? 1 : 0;
        if (!s->arm) for (int i = 0; i < NTRK; i++) s->armed[i] = 0;
        return;
    }
    if (!strcmp(key, "fx_type")) {
        set_t(s, s->cur_track, "fx", v);
        set_t(s, s->cur_track, "fx_on", v ? 1 : 0);
        return;
    }
    if (!strcmp(key, "fx_amt"))   { set_t(s, s->cur_track, "fxp", v); return; }
    if (!strcmp(key, "trk_pan"))  { set_t(s, s->cur_track, "pan", v); return; }
    if (!strcmp(key, "trk_rev"))  { set_t(s, s->cur_track, "rev", v ? 1 : 0); return; }
    if (!strcmp(key, "trk_shot")) { set_t(s, s->cur_track, "shot", v ? 1 : 0); return; }
    if (!strcmp(key, "trk_spd"))  { set_t(s, s->cur_track, "spd", clampi(v, 0, MK_SPD_N - 1)); return; }
    if (is_trk_key(key, &t, &k)) {
        if (!strcmp(k, "state")) return;                   /* read-only */
        if (!strcmp(k, "level") && s->muted[t - 1]) {      /* applied on unmute */
            s->saved_level[t - 1] = clampi(v, 0, 200);
            return;
        }
        if (!strcmp(k, "btn"))  { if (v) rec_pad(s, t); return; }
        if (!strcmp(k, "stop") || !strcmp(k, "clear")) clear_flags(s, t);
    }
    mark_set_param(s->m, key, val);
}

static int gen_get_param(void *inst, const char *key, char *buf, int buf_len) {
    synth_t *s = (synth_t *)inst;
    int t; const char *k;
    if (!s) return -1;
#define RET(...) return snprintf(buf, (size_t)buf_len, __VA_ARGS__)
    if (!strcmp(key, "ui_hierarchy")) RET("%s", s->hierarchy);
    if (!strcmp(key, "cur_track"))    RET("%d", s->cur_track);
    if (!strcmp(key, "threshold"))    RET("%d", s->threshold);
    if (!strcmp(key, "count_in"))     RET("%d", s->count_in);
    if (!strcmp(key, "rec_len"))      RET("%d", s->rec_len);
    if (!strcmp(key, "in_gain"))      RET("%d", s->in_gain);
    if (!strcmp(key, "arm"))          RET("%d", s->arm);
    if (!strcmp(key, "track_mute"))   RET("%d", s->host_mute);
    if (!strcmp(key, "fx_type"))  RET("%d", get_t(s, s->cur_track, "fx"));
    if (!strcmp(key, "fx_amt"))   RET("%d", get_t(s, s->cur_track, "fxp"));
    if (!strcmp(key, "trk_pan"))  RET("%d", get_t(s, s->cur_track, "pan"));
    if (!strcmp(key, "trk_rev"))  RET("%d", get_t(s, s->cur_track, "rev"));
    if (!strcmp(key, "trk_shot")) RET("%d", get_t(s, s->cur_track, "shot"));
    if (!strcmp(key, "trk_spd"))  RET("%d", get_t(s, s->cur_track, "spd"));
    if (!strcmp(key, "in_peak"))      RET("%d", s->in_peak_now);
    if (!strcmp(key, "status_text"))  RET("%u:%s", s->status_seq, s->status_text);
    if (!strcmp(key, "display")) {
        char d[WAVE_BINS * 4 + 128];
        build_display(s, d, sizeof d);
        RET("%s", d);
    }
    if (!strcmp(key, "knob_page")) {
        char pg[700];
        build_page(s, pg, sizeof pg);
        RET("%s", pg);
    }
    if (!strcmp(key, "step_leds")) {
        char hx[33];
        build_step_leds(s, hx);
        RET("%s", hx);
    }
    if (!strcmp(key, "step_bank")) RET("%d", perf_page(s) ? s->perf_bank : -1);   /* -1: not on the PERFORM page, < > stay the host's */
    if (!strcmp(key, "knob_capture")) RET("%d", s->sel != 0 || perf_knob_fx(s) >= 0);   /* with a PISTA selected in PERFORM the module keeps the knobs and ignores the ones with no effect to steer */
    if (!strcmp(key, "screen")) {
        static char sc[GFX_HEX_LEN + 24];       /* get_param runs on one thread */
        perf_ui_set_bank(s->perf_bank);
        perf_ui_set_grid(s->grid_view || (s->shift_held && perf_held_count(s->perf) == 0));          /* SHIFT held = peek at the grid */
        perf_ui_param(&s->pui, s->perf, perf_page(s), s->sel, s->clock_frames, s->perf_mode, sc, sizeof sc);
        RET("%s", sc);
    }
    if (!strcmp(key, "sel"))          RET("%d", s->sel);
    if (!strcmp(key, "perf_fx"))      RET("%d", perf_top_fx(s->perf));
    if (!strcmp(key, "perf_n"))       RET("%d", perf_count(s->perf));
    if (!strcmp(key, "perf_target")) { int tg = -1; if (perf_count(s->perf)) perf_entry(s->perf, perf_count(s->perf) - 1, NULL, &tg, NULL, NULL); RET("%d", tg); }
    if (!strcmp(key, "perf_boxes"))   RET("%d", perf_ui_boxes());
    if (!strcmp(key, "perf_on"))      RET("%u", perf_on_mask(s->perf));
    if (!strcmp(key, "perf_auto"))    RET("%d", perf_auto_has(s->perf, -1, perf_top_fx(s->perf), -1));   /* steps stored for the current effect */
    if (!strcmp(key, "perf_auto_hold")) RET("%d", s->auto_hold);
    if (!strcmp(key, "perf_bank"))    RET("%d", s->perf_bank);
    if (!strcmp(key, "perf_latch"))   RET("%u", perf_latch_mask(s->perf));
    if (!strncmp(key, "perf_p", 6)) {              /* perf_p<fx>_<knob>: stored 0..100 of a PERFORM knob */
        int fx, kb;
        if (sscanf(key + 6, "%d_%d", &fx, &kb) == 2) RET("%d", perf_pct(s->perf, fx, kb));
    }
    if (!strcmp(key, "loop_id"))      RET("%s", loop_id_now(s));
    if (!strcmp(key, "state"))        return state_get(s, buf, buf_len);
    if (!strcmp(key, "pad_leds")) {
        char hx[65];
        build_pad_leds(s, hx);
        RET("%s", hx);
    }
    if (is_trk_key(key, &t, &k)) {
        if (!strcmp(k, "state")) RET("%d", shim_state(s, t));
        if (!strcmp(k, "level") && s->muted[t - 1]) RET("%d", s->saved_level[t - 1]);
    }
#undef RET
    return mark_get_param(s->m, key, buf, buf_len);
}

static int gen_get_error(void *inst, char *buf, int buf_len) {
    (void)inst; (void)buf; (void)buf_len;
    return 0;
}

static plugin_api_v2_t api = {
    .api_version      = MOVE_PLUGIN_API_VERSION_2,
    .create_instance  = gen_create,
    .destroy_instance = gen_destroy,
    .on_midi          = gen_on_midi,
    .set_param        = gen_set_param,
    .get_param        = gen_get_param,
    .get_error        = gen_get_error,
    .render_block     = gen_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &api;
}
