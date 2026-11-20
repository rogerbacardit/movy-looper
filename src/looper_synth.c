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
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define DEFAULT_THRESHOLD 8          /* percent of full scale = 0.08 */
#define NTRK              MARK_TRACKS
#define MAXBLK            128        /* core block size */

static const int REC_LEN_BARS[5] = { 0, 1, 2, 4, 8 };

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
    int  threshold;                /* 1..100 percent */
    int  count_in;
    int  rec_len;                  /* index into REC_LEN_BARS */
    int  in_gain;                  /* 0..200 percent */
    int  clock_logged, audio_logged, notes_logged;
    long in_frames;                /* frames rendered so far (diagnostics) */
    int  in_peak_1s;               /* raw input peak over the first second */
    int  in_diag_done;             /* the "peak after 1 s" line was logged */
    int  in_peak_now;              /* raw peak of the last block (get_param in_peak) */
    int  bpm_src_logged;           /* last tempo source logged (-1 = none yet) */
    long pend_frames[NTRK];        /* frames a track has been waiting on the grid */
    int  pend_logged;
    unsigned status_seq;           /* bumps on every status_text change */
    char status_text[40];          /* last action, for Movy's toast */
    /* display_param cache: the peaks are rescanned only when the clip can have
     * changed (a pad was pressed, the state/length moved, or a refresh timer
     * ran); the playhead is read fresh every call. */
    uint8_t  wave[WAVE_BINS];
    int      wave_track, wave_st, wave_dirty;
    uint32_t wave_span;
    long     wave_at;
    unsigned wave_seq;
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
    case A_FIXLEN:
        s->rec_len = (s->rec_len + 1) % 5;
        if (s->rec_len == 0) say(s, "LEN FREE");
        else if (REC_LEN_BARS[s->rec_len] == 1) say(s, "LEN 1 BAR");
        else say(s, "LEN %d BARS", REC_LEN_BARS[s->rec_len]);
        break;
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
static const uint8_t FIXLEN_LEVEL[5]  = { L_DARK_GREY, L_GREY_40, L_LIGHT_GREY,
                                          L_OFF_WHITE, L_WHITE };

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
        if (st == MK_EMPTY)     return n->pend[i] ? (LED_BLINK | TRK_BRIGHT[i]) : TRK_VDIM[i];
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
    case A_FIXLEN:  return FIXLEN_LEVEL[clampi_(s->rec_len, 0, 4)];
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
    char hdr[48], hx[WAVE_BINS * 2 + 1];
    header_text(s, t, span, hdr, sizeof hdr);
    for (int i = 0; i < WAVE_BINS; i++) snprintf(hx + i * 2, 3, "%02X", s->wave[i]);
    snprintf(out, n, "%u|%d|%s|%s", s->wave_seq, pos, span ? hx : "", hdr);
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

    APP("{\"pad_layout\":\"drums\",\"pad_leds_param\":\"pad_leds\","
        "\"status_text_param\":\"status_text\",\"display_param\":\"display\",\"delete_pad_param\":\"delete_pad\",\"levels\":{\"root\":{\"name\":\"Looper 606\",\"params\":[");
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
        "\"min\":1,\"max\":100,\"default\":%d,\"step\":1,\"unit\":\"%%\"},"
        "{\"key\":\"quantize\",\"name\":\"Quantize\",\"short_name\":\"QNT\",\"type\":\"enum\","
        "\"options\":%s,\"default\":1},"
        "{\"key\":\"rec_len\",\"name\":\"Fixed Length\",\"short_name\":\"LEN\",\"type\":\"enum\","
        "\"options\":[\"Free\",\"1 bar\",\"2 bars\",\"4 bars\",\"8 bars\"],\"default\":0},"
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
        DEFAULT_THRESHOLD, onoff, onoff, onoff, onoff);

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
    mark_set_param(s->m, "session_dir", "/data/UserData/schwung/looper-sessions");
    build_hierarchy(s);
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
    int thr = s->threshold * 32768 / 100;
    for (int t = 1; t <= NTRK; t++) {
        int i = t - 1;
        if (s->armed[i] && peak >= thr) {
            s->armed[i] = 0; s->rec_fired[i] = 0;
            say_rec(s, t);
            trig_t(s, t, "btn");
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
    int bars = REC_LEN_BARS[s->rec_len];
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
     * track is in use we add a +-6 LSB Nyquist dither (about -74 dBFS). */
    {
        int active = 0;
        for (int t = 1; t <= NTRK && !active; t++)
            if (core_state(s, t) != MK_EMPTY || s->armed[t - 1] || s->countin[t - 1] >= 0)
                active = 1;
        if (active)
            for (int n = 0; n < frames; n++) {
                int d = (n & 1) ? -6 : 6, l = out[n * 2] + d, r = out[n * 2 + 1] + d;
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
    if (!strcmp(key, "threshold")) { s->threshold = clampi(v, 1, 100); return; }
    if (!strcmp(key, "count_in"))  { s->count_in = v ? 1 : 0; return; }
    if (!strcmp(key, "rec_len"))   { s->rec_len = clampi(v, 0, 4); return; }
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
