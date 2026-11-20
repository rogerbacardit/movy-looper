/*
 * Movy Looper -- 6-track live looper as a Schwung sound_generator.
 *
 * Shim around the Mark looper engine (mark_core.c, shipped unmodified in
 * src/; built with -DMARK_TRACKS=6, see scripts/patch-core.sh). This file
 * translates
 *   MIDI notes (Movy drum-rack pads) -> mark_set_param() actions,
 *   hardware audio input (host mailbox) -> mark_process(),
 * and adds what the core does not have: MUTE, ARM with input threshold,
 * count-in, fixed recording length, input gain, per-track "state" enum.
 *
 * Mark engine: (c) Tim Cox, MIT (https://github.com/timncox/schwung-mark),
 * see LICENSE-mark. Ideas from Boss RC-505 / Loopex only; no Loopex code.
 */
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
    A_STOP,       /* arg = track: tap = stop, hold 1 s = clear          */
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
  /* row 2       */ { P(A_STOPALL,0,"Stop All"), P(A_STOP,1,"Stop 1"), P(A_STOP,2,"Stop 2"),
                      P(A_STOP,3,"Stop 3"), P(A_STOP,4,"Stop 4"), P(A_STOP,5,"Stop 5"),
                      P(A_STOP,6,"Stop 6"), P(A_FIXLEN,0,"Fixed Length") },
  /* row 1 (bot) */ { P(A_ALL,0,"All Start/Stop"), P(A_REC,1,"Rec 1"), P(A_REC,2,"Rec 2"),
                      P(A_REC,3,"Rec 3"), P(A_REC,4,"Rec 4"), P(A_REC,5,"Rec 5"),
                      P(A_REC,6,"Rec 6"), P(A_QUANT,0,"Quantize") },
};

#define CLEAR_HOLD_MS     1000
#define DEFAULT_THRESHOLD 8          /* percent of full scale = 0.08 */
#define NTRK              MARK_TRACKS
#define MAXBLK            128        /* core block size */

static const int REC_LEN_BARS[5] = { 0, 1, 2, 4, 8 };

/* Shim-level track state enum (tN_state), read-only for the UI. */
enum { S_EMPTY = 0, S_ARMED, S_REC, S_PLAY, S_DUB, S_STOP, S_MUTE };

static const host_api_v1_t *g_host;

typedef struct {
    mark_t *m;
    int cur_track;                 /* 1..NTRK, target of the Track page */
    int arm;                       /* auto-rec armed mode */
    int arm_down, arm_used;        /* ARM pad is a REVERSE modifier */
    int armed[NTRK];               /* waiting for the input threshold */
    int muted[NTRK];
    int saved_level[NTRK];
    long countin[NTRK];            /* frames left, -1 = none */
    long hold_frames[NTRK];        /* >=0 while STOP pad down */
    int  hold_fired[NTRK];
    long rec_frames[NTRK];         /* frames spent in REC */
    int  rec_fired[NTRK];
    int  prev_st[NTRK];
    int  threshold;                /* 1..100 percent */
    int  count_in;
    int  rec_len;                  /* index into REC_LEN_BARS */
    int  in_gain;                  /* 0..200 percent */
    int  clock_logged, audio_logged, notes_logged;
    char hierarchy[32768];
} synth_t;

static void hlog(const char *fmt, int a) {
    char b[160];
    if (!g_host || !g_host->log) return;
    snprintf(b, sizeof b, fmt, a);
    g_host->log(b);
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

static int core_state(synth_t *s, int t) { return get_t(s, t, "state"); }

static int core_pending(synth_t *s, int t) {
    char b[64] = "";
    if (mark_get_param(s->m, "tpend", b, sizeof b) < 0) return 0;
    const char *p = b;
    for (int i = 1; i < t; i++) { p = strchr(p, ','); if (!p) return 0; p++; }
    return atoi(p);
}

static double cur_bpm(synth_t *s) {
    int b = get_i(s, "bpm_override");
    if (b < 20) b = get_i(s, "grid_bpm");
    if (b < 20) b = 120;
    return (double)b;
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

static void rec_pad(synth_t *s, int t) {
    int i = t - 1;
    if (s->armed[i])        { s->armed[i] = 0; return; }       /* cancel arm */
    if (s->countin[i] >= 0) { s->countin[i] = -1; return; }    /* cancel count-in */
    if (core_state(s, t) == MK_EMPTY && !core_pending(s, t)) {
        if (s->arm) { s->armed[i] = 1; return; }
        if (s->count_in) {
            s->countin[i] = (long)((double)MARK_SR * 60.0 / cur_bpm(s) * 4.0);
            return;
        }
    }
    s->rec_fired[i] = 0;
    trig_t(s, t, "btn");
}

static void pad_down(synth_t *s, const pad_def_t *p) {
    int t = p->arg;
    if (t >= 1 && t <= NTRK) s->cur_track = t;
    switch (p->act) {
    case A_REC:
        if (s->arm_down) {                 /* ARM held + REC tap = REVERSE */
            s->arm_used = 1;
            toggle_t(s, t, "rev");
        } else rec_pad(s, t);
        break;
    case A_STOP:    s->hold_frames[t - 1] = 0; s->hold_fired[t - 1] = 0; break;
    case A_MUTE:    set_mute(s, t, !s->muted[t - 1]); break;
    case A_FX:
        if (!get_t(s, t, "fx_on") && get_t(s, t, "fx") == 0) set_t(s, t, "fx", 1);
        toggle_t(s, t, "fx_on");
        break;
    case A_ALL:     trig(s, "all_btn"); break;
    case A_STOPALL:
        for (int k = 1; k <= NTRK; k++) { clear_flags(s, k); trig_t(s, k, "stop"); }
        break;
    case A_UNDO:    trig(s, "undo"); break;
    case A_ARM:     s->arm_down = 1; s->arm_used = 0; break;
    case A_COUNTIN: s->count_in = !s->count_in; break;
    case A_RECACT:  set_i(s, "rec_action", !get_i(s, "rec_action")); break;
    case A_FIXLEN:  s->rec_len = (s->rec_len + 1) % 5; break;
    case A_QUANT:   set_i(s, "quantize", !get_i(s, "quantize")); break;
    default: break;
    }
}

static void pad_up(synth_t *s, const pad_def_t *p) {
    if (p->act == A_ARM) {
        s->arm_down = 0;
        if (!s->arm_used) {
            s->arm = !s->arm;
            if (!s->arm) for (int k = 1; k <= NTRK; k++) s->armed[k - 1] = 0;
        }
    } else if (p->act == A_STOP) {
        int i = p->arg - 1;
        if (s->hold_frames[i] >= 0 && !s->hold_fired[i]) {
            clear_flags(s, p->arg);
            trig_t(s, p->arg, "stop");
        }
        s->hold_frames[i] = -1;
    }
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

    APP("{\"pad_layout\":\"drums\",\"levels\":{\"root\":{\"name\":\"Looper\",\"params\":[");
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
        "\"options\":%s,\"default\":0}],"
        "\"knobs\":[\"fx_type\",\"fx_amt\",\"trk_pan\",\"trk_rev\",\"trk_shot\"]},",
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
    s->threshold = DEFAULT_THRESHOLD;
    s->in_gain = 100;
    for (int i = 0; i < NTRK; i++) {
        s->hold_frames[i] = -1; s->countin[i] = -1; s->saved_level[i] = 100;
    }
    mark_set_param(s->m, "hw_input", "1");
    mark_set_param(s->m, "session_dir", "/data/UserData/schwung/looper-sessions");
    build_hierarchy(s);
    hlog("movy-looper: created, host get_bpm present=%d",
         (g_host && g_host->get_bpm) ? 1 : 0);
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
    if (!s->audio_logged && raw > 0) {
        s->audio_logged = 1; hlog("movy-looper: audio_in is live (peak %d)", raw);
    }
    /* ARM: threshold crossing starts recording; count-in expiry too */
    int thr = s->threshold * 32768 / 100;
    for (int t = 1; t <= NTRK; t++) {
        int i = t - 1;
        if (s->armed[i] && peak >= thr) {
            s->armed[i] = 0; s->rec_fired[i] = 0;
            trig_t(s, t, "btn");
        }
        if (s->countin[i] >= 0) {
            s->countin[i] -= frames;
            if (s->countin[i] < 0) {
                s->countin[i] = -1; s->rec_fired[i] = 0;
                trig_t(s, t, "btn");
            }
        }
    }
    mark_process(s->m, buf, out, frames);

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
                trig_t(s, t, "btn");
            }
        }
        s->prev_st[i] = st;
    }
    /* long-press STOP = CLEAR */
    long limit = (long)MARK_SR * CLEAR_HOLD_MS / 1000;
    for (int i = 0; i < NTRK; i++) {
        if (s->hold_frames[i] < 0 || s->hold_fired[i]) continue;
        s->hold_frames[i] += frames;
        if (s->hold_frames[i] >= limit) {
            s->hold_fired[i] = 1;
            clear_flags(s, i + 1);
            trig_t(s, i + 1, "clear");
        }
    }
}

static void gen_render(void *inst, int16_t *out_lr, int frames) {
    synth_t *s = (synth_t *)inst;
    const int16_t *in = NULL;
    if (g_host && g_host->mapped_memory)
        in = (const int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    if (!in) {
        memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    for (int done = 0; done < frames; done += MAXBLK) {
        int n = frames - done < MAXBLK ? frames - done : MAXBLK;
        tick_block(s, in + done * 2, out_lr + done * 2, n);
    }
}

static void gen_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    synth_t *s = (synth_t *)inst;
    if (!s || len < 1) return;
    uint8_t st = msg[0];
    if (st == 0xF8 && !s->clock_logged) {
        s->clock_logged = 1; hlog("movy-looper: first MIDI clock received (src %d)", source);
    }
    if (len >= 2 && source != MOVE_MIDI_SOURCE_FX_BROADCAST) {
        int hi = st & 0xF0;
        if (hi == 0x90 || hi == 0x80) {
            const pad_def_t *p = pad_for_note(msg[1]);
            int on = (hi == 0x90) && len >= 3 && msg[2] > 0;
            if (on && s->notes_logged < 8) {
                s->notes_logged++;
                hlog("movy-looper: note-on %d", msg[1]);
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
    int v = atoi(val);
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
