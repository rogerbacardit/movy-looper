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
#define NOTE_BASE      36
#define RAW_PAD_FIRST  68

enum act {
    A_NONE = 0,
    A_REC,        /* arg = track: REC->PLAY->DUB (per Rec Action)      */
    A_STOP,       /* arg = track: row 2 PLAY/STOP                         */
    A_MUTE,       /* arg = track: mute (keeps running silently)         */
    A_FX,         /* arg = track: track FX on/off                       */
    A_ALL,        /* all start/stop                                     */
    A_STOPALL,    /* stop every track                                   */
    A_UNDO,       /* undo / redo                                        */
    A_ARM,        /* arm auto-rec toggle; held = reverse modifier       */
    A_COUNTIN,    /* count-in on/off                                    */
    A_RECACT,     /* rec action: rec->play / rec->dub                   */
    A_FIXLEN,     /* fixed length: free/1/2/4/8 bars (cycles)           */
    A_QUANT       /* quantize on/off                                    */
};

typedef struct { uint8_t act, arg; const char *label; } pad_def_t;
#define P(a, n, l) { (a), (n), (l) }

static const pad_def_t PAD_GRID[4][8] = {
  /* row 4 (top) */ { P(A_UNDO,0,"Undo/Redo"), P(A_FX,1,"FX 1"), P(A_FX,2,"FX 2"),
                      P(A_FX,3,"FX 3"), P(A_FX,4,"FX 4"), P(A_FX,5,"FX 5"),
                      P(A_FX,6,"FX 6"), P(A_COUNTIN,0,"Count-in") },
  /* row 3       */ { P(A_ARM,0,"Arm/Reverse"), P(A_MUTE,1,"Mute 1"), P(A_MUTE,2,"Mute 2"),
                      P(A_MUTE,3,"Mute 3"), P(A_MUTE,4,"Mute 4"), P(A_MUTE,5,"Mute 5"),
                      P(A_MUTE,6,"Mute 6"), P(A_RECACT,0,"Rec Action") },
  /* row 2       */ { P(A_STOPALL,0,"Stop All"), P(A_STOP,1,"Play/Stop 1"), P(A_STOP,2,"Play/Stop 2"),
                      P(A_STOP,3,"Play/Stop 3"), P(A_STOP,4,"Play/Stop 4"), P(A_STOP,5,"Play/Stop 5"),
                      P(A_STOP,6,"Play/Stop 6"), P(A_FIXLEN,0,"Fixed Length") },
  /* row 1 (bot) */ { P(A_ALL,0,"All Start/Stop"), P(A_REC,1,"Rec/Dub 1"), P(A_REC,2,"Rec/Dub 2"),
                      P(A_REC,3,"Rec/Dub 3"), P(A_REC,4,"Rec/Dub 4"), P(A_REC,5,"Rec/Dub 5"),
                      P(A_REC,6,"Rec/Dub 6"), P(A_QUANT,0,"Quantize") },
};

#define DEFAULT_THRESHOLD (-42)      /* dBFS (~260/32768): above line-in noise (~-66), below quiet playing */
#define NTRK              MARK_TRACKS
#define MAXBLK            128        /* core block size */

/* Fixed recording length is a number of bars, 0 = free, 1..MAX_BARS. The pad in
 * column 8 cycles free -> 1 -> 2 -> 4 -> 8 -> free; the 16 step buttons pick
 * any of 1..16 directly (pressing the chosen one again = free). */
#define MAX_BARS 16
static const int CYCLE_BARS[4] = { 1, 2, 4, 8 };

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
    int      wave_track, wave_st, wave_dirty;
    uint32_t wave_span;
    long     wave_at;
    unsigned wave_seq;
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
static void set_rec_bars(synth_t *s, int bars) {
    s->rec_len = clampi_(bars, 0, MAX_BARS);
    if (s->rec_len == 0) say(s, "LEN FREE");
    else if (s->rec_len == 1) say(s, "LEN 1 BAR");
    else say(s, "LEN %d BARS", s->rec_len);
}

/* Step button N (0-based) = a fixed length of N+1 bars; the chosen one again = free. */
static void step_press(synth_t *s, int button) {
    if (button < 0 || button >= MAX_BARS) return;
    set_rec_bars(s, s->rec_len == button + 1 ? 0 : button + 1);
}

/* Loop button + knob N (1-based): move the clip start of track N by `delta`
 * grid units (a 1/16 note at the current tempo; fine = 1/64 note). The start
 * is non-destructive: the engine plays (pos + start) mod len and dubs at the
 * same place, so layers stay aligned. */
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

static void loop_knob(synth_t *s, const char *val) {
    int n = 0, d = 0, fine = 0;
    if (sscanf(val, "%d:%d:%d", &n, &d, &fine) < 2) return;
    if (n < 1 || n > NTRK || d == 0) return;
    int t = n;
    s->cur_track = t;
    s->wave_dirty = 1;
    int st = core_state(s, t);
    if (st == MK_EMPTY) { say(s, "START %d EMPTY", t); return; }
    s->start_t = t;
    s->start_until = s->clock_frames + (long)MARK_SR * 5 / 2;
    if (st == MK_REC || st == MK_DUB) { say(s, "START %d LOCKED", t); return; }
    long len = get_t(s, t, "len");
    if (len <= 0) return;
    d = clampi_(d, -16, 16);
    double u = (double)MARK_SR * 60.0 / cur_bpm(s) / (fine ? 16.0 : 4.0);
    long k = lround((double)get_t(s, t, "off") / u) + d;
    long off = lround((double)k * u) % len;
    if (off < 0) off += len;
    if (off > len - 256) off = 0;      /* a detent at the loop start: the grid and the loop length differ by a few frames */
    set_t(s, t, "off", (int)off);
    char lab[32];
    sixteenths_label(s, off, lab, sizeof lab);
    say(s, "START %d %s", t, lab);
}

static void rec_pad(synth_t *s, int t) {
    int i = t - 1;
    if (s->armed[i])        { s->armed[i] = 0; say(s, "CANCEL %d", t); return; }
    if (s->countin[i] >= 0) { s->countin[i] = -1; say(s, "CANCEL %d", t); return; }
    if (core_state(s, t) == MK_EMPTY && !core_pending(s, t)) {
        if (s->arm) { s->armed[i] = 1; say(s, "ARMED %d", t); return; }
        if (s->count_in) {
            s->countin[i] = (long)((double)MARK_SR * 60.0 / cur_bpm(s) * 4.0);
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

static void pad_down(synth_t *s, const pad_def_t *p) {
    int t = p->arg;
    s->wave_dirty = 1;
    if (t >= 1 && t <= NTRK) s->cur_track = t;
    switch (p->act) {
    case A_REC:                            /* row 1: REC / DUB (ARM held = reverse) */
        if (s->arm_held) { s->arm_used = 1; arm_reverse(s, t); break; }
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
        set_mute(s, t, !s->muted[t - 1]);
        say(s, s->muted[t - 1] ? "MUTE %d" : "UNMUTE %d", t);
        break;
    case A_FX:
        if (!get_t(s, t, "fx_on") && get_t(s, t, "fx") == 0) set_t(s, t, "fx", 1);
        toggle_t(s, t, "fx_on");
        say(s, get_t(s, t, "fx_on") ? "FX %d ON" : "FX %d OFF", t);
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
    case A_UNDO:    say(s, "UNDO/REDO"); trig(s, "undo"); break;
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
    case A_FIXLEN: {                       /* cycle free -> 1 -> 2 -> 4 -> 8 -> free */
        int next = 0;
        for (int i = 0; i < 4; i++) if (CYCLE_BARS[i] > s->rec_len) { next = CYCLE_BARS[i]; break; }
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
static void delete_pad(synth_t *s, int idx) {
    const pad_def_t *p = pad_for_note(NOTE_BASE + idx);
    if (!p || p->arg < 1 || p->arg > NTRK) return;
    int t = p->arg;
    s->cur_track = t;
    s->wave_dirty = 1;
    clear_flags(s, t);
    say(s, "CLEAR %d", t);
    trig_t(s, t, "clear");
}

static void pad_up(synth_t *s, const pad_def_t *p) {
    if (p->act != A_ARM || !s->arm_held) return;
    s->arm_held = 0;
    if (s->arm_used) { s->arm_used = 0; return; }   /* was only the modifier */
    s->arm = !s->arm;
    if (!s->arm) for (int k = 1; k <= NTRK; k++) s->armed[k - 1] = 0;
    say(s, s->arm ? "ARM ON" : "ARM OFF");
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
 * Track identity: blue, cyan, yellow, magenta, white, orange.
 * ARM / armed = orange blinking; DUB = violet; mute = grey. */
static const uint8_t TRK_BRIGHT[NTRK] = { 125,  14,   7,  23, 122,   3 };
static const uint8_t TRK_DIM[NTRK]    = {  99,  91,  77, 109, 123,  72 };
static const uint8_t TRK_VDIM[NTRK]   = {  98,  92,  74, 110, 124,  69 };
/* Brightness of the fixed-length pad: free .. 1 .. 2 .. 3-4 .. 5+ bars. */
static uint8_t fixlen_level(int bars) {
    return bars <= 0 ? L_DARK_GREY : bars == 1 ? L_GREY_40 : bars == 2 ? L_LIGHT_GREY
         : bars <= 4 ? L_OFF_WHITE : L_WHITE;
}

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
        case MK_DUB:  return L_PURPLE;
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
        if (s->muted[i]) return L_DARK_GREY;
        return n->st[i] == MK_EMPTY ? TRK_VDIM[i] : TRK_BRIGHT[i];
    case A_FX:      return n->fx_on[i] ? TRK_BRIGHT[i] : TRK_VDIM[i];
    case A_ALL:     return n->any_run ? L_WHITE : L_GREY_40;
    case A_STOPALL: return n->any_run ? L_BRIGHT_YELLOW : L_DARK_GREY;
    case A_UNDO:    return L_ROYAL_BLUE;
    case A_ARM:     return s->arm ? (LED_BLINK | L_ORANGE) : L_DARK_GREY;
    case A_COUNTIN: return s->count_in ? L_YELLOW : L_DIM_YELLOW;
    case A_RECACT:  return n->rec_action ? L_PURPLE : L_DARK_ROYAL_BLUE;
    case A_FIXLEN:  return fixlen_level(s->rec_len);
    case A_QUANT:   return n->quant ? L_CYAN : L_DIM_CYAN;
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
    for (int i = 0; i < 16; i++) {
        int c = L_DARK_GREY;
        if (s->rec_len > 0) {
            if (i + 1 == s->rec_len) c = L_WHITE;
            else if (i + 1 < s->rec_len) c = L_GREY_40;
        }
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
static void header_text(synth_t *s, int t, uint32_t span, char *out, size_t n) {
    static const char *const NAME[7] = { "EMPTY", "ARMED", "REC", "PLAY", "DUB", "STOP", "MUTE" };
    int st = shim_state(s, t);
    if (s->start_t == t && s->clock_frames < s->start_until && span) {   /* clip start readout */
        char lab[32];
        if (st == S_REC || st == S_DUB) snprintf(out, n, "START %d LOCKED", t);
        else { sixteenths_label(s, get_t(s, t, "off"), lab, sizeof lab); snprintf(out, n, "START %d %s", t, lab); }
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

static void build_display(synth_t *s, char *out, size_t n) {
    int t = s->cur_track, st = shim_state(s, t);
    uint32_t span = 0; int pos = 0;
    mark_track_wave(s->m, t - 1, WAVE_BINS, NULL, &span, &pos);
    int busy = (st == S_REC || st == S_DUB);
    long every = busy ? MARK_SR / 4 : MARK_SR * 2;
    if (s->wave_dirty || t != s->wave_track || st != s->wave_st || span != s->wave_span
        || s->in_frames - s->wave_at >= every) {
        uint8_t w[WAVE_BINS];
        mark_track_wave(s->m, t - 1, WAVE_BINS, w, &span, &pos);
        if (t != s->wave_track || span != s->wave_span || memcmp(w, s->wave, sizeof w)) {
            memcpy(s->wave, w, sizeof w);
            s->wave_seq = (s->wave_seq + 1) % 100000u;
        }
        s->wave_track = t; s->wave_st = st; s->wave_span = span;
        s->wave_at = s->in_frames; s->wave_dirty = 0;
    }
    char hdr[48], hx[WAVE_BINS * 2 + 1], posf[24];
    header_text(s, t, span, hdr, sizeof hdr);
    for (int i = 0; i < WAVE_BINS; i++) snprintf(hx + i * 2, 3, "%02X", s->wave[i]);
    /* pos field: "<pos>" or "<pos>,<start marker column>" (marker shown when the
     * clip start is moved, or while it is being adjusted) */
    int mark = span ? mark_track_start_bin(s->m, t - 1, WAVE_BINS) : -1;
    int adjusting = s->start_t == t && s->clock_frames < s->start_until;
    if (mark >= 0 && (adjusting || get_t(s, t, "off") != 0)) snprintf(posf, sizeof posf, "%d,%d", pos, mark);
    else snprintf(posf, sizeof posf, "%d", pos);
    snprintf(out, n, "%u|%s|%s|%s", s->wave_seq, posf, span ? hx : "", hdr);
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
        "\"15 bars\",\"16 bars\"]";

    APP("{\"pad_layout\":\"drums\",\"pad_leds_param\":\"pad_leds\","
        "\"status_text_param\":\"status_text\",\"display_param\":\"display\",\"delete_pad_param\":\"delete_pad\","
        "\"step_buttons_param\":\"step\",\"step_leds_param\":\"step_leds\",\"loop_knob_param\":\"loop_knob\","
        "\"levels\":{\"root\":{\"name\":\"Looper 606\",\"params\":[");
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
        "\"options\":[\"1/2x\",\"1x\",\"2x\"],\"default\":1}],"
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
    int w = snprintf(buf + n, (size_t)(buf_len - n),
        ",\"l6id\":\"%s\",\"l6rl\":%d,\"l6ci\":%d,\"l6thdb\":%d,\"l6ig\":%d,"
        "\"l6sp\":\"%s\",\"l6mu\":\"%s\",\"l6lv\":\"%s\"}",
        loop_id_now(s), s->rec_len, s->count_in, s->threshold, s->in_gain, sp, mu, lv);
    if (w < 0 || w >= buf_len - n) { buf[n] = '}'; buf[n + 1] = 0; return n + 1; }
    return n + w;
}

static void state_set(synth_t *s, const char *val) {
    int fresh = 1;                                 /* nothing recorded yet on this instance */
    for (int t = 1; t <= NTRK; t++)
        if (core_state(s, t) != MK_EMPTY || s->armed[t - 1] || s->countin[t - 1] >= 0) fresh = 0;
    mark_set_param(s->m, "state", val);
    if (strstr(val, "\"l6rl\":")) {                 /* written by state_get below */
        s->rec_len   = clampi_((int)jnum(val, "l6rl", s->rec_len), 0, MAX_BARS);
        s->count_in  = jnum(val, "l6ci", s->count_in) ? 1 : 0;
        s->threshold = clampi_((int)jnum(val, "l6thdb", s->threshold), -70, -6);
        s->in_gain   = clampi_((int)jnum(val, "l6ig", s->in_gain), 0, 200);
        for (int t = 1; t <= NTRK; t++) {
            set_t(s, t, "spd", clampi_(jcsv(val, "l6sp", t - 1, 1), 0, 2));
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
    s->bpm_src_logged = -1;
    s->threshold = DEFAULT_THRESHOLD;
    s->in_gain = 100;
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
    mark_destroy(s->m);
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
    s->clock_frames += frames;
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

static void gen_set_param(void *inst, const char *key, const char *val) {
    synth_t *s = (synth_t *)inst;
    int t; const char *k;
    if (!s) return;
    s->wave_dirty = 1;
    int v = atoi(val);
    if (!strcmp(key, "delete_pad")) { if (v >= 0 && v < 32) delete_pad(s, v); return; }
    if (!strcmp(key, "cur_track")) { s->cur_track = clampi(v, 1, NTRK); return; }
    if (!strcmp(key, "threshold")) { s->threshold = clampi(v, -70, -6); return; }
    if (!strcmp(key, "count_in"))  { s->count_in = v ? 1 : 0; return; }
    if (!strcmp(key, "rec_len"))   { s->rec_len = clampi(v, 0, MAX_BARS); return; }
    if (!strcmp(key, "step"))      { step_press(s, v); return; }
    if (!strcmp(key, "loop_knob")) { loop_knob(s, val); return; }
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
    if (!strcmp(key, "trk_spd"))  { set_t(s, s->cur_track, "spd", clampi(v, 0, 2)); return; }
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
    if (!strcmp(key, "fx_type"))  RET("%d", get_t(s, s->cur_track, "fx"));
    if (!strcmp(key, "fx_amt"))   RET("%d", get_t(s, s->cur_track, "fxp"));
    if (!strcmp(key, "trk_pan"))  RET("%d", get_t(s, s->cur_track, "pan"));
    if (!strcmp(key, "trk_rev"))  RET("%d", get_t(s, s->cur_track, "rev"));
    if (!strcmp(key, "trk_shot")) RET("%d", get_t(s, s->cur_track, "shot"));
    if (!strcmp(key, "trk_spd"))  RET("%d", get_t(s, s->cur_track, "spd"));
    if (!strcmp(key, "in_peak"))      RET("%d", s->in_peak_now);
    if (!strcmp(key, "status_text"))  RET("%u:%s", s->status_seq, s->status_text);
    if (!strcmp(key, "display")) {
        char d[WAVE_BINS * 2 + 96];
        build_display(s, d, sizeof d);
        RET("%s", d);
    }
    if (!strcmp(key, "step_leds")) {
        char hx[33];
        build_step_leds(s, hx);
        RET("%s", hx);
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
