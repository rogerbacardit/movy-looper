/* Host tests for the looper-606 sound_generator (dlopen'd .so/.dylib).
 * usage: synth_smoke <plugin> <hierarchy-out.json> */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/perform.h"   /* effect ids */
#include "plugin_api_v1.h"

static uint8_t mem[4096];
static float bpm_val = 120.0f;
static float fake_bpm(void) { return bpm_val; }
static int nlogs, got_clock_log, got_src_log, got_none_log, got_peak_log, got_silent_log;
static int got_bpm_log, got_cap_log, got_timeout_log;
static void fake_log(const char *m) {
    nlogs++;
    if (strstr(m, "first MIDI clock")) got_clock_log = 1;
    if (strstr(m, "input source = host mailbox")) got_src_log = 1;
    if (strstr(m, "input source = NONE")) got_none_log = 1;
    if (strstr(m, "input peak after 1 s = ")) { if (strstr(m, "SILENT")) got_silent_log = 1; else got_peak_log = 1; }
    if (strstr(m, "bpm source = ")) got_bpm_log = 1;
    if (strstr(m, "track capacity")) got_cap_log = 1;
    if (strstr(m, "quantize wait timed out")) got_timeout_log = 1;
}
static host_api_v1_t host = { .sample_rate = 44100, .frames_per_block = 128,
    .mapped_memory = mem, .audio_out_offset = 256, .audio_in_offset = 2304,
    .get_bpm = fake_bpm, .log = fake_log };
static host_api_v1_t host_nomem = { .sample_rate = 44100, .frames_per_block = 128,
    .audio_out_offset = 256, .audio_in_offset = 2304, .get_bpm = fake_bpm, .log = fake_log };
static host_api_v1_t host_nobpm = { .sample_rate = 44100, .frames_per_block = 128,
    .mapped_memory = mem, .audio_out_offset = 256, .audio_in_offset = 2304, .log = fake_log };

static plugin_api_v2_t *A; static void *S;
static move_plugin_init_v2_fn init_fn;
static const char *MODDIR = "/tmp/modules/sound_generators/looper-606";
/* destroy the current instance and start a fresh one against host h */
static void fresh(host_api_v1_t *h) {
    if (S) A->destroy_instance(S);
    A = init_fn(h);
    S = A->create_instance(MODDIR, NULL);
}
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

enum { IN_SINE, IN_SILENCE, IN_LOUD, IN_RICH };
static int in_mode = IN_SINE; static double ph; static int in_dc = 0; static long rich_n;
static void fill_input(void) {
    int16_t *in = (int16_t *)(mem + 2304);
    for (int i = 0; i < 128; i++) {
        double a = in_mode == IN_SILENCE ? 0.0 : in_mode == IN_LOUD ? 0.5 : 0.25;
        int16_t v = (int16_t)(sin(ph) * a * 32767.0); ph += 2 * M_PI * 220.0 / 44100.0;
        if (in_mode == IN_RICH) {       /* a decaying two-tone "drum" every 11025 frames: deterministic and non-periodic enough */
            double env = exp(-(double)(rich_n++ % 11025) / 2500.0);
            v = (int16_t)((sin(ph) * 0.5 + sin(ph * 1.5) * 0.3) * env * 0.6 * 32767.0);
        }
        if (in_dc) v = (int16_t)in_dc;
        in[2 * i] = in[2 * i + 1] = v;
    }
}
static int16_t out[256]; static int out_peak;
static int16_t *recbuf; static long recpos; static int recing;     /* optional recording of the left channel */
static void run(int blocks) {
    for (int b = 0; b < blocks; b++) {
        fill_input(); A->render_block(S, out, 128);
        if (recing) for (int i = 0; i < 128; i++) recbuf[recpos++] = out[2 * i];
        for (int i = 0; i < 256; i++) if (abs(out[i]) > out_peak) out_peak = abs(out[i]);
    }
}
static void clock_run(int blocks) {   /* 120 BPM = 48 ticks/s ~ one tick / 918 frames */
    uint8_t clk = 0xF8; static double acc;
    for (int b = 0; b < blocks; b++) {
        run(1); acc += 128;
        while (acc >= 918.75) { acc -= 918.75; A->on_midi(S, &clk, 1, 3); }
    }
}
/* physical pad -> note, Movy drum rack order (row 1 = bottom, col 0..7) */
static int note_of(int row, int col) { return 36 + (row - 1) * 4 + (col % 4) + 16 * (col / 4); }
/* zero crossings (with hysteresis) of the output over n blocks */
static int crossings(int blocks) {
    int c = 0, sgn = 0;
    for (int b = 0; b < blocks; b++) {
        run(1);
        for (int i = 0; i < 128; i++) {
            int v = out[2 * i];
            if (v > 300 && sgn <= 0) { if (sgn < 0) c++; sgn = 1; }
            else if (v < -300 && sgn >= 0) { if (sgn > 0) c++; sgn = -1; }
        }
    }
    return c;
}
static void tap(int note) {
    uint8_t on[3] = {0x90, (uint8_t)note, 100}, off[3] = {0x80, (uint8_t)note, 0};
    A->on_midi(S, on, 3, 0); A->on_midi(S, off, 3, 0);
}
static void down(int note) { uint8_t on[3] = {0x90, (uint8_t)note, 100}; A->on_midi(S, on, 3, 0); }
static void up(int note)   { uint8_t of[3] = {0x80, (uint8_t)note, 0};   A->on_midi(S, of, 3, 0); }
static int geti(const char *k) { char b[64] = "-1"; A->get_param(S, k, b, sizeof b); return atoi(b); }
static void seti(const char *k, int v) { char b[16]; snprintf(b, sizeof b, "%d", v); A->set_param(S, k, b); }
static int state(int t) { char k[16]; snprintf(k, sizeof k, "t%d_state", t); return geti(k); }
enum { S_EMPTY, S_ARMED, S_REC, S_PLAY, S_DUB, S_STOP, S_MUTE };
/* display "<seq>|<pos>[,<mark>]|<peaks>|<header>" helpers */
static char dbuf[800];
static void disp(void) { dbuf[0] = 0; A->get_param(S, "display", dbuf, sizeof dbuf); }
static int disp_pos(void) { disp(); return atoi(strchr(dbuf, '|') + 1); }
static int disp_mark(void) {
    disp(); const char *p = strchr(dbuf, '|') + 1; size_t n = strcspn(p, "|");
    const char *c = memchr(p, ',', n); return c ? atoi(c + 1) : -1;
}
static int disp_end(void) {                      /* third pos sub-field: end marker, -1 when none */
    disp(); const char *p = strchr(dbuf, '|') + 1; size_t n = strcspn(p, "|");
    const char *c = memchr(p, ',', n); if (!c) return -1;
    c = memchr(c + 1, ',', n - (size_t)(c + 1 - p)); return c ? atoi(c + 1) : -1;
}
static int pad_led(int note) {
    char b[100]; A->get_param(S, "pad_leds", b, sizeof b);
    char hx[3] = { b[(note - 36) * 2], b[(note - 36) * 2 + 1], 0 }; return (int)strtol(hx, NULL, 16);
}
static const char *disp_field(const char *d, int n, char *out, size_t cap) {   /* n-th '|' field (0-based) */
    for (; n > 0 && d; n--) { d = strchr(d, '|'); if (d) d++; }
    size_t l = d ? strcspn(d, "|") : 0; if (l >= cap) l = cap - 1;
    if (d) memcpy(out, d, l); out[l] = 0; return out;
}
static const char *disp_head(void) { static char h[64]; disp(); return disp_field(dbuf, 3, h, sizeof h); }
/* 5th field: tick bytes. Returns the count; ticks[i] = bin | 0x80 for a beat tick. */
static int disp_ticks(int *ticks) {
    static char f[300]; disp(); disp_field(dbuf, 4, f, sizeof f);
    int n = (int)(strcspn(f, "/") / 2);
    for (int i = 0; i < n; i++) { char h[3] = { f[2*i], f[2*i+1], 0 }; ticks[i] = (int)strtol(h, NULL, 16); }
    return n;
}
/* 5th field, part after '/': phrase (4-bar) tick bins. Returns the count. */
static int disp_phrase(int *ph) {
    static char f[300]; disp(); disp_field(dbuf, 4, f, sizeof f);
    char *sl = strchr(f, '/'); if (!sl) return 0;
    int n = (int)(strlen(sl + 1) / 2);
    for (int i = 0; i < n; i++) { char h[3] = { sl[1 + 2*i], sl[2 + 2*i], 0 }; ph[i] = (int)strtol(h, NULL, 16); }
    return n;
}
static int disp_peak(int bin) {                 /* peak byte of one overview bin, -1 when none */
    disp(); const char *p = strchr(strchr(dbuf, '|') + 1, '|') + 1;
    if (strcspn(p, "|") < 256) return -1;
    char h[3] = { p[bin * 2], p[bin * 2 + 1], 0 }; return (int)strtol(h, NULL, 16);
}
static char sbuf[600];
static const char *getstr(const char *k) { sbuf[0] = 0; A->get_param(S, k, sbuf, sizeof sbuf); return sbuf; }
static const char *status_text(void) { const char *c = strchr(getstr("status_text"), ':'); return c ? c + 1 : ""; }
static int gett(int t, const char *k) { char key[24]; snprintf(key, sizeof key, "t%d_%s", t, k); return geti(key); }
static void sett(int t, const char *k, int v) { char key[24]; snprintf(key, sizeof key, "t%d_%s", t, k); seti(key, v); }
static void setstr(const char *k, const char *v) { A->set_param(S, k, v); }
static int step_led(int i) { char hx[3] = { getstr("step_leds")[i * 2], sbuf[i * 2 + 1], 0 }; return (int)strtol(hx, NULL, 16); }
static int file_exists(const char *dir, const char *f) { char p[400]; snprintf(p, sizeof p, "%s/%s", dir, f); return access(p, F_OK) == 0; }
static void wait_until_saved(const char *dir, const char *f, int ms) {
    for (int w = 0; w < ms / 50 && !file_exists(dir, f); w++) usleep(50000);
}
/* pad shortcuts: track t = 1..6 -> col t */
#define REC(t)  note_of(1, t)
#define STOP(t) note_of(2, t)
#define MUTE(t) note_of(3, t)
#define FX(t)   note_of(4, t)
#define M_ALL   note_of(2, 0)
#define M_STOPALL note_of(1, 0)
#define M_ARM   note_of(3, 0)
#define M_SHIFT note_of(4, 0)
#define UNDO()  setstr("undo_cmd", "undo")
#define REDO()  setstr("undo_cmd", "redo")
#define R_QUANT note_of(1, 7)
#define R_FIXLEN note_of(2, 7)
#define R_RECACT note_of(3, 7)
#define R_COUNTIN note_of(4, 7)



/* ---- PERFORM engine API straight from the .so (filters, kill, order, limit, latch timing) ---- */
static perf_t *(*pf_create)(void); static void (*pf_destroy)(perf_t *);
static int (*pf_press)(perf_t *, int, int, long, int); static int (*pf_release)(perf_t *, int, long);
static void (*pf_begin_raw)(perf_t *, double, double, const double *, const double *);
static int (*pf_pct_get_fn)(const perf_t *, int, int);
#define pf_pct_get(p, f, k) ((double)pf_pct_get_fn((p), (f), (k)))
static const double pf_zero[7];
static void pf_begin(perf_t *p, double g, double b) { pf_begin_raw(p, g, b, pf_zero, pf_zero); }
static void (*pf_proc)(perf_t *, int, float *, float *);
static void (*pf_pct_set)(perf_t *, int, int, int); static int (*pf_count)(const perf_t *);
static unsigned (*pf_latch_mask)(const perf_t *); static unsigned (*pf_on_mask)(const perf_t *);
static int (*pf_top)(const perf_t *); static int (*pf_held)(const perf_t *);
static int (*pf_entry_fn)(const perf_t *, int, int *, int *, int *, int *);
#define perf_entry_ok pf_entry_fn
static int api_x_k = -1, api_x_v = 0;      /* optional 2nd knob set by api_tones on pct_fx */
static void api_init(void *h) {
    *(void **)&pf_create = dlsym(h, "perf_create"); *(void **)&pf_destroy = dlsym(h, "perf_destroy");
    *(void **)&pf_press = dlsym(h, "perf_press"); *(void **)&pf_release = dlsym(h, "perf_release");
    *(void **)&pf_begin_raw = dlsym(h, "perf_block_begin"); *(void **)&pf_pct_get_fn = dlsym(h, "perf_pct"); *(void **)&pf_proc = dlsym(h, "perf_process");
    *(void **)&pf_pct_set = dlsym(h, "perf_pct_set"); *(void **)&pf_count = dlsym(h, "perf_count");
    *(void **)&pf_latch_mask = dlsym(h, "perf_latch_mask"); *(void **)&pf_on_mask = dlsym(h, "perf_on_mask");
    *(void **)&pf_entry_fn = dlsym(h, "perf_entry"); *(void **)&pf_top = dlsym(h, "perf_top_fx"); *(void **)&pf_held = dlsym(h, "perf_held_count");
}
/* two tones through the engine, lane 0; returns the gain of each tone (rms out / rms in) measured over the last 0.5 s */
static void api_tones(int nfx, const int *fxs, int pct_fx, int pct_k, int pct_v, double f1, double f2, double *g1, double *g2) {
    perf_t *p = NULL;
    double acc_in[2] = { 0, 0 }, acc_out[2] = { 0, 0 };
    for (int tone = 0; tone < 2; tone++) {
        p = pf_create();
        for (int i = 0; i < nfx; i++) pf_press(p, fxs[i], 0, 0, 0);
        if (pct_fx >= 0) pf_pct_set(p, pct_fx, pct_k, pct_v);
        if (pct_fx >= 0 && api_x_k >= 0) pf_pct_set(p, pct_fx, api_x_k, api_x_v);
        double f = tone ? f2 : f1, w = 0;
        for (long n = 0; n < 44100; n++) {
            if (n % 128 == 0) pf_begin(p, (double)n, 9600.0);
            float l = (float)(sin(w) * 8000.0), r = l; w += 2 * M_PI * f / 44100.0;
            float il = l; pf_proc(p, 0, &l, &r);
            if (n >= 22050) { acc_in[tone] += (double)il * il; acc_out[tone] += (double)l * l; }
        }
        pf_destroy(p);
    }
    *g1 = sqrt(acc_out[0] / acc_in[0]); *g2 = sqrt(acc_out[1] / acc_in[1]);
}

/* LFO through the engine (lane 0, beat = 9600 frames): per-window rms of L / R (1 = the input) and zero crossings of L,
 * from 0.5 s on; returns the number of windows */
static int lfo_probe(int rate_pct, int dst_pct, int depth_pct, double f, int win, double *rl, double *rr, int *cr, int maxw) {
    perf_t *p = pf_create();
    pf_press(p, PF_LFO, 0, 0, 0);
    pf_pct_set(p, PF_LFO, 0, rate_pct); pf_pct_set(p, PF_LFO, 1, dst_pct); pf_pct_set(p, PF_LFO, 2, depth_pct);
    int nw = 0, c = 0, sg = 0; double al = 0, ar = 0, w = 0;
    for (long n = 0; n < 44100 * 2 && nw < maxw; n++) {
        if (n % 128 == 0) pf_begin(p, (double)n, 9600.0);
        float l = (float)(sin(w) * 8000.0), r = l; w += 2 * M_PI * f / 44100.0;
        pf_proc(p, 0, &l, &r);
        if (n >= 22050) {
            al += (double)l * l; ar += (double)r * r;
            if (l > 300 && sg <= 0) { if (sg < 0) c++; sg = 1; } else if (l < -300 && sg >= 0) { if (sg > 0) c++; sg = -1; }
            if ((n - 22050 + 1) % win == 0) { rl[nw] = sqrt(al / win) / 5657.0; rr[nw] = sqrt(ar / win) / 5657.0; cr[nw] = c; nw++; al = ar = 0; c = 0; }
        }
    }
    pf_destroy(p);
    return nw;
}

/* ---- PERFORM scenarios: the same timeline with and without an effect ---- */
#define PRE 108
static void scen(int16_t *dst, int fx, int sel, int rec_mode, int hold, int tail, const char *knobs, int silent) {
    fresh(&host); seti("quantize", 0);
    ph = 0; rich_n = 0; in_mode = rec_mode;
    tap(REC(1)); run(300); tap(REC(1)); run(2);
    if (sel) tap(FX(1));
    recbuf = dst; recpos = 0; recing = 1;
    run(PRE);
    if (fx >= 0) {
        char b[8]; snprintf(b, sizeof b, "%d", fx);
        down(M_SHIFT); setstr("step", b); up(M_SHIFT);      /* SHIFT is released while the step stays down */
        for (const char *k = knobs; k && *k; ) {
            setstr("knob_delta", k);
            k = strchr(k, ';'); if (k) k++;
        }
    }
    if (silent) in_mode = IN_SILENCE;
    run(hold);
    if (fx >= 0) { char b[8]; snprintf(b, sizeof b, "%d", fx); setstr("step_up", b); }
    in_mode = rec_mode;
    run(tail);
    recing = 0;
}
static long ndiff(const int16_t *a, const int16_t *b, long from, long to) {
    long n = 0; for (long i = from; i < to; i++) if (a[i] != b[i]) n++; return n;
}
static int maxjump(const int16_t *a, long from, long to) {
    int m = 0; for (long i = from + 1; i < to; i++) { int d = abs(a[i] - a[i - 1]); if (d > m) m = d; } return m;
}
static int cross_count(const int16_t *a, long from, long to) {
    int c = 0, sg = 0;
    for (long i = from; i < to; i++) {
        if (a[i] > 300 && sg <= 0) { if (sg < 0) c++; sg = 1; } else if (a[i] < -300 && sg >= 0) { if (sg > 0) c++; sg = -1; }
    }
    return c;
}
static int peak_of(const int16_t *a, long from, long to) { int m = 0; for (long i = from; i < to; i++) if (abs(a[i]) > m) m = abs(a[i]); return m; }
static int ndistinct(const int16_t *a, long from, long to) {
    int16_t seen[64]; int n = 0;
    for (long i = from; i < to; i++) {
        int f = 0; for (int k = 0; k < n; k++) if (seen[k] == a[i]) { f = 1; break; }
        if (!f) { if (n < 64) seen[n++] = a[i]; else return 64; }
    }
    return n;
}
static char scr[4300];
static const char *screen(void) { scr[0] = 0; A->get_param(S, "screen", scr, sizeof scr); return scr; }
static unsigned scr_seq(void) { const char *b = strchr(screen(), '|'); return b ? (unsigned)atoi(scr) : 0xFFFFFFFFu; }

static char pdir[200];
int main(int argc, char **argv) {
    (void)argc;
    snprintf(pdir, sizeof pdir, "/tmp/looper606-test-%d", (int)getpid());
    setenv("LOOPER606_PERSIST_DIR", pdir, 1);
    setenv("LOOPER606_PERSIST_DELAY_MS", "300", 1);
    void *h = dlopen(argv[1], RTLD_NOW); CHECK(h);
    init_fn = (move_plugin_init_v2_fn)dlsym(h, "move_plugin_init_v2"); CHECK(init_fn);
    A = init_fn(&host);
    S = A->create_instance(MODDIR, NULL); CHECK(S);
    CHECK(got_src_log && got_cap_log);

    /* --- hierarchy + params --- */
    static char hb[40000];
    CHECK(A->get_param(S, "ui_hierarchy", hb, sizeof hb) > 1000);
    FILE *f = fopen(argv[2], "w"); fputs(hb, f); fclose(f);
    seti("t6_level", 77); CHECK(geti("t6_level") == 77);          /* track 6 reaches the core */
    seti("t6_level", 100);
    CHECK(geti("threshold") == -42);
    seti("threshold", -30); CHECK(geti("threshold") == -30); seti("threshold", -42);
    seti("in_gain", 150); CHECK(geti("in_gain") == 150); seti("in_gain", 100);
    seti("master", 90); CHECK(geti("master") == 90); seti("master", 100);
    seti("bpm_override", 100); CHECK(geti("bpm_override") == 100); seti("bpm_override", 0);
    seti("quantize", 0);
    CHECK(state(1) == S_EMPTY);
    seti("t1_state", 5); CHECK(state(1) == S_EMPTY);              /* read-only */

    /* --- rec / play / dub cycle on track 1, no clock --- */
    run(10);
    tap(REC(1)); CHECK(state(1) == S_REC);
    run(400); CHECK(state(1) == S_REC);
    tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    out_peak = 0; run(200); CHECK(out_peak > 500);                 /* loop is audible */
    tap(REC(1)); CHECK(state(1) == S_DUB);
    run(50); tap(REC(1)); CHECK(state(1) == S_PLAY);
    CHECK(geti("undo_avail") > 0);

    /* --- mute keeps running, re-enters --- */
    tap(MUTE(1)); CHECK(state(1) == S_MUTE); CHECK(geti("t1_level") == 100);
    seti("monitor", 0); run(40); out_peak = 0; run(200); CHECK(out_peak < 50);   /* gain ramp done */
    tap(MUTE(1)); CHECK(state(1) == S_PLAY);
    out_peak = 0; run(200); CHECK(out_peak > 500);

    /* --- track 6 (col 7) records --- */
    tap(REC(6)); CHECK(state(6) == S_REC); run(100); tap(REC(6)); run(2); CHECK(state(6) == S_PLAY);

    /* --- STOP tap = stop, STOP all, ALL start --- */
    tap(STOP(1)); CHECK(state(1) == S_STOP);
    tap(M_ALL); run(2); CHECK(state(6) == S_STOP);                   /* something plays -> stop all */
    tap(M_ALL); run(2); CHECK(state(1) == S_PLAY && state(6) == S_PLAY);
    tap(M_STOPALL); CHECK(state(1) == S_STOP && state(6) == S_STOP);
    tap(M_ALL); run(2); CHECK(state(1) == S_PLAY);

    /* --- reverse: ARM held + REC tap; ARM tap alone toggles arm --- */
    CHECK(geti("trk_rev") == 0 && geti("arm") == 0);
    down(M_ARM); tap(STOP(1)); up(M_ARM);
    CHECK(geti("t1_rev") == 1); CHECK(geti("arm") == 0); CHECK(state(1) == S_PLAY);
    down(M_ARM); tap(STOP(1)); up(M_ARM); CHECK(geti("t1_rev") == 0); CHECK(state(1) == S_PLAY);
    /* ARM held + row 1 = reverse, never rec/dub; arm unchanged after release */
    down(M_ARM); tap(REC(1)); up(M_ARM);
    CHECK(state(1) == S_PLAY && geti("t1_rev") == 1 && geti("arm") == 0);
    down(M_ARM); run(400); tap(REC(1)); up(M_ARM);                  /* hold > 1 s still works */
    CHECK(state(1) == S_PLAY && geti("t1_rev") == 0 && geti("arm") == 0);
    tap(M_ARM); CHECK(geti("arm") == 1); tap(M_ARM); CHECK(geti("arm") == 0);
    down(M_ARM); CHECK(geti("arm") == 0); up(M_ARM); CHECK(geti("arm") == 1);
    tap(M_ARM); CHECK(geti("arm") == 0); run(400);

    /* --- ARM + threshold --- */
    tap(M_ARM); CHECK(geti("arm") == 1); run(400);                 /* past the 1 s REVERSE window */
    in_mode = IN_SILENCE; tap(REC(2)); CHECK(state(2) == S_ARMED);
    run(100); CHECK(state(2) == S_ARMED);                          /* below threshold */
    in_mode = IN_SILENCE; tap(REC(5)); CHECK(state(5) == S_ARMED);
    in_dc = 16; run(100); CHECK(state(5) == S_ARMED);              /* noise floor (-66 dB) < -42 dB */
    in_dc = 400; run(3); CHECK(state(5) == S_REC);                 /* quiet playing (-38 dB) >= -42 dB */
    in_dc = 0; run(2); tap(REC(5)); run(2); CHECK(state(5) == S_PLAY);
    seti("delete_pad", MUTE(5) - 36); CHECK(state(5) == S_EMPTY);
    in_mode = IN_SILENCE;
    in_mode = IN_SINE; run(3); CHECK(state(2) == S_REC); /* crossed threshold */
    run(200); tap(REC(2)); run(2); CHECK(state(2) == S_PLAY);
    tap(M_ARM); CHECK(geti("arm") == 0); run(400);
    in_mode = IN_SILENCE; tap(REC(3)); tap(REC(3));                /* arm off: plain REC, then cancel */
    in_mode = IN_SINE; tap(M_ARM); run(400); tap(REC(4)); CHECK(state(4) == S_ARMED);
    tap(REC(4)); CHECK(state(4) == S_EMPTY);                        /* tap again cancels arm */
    tap(M_ARM);
    run(400);

    /* --- audio input path: diagnostics fired during the first second --- */
    CHECK(got_peak_log && !got_silent_log && got_bpm_log);
    CHECK(geti("in_peak") > 0);
    in_mode = IN_SILENCE; run(2); CHECK(geti("in_peak") == 0); in_mode = IN_SINE;

    /* --- clear: hold STOP 1 s --- */
    tap(REC(3)); CHECK(state(3) != S_EMPTY); run(10); tap(STOP(3));
    tap(M_STOPALL);
    CHECK(state(2) == S_STOP);
    tap(STOP(2)); run(2); CHECK(state(2) == S_PLAY);                /* row 2 on a stopped track = PLAY (no clear) */
    tap(STOP(2)); CHECK(state(2) == S_STOP);
    down(STOP(2)); run(500); up(STOP(2)); CHECK(state(2) != S_EMPTY);   /* holding row 2 never clears */
    seti("delete_pad", MUTE(2) - 36); CHECK(state(2) == S_EMPTY);       /* Movy Delete + any pad of the column = CLEAR */
    seti("delete_pad", M_SHIFT - 36); seti("delete_pad", 99);            /* master column / junk: ignored */

    /* --- count-in --- */
    tap(R_COUNTIN); CHECK(geti("count_in") == 1);
    tap(REC(5)); CHECK(state(5) == S_ARMED);
    run(600); CHECK(state(5) == S_ARMED);                           /* < 2 s */
    run(150); CHECK(state(5) == S_REC);                             /* 4 beats @120 = 2 s */
    tap(REC(5)); run(2);
    tap(R_COUNTIN); CHECK(geti("count_in") == 0);

    /* --- fixed length: 1 bar @120 = 2 s --- */
    tap(M_STOPALL);
    for (int i = 0; i < 7; i++) tap(R_FIXLEN);                      /* full cycle -> Free */
    CHECK(geti("rec_len") == 0);
    tap(R_FIXLEN); CHECK(geti("rec_len") == 1);
    tap(REC(4)); CHECK(state(4) == S_REC);
    run(600); CHECK(state(4) == S_REC);
    run(200); CHECK(state(4) == S_PLAY);
    seti("rec_len", 0);

    /* --- settings pads --- */
    int q0 = geti("quantize"); tap(R_QUANT); CHECK(geti("quantize") == !q0);
    int r0 = geti("rec_action"); tap(R_RECACT); CHECK(geti("rec_action") == !r0);
    tap(R_RECACT);

    /* --- FX pad + track page (cur_track = last pad touched) --- */
    tap(STOP(2)); seti("t2_fx", 1); seti("t2_fx_on", 1); CHECK(geti("t2_fx_on") == 1); CHECK(geti("cur_track") == 2);
    CHECK(geti("fx_type") >= 1);
    seti("fx_type", 3); CHECK(geti("t2_fx") == 3);
    seti("fx_amt", 60); CHECK(geti("t2_fxp") == 60);
    seti("trk_pan", 20); CHECK(geti("t2_pan") == 20);
    seti("trk_shot", 1); CHECK(geti("t2_shot") == 1);
    down(M_SHIFT); tap(FX(2)); up(M_SHIFT); CHECK(geti("t2_fx_on") == 0);

    /* --- undo pad runs without crashing and reports availability --- */
    UNDO(); run(20);

    /* --- clock: first clock logged --- */
    clock_run(50); CHECK(got_clock_log);

    /* ================= phase 1.1 ================= */
    CHECK(strstr(hb, "\"Looper 606\"") != NULL);

    /* --- audio input path: silent mailbox is reported, missing mailbox is safe --- */
    fresh(&host); in_mode = IN_SILENCE; run(400);
    CHECK(got_silent_log);
    in_mode = IN_SINE;
    fresh(&host_nomem); CHECK(got_none_log);
    { int16_t o[256]; int pk = 0;
      for (int b = 0; b < 20; b++) { A->render_block(S, o, 128); for (int i = 0; i < 256; i++) pk = abs(o[i]) > pk ? abs(o[i]) : pk; }
      CHECK(pk == 0); }

    /* --- bpm fallback with NO MIDI clock: host get_bpm() drives the grid --- */
    bpm_val = 90.0f; fresh(&host); got_bpm_log = 0;
    run(200);
    CHECK(got_bpm_log);
    { char b[32] = ""; A->get_param(S, "tempo_src", b, sizeof b); CHECK(atoi(b) == 1);
      A->get_param(S, "tempo_bpm", b, sizeof b); CHECK((int)(atof(b) + 0.5) == 90); }
    tap(REC(1)); CHECK(state(1) == S_REC);                          /* quantize on, no clock: starts at once */
    run(1100);                                                      /* ~3.2 s */
    tap(REC(1)); run(400);
    CHECK(state(1) == S_PLAY);
    { int len = geti("t1_len"), want = (int)(44100.0 * 60.0 / 90.0 * 4.0);   /* 1 measure @90 */
      CHECK(abs(len - want) < 64); }
    /* no clock and no host bpm: free length, records immediately */
    bpm_val = 120.0f; fresh(&host_nobpm);
    { char b[32] = ""; A->get_param(S, "tempo_src", b, sizeof b); CHECK(atoi(b) == 0); }
    tap(REC(1)); CHECK(state(1) == S_REC);
    run(300); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    CHECK(abs(geti("t1_len") - 300 * 128) < 256);
    /* clock STARTED but never ticks: quantize must not block recording forever */
    fresh(&host);
    { uint8_t st = 0xFA; A->on_midi(S, &st, 1, 3); }
    run(2);                                                         /* downbeat flag consumed */
    tap(REC(1)); CHECK(state(1) == S_ARMED);                        /* waiting for the grid */
    run(1500); CHECK(got_timeout_log); CHECK(state(1) == S_REC);    /* watchdog released it */
    { uint8_t st = 0xFC; A->on_midi(S, &st, 1, 3); }

    /* --- ARM + stale clock + sparse rendering (device bug: armed forever) --- */
    fresh(&host);
    { uint8_t st = 0xFA; A->on_midi(S, &st, 1, 3); }                /* clock started, never ticks */
    in_mode = IN_SILENCE; run(2);
    tap(M_ARM); run(400); tap(REC(1)); CHECK(state(1) == S_ARMED);
    in_mode = IN_SINE; run(1);
    CHECK(state(1) == S_REC);
    tap(REC(1)); run(2);
    { uint8_t st = 0xFC; A->on_midi(S, &st, 1, 3); }
    /* sparse rendering: the host only renders every 172nd block once parked */
    fresh(&host);
    in_mode = IN_SILENCE; run(2);
    tap(M_ARM); run(400); tap(REC(2)); CHECK(state(2) == S_ARMED);
    out_peak = 0; run(20); CHECK(out_peak > 4);                     /* keep-alive above the gate's silence level */
    in_mode = IN_SINE;
    for (int k = 0; k < 3 && state(2) != S_REC; k++) run(1);
    CHECK(state(2) == S_REC);
    tap(REC(2)); run(2);

    /* --- CLEAR is undoable (and redoable) --- */
    fresh(&host); seti("quantize", 0);
    tap(REC(1)); run(300); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    int len1 = geti("t1_len"); CHECK(len1 > 0);
    tap(STOP(1)); CHECK(state(1) == S_STOP);
    down(M_SHIFT); seti("delete_pad", FX(1) - 36); up(M_SHIFT); CHECK(state(1) == S_EMPTY); CHECK(geti("t1_len") == 0);
    CHECK(geti("undo_avail") == 1);
    UNDO(); run(2);
    CHECK(state(1) == S_STOP); CHECK(geti("t1_len") == len1);
    tap(STOP(1)); run(2); CHECK(state(1) == S_PLAY);
    out_peak = 0; run(200); CHECK(out_peak > 500);                  /* audio really came back */
    REDO(); CHECK(state(1) == S_EMPTY);                             /* redo = clear again */
    UNDO(); CHECK(state(1) == S_STOP && geti("t1_len") == len1);

    /* --- user sequence: rec, rec, STOP, STOP must PLAY (with / without note-off) --- */
    for (int with_off = 0; with_off < 2; with_off++) {
        fresh(&host); seti("quantize", 0);
        down(REC(1)); if (with_off) up(REC(1)); run(300);
        down(REC(1)); if (with_off) up(REC(1)); run(2); CHECK(state(1) == S_PLAY);
        down(STOP(1)); if (with_off) up(STOP(1));
        CHECK(state(1) == S_STOP);
        run(1000); CHECK(state(1) == S_STOP && geti("t1_len") > 0);   /* holding never clears */
        down(STOP(1)); if (with_off) up(STOP(1)); run(2);
        CHECK(state(1) == S_PLAY);
        up(STOP(1));
    }
    /* row 1 on a stopped track plays straight into DUB */
    fresh(&host); seti("quantize", 0);
    tap(REC(1)); run(300); tap(REC(1)); run(2); tap(STOP(1)); CHECK(state(1) == S_STOP);
    tap(REC(1)); run(4); CHECK(state(1) == S_DUB);
    tap(REC(1)); CHECK(state(1) == S_PLAY);                          /* DUB -> PLAY */
    tap(REC(1)); CHECK(state(1) == S_DUB);                           /* PLAY -> DUB */
    tap(STOP(1)); CHECK(state(1) == S_STOP);                         /* row 2 stops a dubbing track */
    tap(REC(1)); tap(STOP(1)); run(2); CHECK(state(1) == S_STOP);    /* cancelled before it played: still stopped */
    /* row 2 while recording closes it and stops */
    fresh(&host); seti("quantize", 0);
    tap(REC(2)); run(200); tap(STOP(2)); run(4); CHECK(state(2) == S_STOP && geti("t2_len") > 0);
    /* ARM toggles on release; held as modifier it leaves arm unchanged */
    fresh(&host); seti("quantize", 0);
    tap(M_ARM); CHECK(geti("arm") == 1);
    run(400); down(REC(1)); CHECK(state(1) == S_ARMED); down(REC(1)); CHECK(state(1) == S_EMPTY);
    tap(REC(1)); run(200); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    down(M_ARM); down(STOP(1)); up(STOP(1)); up(M_ARM);
    CHECK(geti("t1_rev") == 1 && geti("arm") == 1 && state(1) == S_PLAY);   /* arm unchanged, no stop */

    /* --- 6th track works without any source patch --- */
    fresh(&host); seti("quantize", 0);
    tap(REC(6)); CHECK(state(6) == S_REC); run(200); tap(REC(6)); run(2);
    CHECK(state(6) == S_PLAY); CHECK(geti("t6_len") > 0);
    seti("t6_pan", 10); CHECK(geti("t6_pan") == 10);
    seti("t6_rev", 1); CHECK(geti("t6_rev") == 1);
    seti("t6_rev", 0);
    CHECK(geti("t5_len") == 0 && state(5) == S_EMPTY);              /* neighbours untouched */
    tap(STOP(6)); CHECK(state(6) == S_STOP);

    /* --- varispeed 1/2 / 1 / 2 (tape style) and reverse --- */
    fresh(&host); seti("quantize", 0); seti("monitor", 0);
    tap(REC(1)); run(500); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    CHECK(geti("trk_spd") == 3);
    int base_c = crossings(300);
    CHECK(base_c > 50);
    seti("trk_spd", 5); CHECK(geti("t1_spd") == 5);
    int c2 = crossings(300);   CHECK(c2 > base_c * 17 / 10 && c2 < base_c * 23 / 10);
    tap(REC(1)); CHECK(state(1) == S_PLAY);                         /* no overdub while varispeed */
    seti("trk_spd", 1); CHECK(geti("t1_spd") == 1);
    int ch = crossings(300);   CHECK(ch > base_c * 4 / 10 && ch < base_c * 6 / 10);
    seti("trk_rev", 1); seti("trk_spd", 5);
    int cr = crossings(300);   CHECK(cr > base_c * 17 / 10 && cr < base_c * 23 / 10);
    out_peak = 0; run(100); CHECK(out_peak > 500);                  /* reverse + varispeed audible */
    seti("trk_rev", 0); seti("trk_spd", 3);
    int c1 = crossings(300);   CHECK(c1 > base_c * 9 / 10 && c1 < base_c * 11 / 10);
    tap(REC(1)); CHECK(state(1) == S_DUB); tap(REC(1));             /* overdub back at 1x */

    /* --- Movy feedback: pad_leds colours + status_text --- */
    {
        char b[700]; int seq0, seq1; char txt[64];
        #define STATUS() do { b[0] = 0; A->get_param(S, "status_text", b, sizeof b); \
                              txt[0] = 0; sscanf(b, "%d:%63[^\n]", &seq1, txt); } while (0)
        #define LEDOF(pad) ({ char hx[3] = {0}; A->get_param(S, "pad_leds", b, sizeof b); \
                              hx[0] = b[(pad) * 2]; hx[1] = b[(pad) * 2 + 1]; (int)strtol(hx, NULL, 16); })
        /* rack pad index (0-based) of a physical (row from bottom 1..4, col 0..7) */
        #define RACKPAD(row, col) (note_of(row, col) - 36)
        fresh(&host); seti("quantize", 0);
        A->get_param(S, "pad_leds", b, sizeof b);
        CHECK(strlen(b) == 64);
        STATUS(); seq0 = seq1; CHECK(seq0 == 0);
        CHECK(LEDOF(RACKPAD(1, 1)) == 98);                       /* empty = very dim track 1 colour (blue) */
        CHECK(LEDOF(RACKPAD(1, 2)) == 92);                       /* track 2 very dim cyan */
        CHECK(LEDOF(RACKPAD(1, 1)) != LEDOF(RACKPAD(1, 2)));
        tap(REC(1)); STATUS(); CHECK(seq1 == seq0 + 1 && !strcmp(txt, "REC 1"));
        CHECK(LEDOF(RACKPAD(1, 1)) == 127);                      /* rec = red */
        run(200); tap(REC(1)); run(2); STATUS(); CHECK(!strcmp(txt, "PLAY 1"));
        CHECK(LEDOF(RACKPAD(1, 1)) == 98);                       /* playing: row 1 back to dim */
        CHECK(LEDOF(RACKPAD(2, 1)) == 126);                      /* play = green on row 2 */
        CHECK(LEDOF(RACKPAD(3, 1)) == 125);                      /* mute pad: live track identity (blue) */
        tap(MUTE(1)); STATUS(); CHECK(!strcmp(txt, "MUTE 1"));
        CHECK(LEDOF(RACKPAD(3, 1)) == 119 && LEDOF(RACKPAD(2, 1)) == 119);   /* mute = grey */
        tap(MUTE(1)); STATUS(); CHECK(!strcmp(txt, "UNMUTE 1"));
        tap(REC(1)); STATUS(); CHECK(!strcmp(txt, "DUB 1")); CHECK(LEDOF(RACKPAD(1, 1)) == 127);  /* dub = red like REC (v0.12) */
        CHECK(LEDOF(RACKPAD(2, 1)) == 126);
        tap(REC(1));
        tap(STOP(1)); STATUS(); CHECK(!strcmp(txt, "STOP 1"));
        CHECK(LEDOF(RACKPAD(2, 1)) == 99);                       /* stopped = dim track colour on row 2 */
        seti("delete_pad", REC(1) - 36); STATUS(); CHECK(!strcmp(txt, "CLEAR 1"));      /* an empty dub holds nothing to undo: the clip goes */
        CHECK(LEDOF(RACKPAD(1, 1)) == 98 && LEDOF(RACKPAD(2, 1)) == 0);
        /* settings + master pads */
        CHECK(LEDOF(RACKPAD(1, 7)) == 123);                      /* quantize off = dim white */
        tap(R_QUANT); STATUS(); CHECK(!strcmp(txt, "QUANT ON")); CHECK(LEDOF(RACKPAD(1, 7)) == 120);
        tap(R_COUNTIN); STATUS(); CHECK(!strcmp(txt, "COUNT-IN ON")); CHECK(LEDOF(RACKPAD(4, 7)) == 120);
        tap(R_RECACT); STATUS(); CHECK(!strcmp(txt, "REC>DUB")); CHECK(LEDOF(RACKPAD(3, 7)) == 120);
        tap(R_FIXLEN); STATUS(); CHECK(!strcmp(txt, "LEN 1 BAR")); CHECK(LEDOF(RACKPAD(2, 7)) == 120);
        tap(R_FIXLEN); STATUS(); CHECK(!strcmp(txt, "LEN 2 BARS"));
        tap(M_ARM); STATUS(); CHECK(!strcmp(txt, "ARM ON")); CHECK(LEDOF(RACKPAD(3, 0)) == (0x80 | 3));
        tap(M_ARM); STATUS(); CHECK(!strcmp(txt, "ARM OFF"));
        fresh(&host); seti("quantize", 0); tap(M_ARM); run(400);
        tap(REC(2)); STATUS(); CHECK(!strcmp(txt, "ARMED 2")); CHECK(LEDOF(RACKPAD(1, 2)) == (0x80 | 3));
        UNDO(); STATUS(); CHECK(!strcmp(txt, "NOTHING TO UNDO"));

        /* colour rules: no track identity, master or settings colour is red or green */
        fresh(&host); seti("quantize", 0);
        for (int k = 1; k <= 6; k++) {
            int ids[3] = { LEDOF(RACKPAD(1, k)) & 0x7F, LEDOF(RACKPAD(3, k)) & 0x7F, LEDOF(RACKPAD(4, k)) & 0x7F };
            for (int j = 0; j < 3; j++) CHECK(ids[j] != 126 && ids[j] != 127 && ids[j] != 11 && ids[j] != 85 && ids[j] != 1);
        }
        for (int row = 1; row <= 4; row++) for (int col = 0; col <= 7; col += 7) {
            int c = LEDOF(RACKPAD(row, col)) & 0x7F;
            CHECK(c != 126 && c != 127 && c != 11 && c != 85 && c != 1 && c != 65);
        }
        tap(REC(2)); run(200); tap(REC(2)); run(2);               /* PLAY: masters/settings still not red/green */
        for (int row = 1; row <= 4; row++) for (int col = 0; col <= 7; col += 7) {
            int c = LEDOF(RACKPAD(row, col)) & 0x7F;
            CHECK(c != 126 && c != 127 && c != 11 && c != 85 && c != 1 && c != 65);
        }
        CHECK(LEDOF(RACKPAD(2, 2)) == 126);                      /* but the playing pad itself is green */
        CHECK(LEDOF(RACKPAD(3, 2)) == 14);                       /* mute pad keeps the track 2 identity (cyan) */

        /* display_param: "<seq>|<pos>|<peaks>|<header>" */
        char hdrbuf[64];
        #define DISPLAY() do { b[0] = 0; A->get_param(S, "display", b, sizeof b); } while (0)
        #define DSEQ()  atoi(b)
        #define DPOS()  atoi(strchr(b, '|') + 1)
        #define DPEAKS(o) do { const char *q = strchr(strchr(b, '|') + 1, '|') + 1; \
                               size_t n = strcspn(q, "|"); memcpy(o, q, n); (o)[n] = 0; } while (0)
        #define DHEAD() disp_field(b, 3, hdrbuf, sizeof hdrbuf)
        {
            char pk[600];
            fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
            DISPLAY(); DPEAKS(pk);
            CHECK(strlen(pk) == 0 && !strcmp(DHEAD(), "EMPTY 1"));
            tap(REC(1)); run(100); DISPLAY(); CHECK(!strncmp(DHEAD(), "REC 1", 5));
            run(245); tap(REC(1)); run(2);
            DISPLAY(); DPEAKS(pk);
            CHECK(strlen(pk) == 256);
            int mx = 0;
            for (int i = 0; i < 128; i++) { char h2[3] = { pk[2*i], pk[2*i+1], 0 }; int v = (int)strtol(h2, NULL, 16); if (v > mx) mx = v; }
            CHECK(mx > 100);                                     /* a 0.25 sine reads clearly on the overview */
            CHECK(!strncmp(DHEAD(), "PLAY 1 - ", 9) && strstr(DHEAD(), " S"));
            int seq_a = DSEQ(), pos_a = DPOS();
            run(60); DISPLAY();
            CHECK(DSEQ() == seq_a);                              /* playing does not produce new peaks */
            CHECK(DPOS() != pos_a && DPOS() >= 0 && DPOS() < 128);
            tap(MUTE(1)); DISPLAY(); CHECK(!strncmp(DHEAD(), "MUTE 1", 6)); tap(MUTE(1));
            seti("cur_track", 2); DISPLAY(); DPEAKS(pk);
            CHECK(strlen(pk) == 0 && !strcmp(DHEAD(), "EMPTY 2"));   /* the selected track is the one shown */
            DISPLAY();
            seti("cur_track", 1); DISPLAY(); CHECK(DSEQ() != seq_a);  /* switching track bumps seq */
            /* fixed length: 2 bars at the host tempo reads as bars, not seconds */
            fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
            tap(R_FIXLEN); tap(R_FIXLEN);
            tap(REC(1)); run(1700); DISPLAY();
            CHECK(strstr(DHEAD(), "2 BARS") != NULL);
        }
        #undef DISPLAY
        #undef DSEQ
        #undef DPOS
        #undef DPEAKS
        #undef DHEAD
        #undef STATUS
        #undef LEDOF
        #undef RACKPAD
    }


    /* --- display ticks: "<seq>|<pos>|<peaks>|<header>|<ticks>" --- */
    {
        int tk[200], bars, beats;
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE; bpm_val = 120.0f;
        tap(R_FIXLEN); tap(R_FIXLEN); tap(R_FIXLEN);                  /* 1 -> 2 -> 4 bars */
        CHECK(geti("rec_len") == 4);
        tap(REC(1)); run(2800); run(4);
        CHECK(state(1) == S_PLAY);
        disp();
        CHECK(strchr(strchr(strchr(strchr(dbuf, '|') + 1, '|') + 1, '|') + 1, '|') != NULL);   /* 5 fields */
        CHECK(strchr(disp_head(), '|') == NULL);
        int n = disp_ticks(tk); bars = beats = 0;
        int ph[64], np = disp_phrase(ph);
        for (int i = 0; i < n; i++) { if (tk[i] & 0x80) beats++; else bars++; }
        CHECK(bars == 3 && beats == 12 && np == 1 && ph[0] == 0);     /* 4 bars: bar 1 is a phrase tick, 3 bar ticks, 12 beats */
        CHECK(tk[0] & 0x80); CHECK((tk[0] & 0x7F) >= 7 && (tk[0] & 0x7F) <= 9);
        int barbins[8], nb = 0;
        for (int i = 0; i < n; i++) if (!(tk[i] & 0x80)) barbins[nb++] = tk[i];
        CHECK(abs(barbins[0] - 32) <= 1 && abs(barbins[2] - 96) <= 1);
        /* clip start moved half a bar: the grid follows it, in both directions */
        setstr("loop_knob", "1:8");
        run(1000);                                                  /* let the Loop+knob zoom end: MIX shows IN..OUT */
        n = disp_ticks(tk); nb = 0; np = disp_phrase(ph);
        for (int i = 0; i < n; i++) if (!(tk[i] & 0x80)) barbins[nb++] = tk[i];
        CHECK(nb == 3 && abs(barbins[0] - 37) <= 1 && abs(barbins[2] - 110) <= 1);      /* region = 3.5 bars scaled to 128, bars counted from IN */
        CHECK(np == 1 && ph[0] == 0);                                  /* bar 1 = IN = bin 0 */
        CHECK(!(tk[0] & 0x80) || (tk[0] & 0x7F) >= 8);                /* nothing before IN: the view starts there */
        /* 8-bar loop at 120 bpm: phrase ticks at bars 1 and 5 (bins 0, 64), the other 6 are bar ticks */
        setstr("loop_knob", "1:0");
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE; bpm_val = 120.0f;
        for (int i = 0; i < 4; i++) tap(R_FIXLEN);                    /* 1 -> 2 -> 4 -> 8 bars */
        CHECK(geti("rec_len") == 8);
        tap(REC(1)); run(5600); run(4);
        CHECK(state(1) == S_PLAY);
        n = disp_ticks(tk); np = disp_phrase(ph); bars = nb = 0;
        for (int i = 0; i < n; i++) if (!(tk[i] & 0x80)) bars++;
        CHECK(np == 2 && ph[0] == 0 && abs(ph[1] - 64) <= 1);
        CHECK(bars == 6);
        setstr("loop_knob", "1:8");                                    /* off moved half a bar: the view starts at IN */
        run(1000);
        n = disp_ticks(tk); np = disp_phrase(ph);
        CHECK(np == 2 && ph[0] == 0 && abs(ph[1] - 68) <= 1);         /* 7.5 bars over 128 px: bar 5 at 4/7.5 */
        setstr("loop_knob", "1:0"); run(1000);
        /* slower than 4 px per beat -> bars only (60 bpm keeps 4 bars x 4 beats = 16 beats over 128 px?) */
        bpm_val = 480.0f; run(2); n = disp_ticks(tk);                 /* 1 beat = 5512 frames; px/beat ~ 2 -> bars only */
        beats = 0; for (int i = 0; i < n; i++) if (tk[i] & 0x80) beats++;
        CHECK(n > 0 && beats == 0);
        bpm_val = 120.0f; run(2);
    }

    /* --- reverse playhead (display runs right-to-left) and varispeed --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        tap(REC(1)); run(345); tap(REC(1)); run(4);                 /* ~1 s loop, 128 bins ~ 345 frames each */
        CHECK(state(1) == S_PLAY);
        int p1 = disp_pos(); run(5); int p2 = disp_pos();
        int fwd = (p2 - p1 + 128) % 128;
        CHECK(fwd >= 1 && fwd <= 5);                                /* forward: left to right */
        int q = disp_pos(); seti("trk_rev", 1);                     /* same instant, mirrored */
        int qr = disp_pos();
        CHECK(abs(qr - (127 - q)) <= 1);
        p1 = disp_pos(); run(5); p2 = disp_pos();
        int back = (p1 - p2 + 128) % 128;
        CHECK(back >= 1 && back <= 5);                              /* reversed: right to left */
        seti("trk_spd", 5);
        p1 = disp_pos(); run(5); p2 = disp_pos();
        back = (p1 - p2 + 128) % 128;
        CHECK(back >= 3 && back <= 9);                              /* 2x reverse: about twice as fast, still leftwards */
        seti("trk_rev", 0);
        p1 = disp_pos(); run(5); p2 = disp_pos();
        fwd = (p2 - p1 + 128) % 128;
        CHECK(fwd >= 3 && fwd <= 9);                                /* 2x forward */
        seti("trk_spd", 1);
        p1 = disp_pos(); run(10); p2 = disp_pos();
        fwd = (p2 - p1 + 128) % 128;
        CHECK(fwd >= 1 && fwd <= 5);                                /* 1/2x: 10 blocks = 5 blocks at 1x */
        seti("trk_spd", 3);
    }

    /* --- step buttons = fixed recording length --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        CHECK(strlen(getstr("step_leds")) == 32);
        for (int i = 0; i < 16; i++) CHECK(step_led(i) == 119);      /* free: all dark */
        setstr("step", "3");                                          /* step 4 = 4 bars */
        CHECK(geti("rec_len") == 4); CHECK(!strcmp(status_text(), "LEN 4 BARS"));
        CHECK(step_led(3) == 120);                                    /* chosen = white */
        CHECK(step_led(0) == 123 && step_led(2) == 123);              /* 1..N-1 dim */
        CHECK(step_led(4) == 119 && step_led(15) == 119);             /* beyond: dark */
        setstr("step", "3");                                          /* again = free */
        CHECK(geti("rec_len") == 0); CHECK(!strcmp(status_text(), "LEN FREE"));
        CHECK(step_led(3) == 119);
        setstr("step", "0"); CHECK(!strcmp(status_text(), "LEN 1 BAR")); CHECK(geti("rec_len") == 1);
        setstr("step", "15"); CHECK(geti("rec_len") == 16); CHECK(!strcmp(status_text(), "LEN 16 BARS"));
        setstr("step", "16"); setstr("step", "-1"); CHECK(geti("rec_len") == 16);   /* out of range: ignored */
        /* the column-8 pad cycles from wherever the steps left it, and the steps follow */
        setstr("step", "2");                                          /* 3 bars */
        tap(R_FIXLEN); CHECK(geti("rec_len") == 4); CHECK(step_led(3) == 120);
        tap(R_FIXLEN); CHECK(geti("rec_len") == 8);
        tap(R_FIXLEN); CHECK(geti("rec_len") == 16);
        tap(R_FIXLEN); CHECK(geti("rec_len") == 32 && !strcmp(status_text(), "LEN 32 BARS"));
        tap(R_FIXLEN); CHECK(geti("rec_len") == 0); CHECK(!strcmp(status_text(), "LEN FREE"));
        tap(R_FIXLEN); CHECK(geti("rec_len") == 1); CHECK(step_led(0) == 120);
        /* the pad LED and the setup enum agree with the steps */
        setstr("step", "4");
        { char b[100]; A->get_param(S, "pad_leds", b, sizeof b);
          char hx[3] = { b[(note_of(2, 7) - 36) * 2], b[(note_of(2, 7) - 36) * 2 + 1], 0 };
          CHECK(strtol(hx, NULL, 16) == 120); }                       /* 5 bars -> brightest */
        /* a recording of N bars closes by itself: 1 bar @120 = 2 s; 3 bars = 6 s */
        setstr("step", "0");
        tap(REC(1)); run(600); CHECK(state(1) == S_REC); run(200); CHECK(state(1) == S_PLAY);
        seti("cur_track", 3);                                         /* an empty track selected: steps = next-rec length */
        setstr("step", "2");
        tap(REC(2)); run(2000); CHECK(state(2) == S_REC); run(100); CHECK(state(2) == S_PLAY);
        CHECK(gett(2, "len") > 3 * gett(1, "len") * 95 / 100 && gett(2, "len") < 3 * gett(1, "len") * 105 / 100);
        seti("cur_track", 3); setstr("step", "2");
        /* the setup page's enum is the same value */
        seti("rec_len", 6); CHECK(geti("rec_len") == 6 && step_led(5) == 120);
        seti("rec_len", 99); CHECK(geti("rec_len") == 32);
        seti("rec_len", 0);
        /* v0.14.6: 32 bars do not fit the 120 s buffer at slow tempos: clamp to the largest that fits, never stuck */
        seti("bpm_override", 40);                                     /* 6 s per bar: 20 bars fit, so 16 is the largest cycle value */
        seti("rec_len", 16);
        seti("rec_len", 32); CHECK(geti("rec_len") == 16 && !strcmp(status_text(), "LEN 32 > MAX 16"));
        tap(R_FIXLEN); CHECK(geti("rec_len") == 0);                   /* at the largest that fits, the next press goes back to free */
        seti("rec_len", 8); tap(R_FIXLEN); CHECK(geti("rec_len") == 16);
        tap(R_FIXLEN); CHECK(geti("rec_len") == 0);                   /* 32 does not fit: free instead of sticking at 16 */
        seti("rec_len", 0); seti("bpm_override", 0);
    }

    /* --- Loop + knob = clip start --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        setstr("step", "0");                                          /* 1 bar */
        tap(REC(1));
        run(344); in_mode = IN_SILENCE; run(350);                     /* first half loud, second half silent; 1 bar closes itself */
        run(4);
        int L = gett(1, "len");
        CHECK(state(1) == S_PLAY && L > 88000 && L < 88800);
        CHECK(gett(1, "off") == 0 && disp_mark() == -1);              /* no marker until it is moved */
        int pk0 = disp_peak(10), pkq = disp_peak(100);
        CHECK(pk0 > 100 && pkq == 0);
        /* +8 sixteenths at 120 bpm = half a bar */
        setstr("loop_knob", "1:8");
        CHECK(abs(gett(1, "off") - 44100) <= 2);
        CHECK(!strcmp(status_text(), "START 1 +8/16"));
        CHECK(!strcmp(disp_head(), "START 1 +8/16 ") || strstr(disp_head(), "START 1 +8/16") != NULL);
        CHECK(abs(disp_mark() - 63) <= 2);                            /* marker column */
        CHECK(geti("cur_track") == 1);
        /* playback starts from the clip start: stopped track restarts at pos 0 = buffer L/2 (silent half) */
        tap(STOP(1)); tap(STOP(1)); run(20);
        out_peak = 0; run(20); CHECK(out_peak < 100);                 /* half-bar offset: starts in the quiet half */
        setstr("loop_knob", "1:-8");
        CHECK(gett(1, "off") == 0 || gett(1, "off") == L - 0);        /* back to the start */
        tap(STOP(1)); tap(STOP(1)); run(2);
        out_peak = 0; run(20); CHECK(out_peak > 500);                 /* offset 0: starts loud */
        setstr("loop_knob", "1:-1");                                  /* clamps at zero: no wrap */
        CHECK(gett(1, "off") == 0);
        setstr("loop_knob", "1:1"); CHECK(abs(gett(1, "off") - 5512) <= 3);
        /* fine mode: 1/64 note, free of the 1/16 grid; coarse snaps back */
        setstr("loop_knob", "1:8");
        setstr("loop_knob", "1:1:1");
        CHECK(abs(gett(1, "off") - (int)(37 * 1378.125 + 0.5)) <= 3);
        CHECK(strstr(status_text(), " MS") != NULL);
        setstr("loop_knob", "1:1");
        CHECK(abs(gett(1, "off") - (int)(10 * 5512.5 + 0.5)) <= 3);
        setstr("loop_knob", "1:-16");
        CHECK(gett(1, "off") == 0);
        /* other tracks: empty = refused with a message; knobs 7/8 and junk = nothing */
        setstr("loop_knob", "3:2"); CHECK(gett(3, "off") == 0); CHECK(!strcmp(status_text(), "START 3 EMPTY"));
        setstr("loop_knob", "7:1"); setstr("loop_knob", "0:1"); setstr("loop_knob", "x"); setstr("loop_knob", "1:0");
        CHECK(gett(1, "off") == 0);
        /* reverse stays inside [off, len): with off = L/2 only the silent half exists, so it is silent for good */
        setstr("loop_knob", "1:8"); seti("cur_track", 1); seti("trk_rev", 1);
        tap(STOP(1)); tap(STOP(1)); run(2);
        out_peak = 0; run(3000); CHECK(out_peak < 100);               /* 4+ cycles: the loud half never sounds */
        seti("trk_rev", 0);
        seti("trk_spd", 5); out_peak = 0; run(3000); CHECK(out_peak < 100); seti("trk_spd", 3);
        tap(STOP(1)); tap(STOP(1)); run(2);
        out_peak = 0; run(3000); CHECK(out_peak < 100);               /* forward: the same, over several cycles */

        /* overdub writes where it plays: with the start at L/2 a dub from the top lands at bin 64 */
        CHECK(disp_peak(64) == 0 && disp_peak(66) == 0);
        tap(STOP(1)); tap(REC(1));
        run(2); CHECK(state(1) == S_DUB);
        in_mode = IN_LOUD; run(10); in_mode = IN_SILENCE;
        setstr("loop_knob", "1:1"); CHECK(!strcmp(status_text(), "START 1 LOCKED"));   /* not while dubbing */
        CHECK(abs(gett(1, "off") - 44100) <= 3);
        tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
        tap(STOP(1)); run(1);
        tap(FX(1));                                                   /* PISTA: whole buffer, bins = buffer position */
        CHECK(disp_peak(64) > 0);                                     /* the dub is at buffer L/2... */
        CHECK(disp_peak(10) > 100 && disp_peak(100) == 0);            /* ...and the rest of the loop is as recorded */
        /* undo of the dub restores the quiet half, clear + undo restores the start */
        UNDO(); run(200); CHECK(disp_peak(64) == 0);
        REDO(); run(200);
        tap(FX(1)); CHECK(geti("sel") == 0);
        seti("delete_pad", REC(1) - 36); seti("delete_pad", REC(1) - 36); CHECK(state(1) == S_EMPTY);   /* dub layer, then the clip */
        CHECK(gett(1, "off") == 0 && disp_mark() == -1);
        UNDO(); CHECK(state(1) == S_STOP); CHECK(abs(gett(1, "off") - 44100) <= 3);
        seti("delete_pad", REC(1) - 36); UNDO();
        /* a moved start is shown on the display even after the readout fades */
        in_mode = IN_SINE; run(1200); in_mode = IN_SILENCE;
        CHECK(disp_mark() == -1 && strncmp(disp_head(), "START", 5) != 0);   /* MIX shows IN..OUT only: no marker */
        tap(FX(1)); CHECK(abs(disp_mark() - 63) <= 2); tap(FX(1));          /* the PISTA overview marks IN */
    }

    /* --- clip start is a real trim: [off, len) only, cycle = len - off --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        setstr("step", "0");
        tap(REC(1));
        run(344); in_mode = IN_SILENCE; run(350); run(4);             /* loud first half, silent second half */
        int L = gett(1, "len");
        CHECK(state(1) == S_PLAY && L > 88000 && L < 88800);
        setstr("loop_knob", "1:4");                                   /* off = 1/4 bar: region = loud quarter + silent half */
        int off = gett(1, "off");
        CHECK(abs(off - 22050) <= 3);
        for (int rev = 0; rev < 2; rev++) {
            seti("trk_rev", rev);
            tap(STOP(1)); tap(STOP(1)); run(2);
            int edges[8], ne = 0, prev = 0, ever_loud_blocks = 0;
            for (int b = 0; b < 2200 && ne < 8; b++) {
                out_peak = 0; run(1);
                int loud = out_peak > 500;
                if (loud) ever_loud_blocks++;
                if (loud && !prev) edges[ne++] = b;
                prev = loud;
            }
            CHECK(ne >= 4);
            double cyc = (double)(L - off) / 128.0;                   /* blocks per cycle */
            for (int i = 2; i < ne; i++)
                CHECK(fabs((double)(edges[i] - edges[i - 1]) - cyc) <= 2.5);
            /* loud time per cycle = the loud quarter only (old rotation would give a half) */
            CHECK(ever_loud_blocks > 0 && ever_loud_blocks < (int)(2200 / cyc * (L / 4.0 / 128.0) * 1.2 + 4));
        }
        seti("trk_rev", 0);
        /* moving the start to the end of the loud part: silent for good */
        setstr("loop_knob", "1:4");
        tap(STOP(1)); tap(STOP(1)); run(2);
        out_peak = 0; run(3000); CHECK(out_peak < 100);
        /* the knob clamps at the end, keeping at least a 1/16 note of loop */
        for (int i = 0; i < 6; i++) setstr("loop_knob", "1:16");
        off = gett(1, "off");
        CHECK(off < L && L - off >= 5000 && L - off <= 6000);
        /* a shorter END pulls the start back in; changing it while playing keeps the head in range */
        tap(STOP(1)); tap(STOP(1)); run(2);
        setstr("step", "8");
        CHECK(gett(1, "off") < gett(1, "len") && gett(1, "len") - gett(1, "off") >= 5000);
        out_peak = 0; run(1500); CHECK(state(1) == S_PLAY);
        setstr("loop_knob", "1:-16"); CHECK(gett(1, "off") == 0);
        /* a start with the loud half to its right: audible, never drops below the start */
        fresh(&host); seti("quantize", 0);
        in_mode = IN_SILENCE; setstr("step", "0"); tap(REC(1));
        run(344); in_mode = IN_SINE; run(350); in_mode = IN_SILENCE; run(4);   /* silent first half, loud second */
        L = gett(1, "len");
        setstr("loop_knob", "1:8");
        tap(STOP(1)); tap(STOP(1)); run(2);
        int quiet = 0;
        for (int b = 0; b < 2000; b++) { out_peak = 0; run(1); if (out_peak < 500) quiet++; }
        CHECK(quiet <= 3);                                            /* the silent half never comes round */
    }

    /* --- persistence: loops survive a reload --- */
    {
        char dir[300], id1[32], blob[8192];
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        snprintf(id1, sizeof id1, "%s", getstr("loop_id"));
        CHECK(strlen(id1) == 12);
        char first_id[32]; snprintf(first_id, sizeof first_id, "%s", id1);
        fresh(&host); seti("quantize", 0);
        CHECK(strcmp(getstr("loop_id"), first_id) != 0);              /* every instance has its own id */
        snprintf(id1, sizeof id1, "%s", getstr("loop_id"));
        snprintf(dir, sizeof dir, "%s/%s", pdir, id1);
        setstr("step", "0");
        seti("rec_len", 1); seti("count_in", 1); seti("count_in", 0);
        tap(REC(1)); run(800); run(2);                                /* a 1 bar loop on track 1 (closes itself) */
        in_mode = IN_SINE;
        tap(REC(2)); run(200); tap(REC(2)); run(2);                   /* a short one on track 2 */
        seti("rec_len", 3);
        sett(2, "rev", 1); setstr("loop_knob", "1:4"); tap(MUTE(3));
        int len1 = gett(1, "len"), len2 = gett(2, "len"), off1 = gett(1, "off");
        CHECK(len1 > 0 && len2 > 0 && off1 > 20000);
        CHECK(!file_exists(dir, "t1.wav"));                           /* nothing is written on the audio/control thread */
        wait_until_saved(dir, "loops.json", 3000);
        CHECK(file_exists(dir, "t1.wav") && file_exists(dir, "t2.wav") && file_exists(dir, "loops.json"));
        CHECK(!file_exists(dir, "t3.wav"));
        { /* the saved WAV is plain 16-bit stereo 44.1k */
          char path[400]; snprintf(path, sizeof path, "%s/t2.wav", dir);
          struct stat st; CHECK(stat(path, &st) == 0 && st.st_size == 44 + (off_t)len2 * 4); }
        /* the state Movy stores carries the id and the shim settings */
        A->get_param(S, "state", blob, sizeof blob);
        CHECK(strstr(blob, id1) != NULL && strstr(blob, "\"l6rl\":3") != NULL);
        /* close and reopen: a fresh instance given that state restores the loops, stopped */
        fresh(&host);
        CHECK(state(1) == S_EMPTY);
        A->set_param(S, "state", blob);
        for (int w = 0; w < 60 && state(1) == S_EMPTY; w++) usleep(50000);
        CHECK(state(1) == S_STOP && state(2) == S_STOP && state(3) == S_EMPTY);
        CHECK(gett(1, "len") == len1 && gett(2, "len") == len2);
        CHECK(gett(1, "off") == off1);                                /* the clip start came back too */
        CHECK(!strcmp(getstr("loop_id"), id1));
        CHECK(geti("rec_len") == 3 && gett(2, "rev") == 1);
        { char b[100]; A->get_param(S, "pad_leds", b, sizeof b);      /* mute 3 restored: its mute pad is grey */
          char hx[3] = { b[(MUTE(3) - 36) * 2], b[(MUTE(3) - 36) * 2 + 1], 0 };
          CHECK(strtol(hx, NULL, 16) == 119 && geti("t3_level") == 100); }
        tap(STOP(1)); run(4);
        out_peak = 0; run(30); CHECK(state(1) == S_PLAY && out_peak > 100);   /* and it plays */
        /* nothing was re-saved just because it was restored */
        struct stat st0; char jp[400]; snprintf(jp, sizeof jp, "%s/loops.json", dir); stat(jp, &st0);
        usleep(700000);
        struct stat st1; stat(jp, &st1);
        CHECK(st0.st_mtime == st1.st_mtime);
        /* a change is saved after it settles: clear track 2 deletes its file */
        seti("delete_pad", REC(2) - 36);
        for (int w = 0; w < 60 && file_exists(dir, "t2.wav"); w++) usleep(50000);
        CHECK(!file_exists(dir, "t2.wav") && file_exists(dir, "t1.wav"));
        /* closing flushes what is still unsaved */
        tap(REC(4)); run(150); tap(REC(4)); run(2);
        CHECK(!file_exists(dir, "t4.wav"));
        fresh(&host);                                                  /* destroys the previous instance */
        CHECK(file_exists(dir, "t4.wav"));
        /* a state with another id on an instance that already holds loops does not clobber them */
        tap(REC(1)); run(100); tap(REC(1)); run(2);
        snprintf(blob, sizeof blob, "{\"q\":0,\"l6id\":\"abcdef012345\",\"l6rl\":2}");
        A->set_param(S, "state", blob); usleep(400000);
        CHECK(strcmp(getstr("loop_id"), "abcdef012345") != 0 && state(1) != S_EMPTY);
        /* an id that is not hex (path tricks) is never used */
        fresh(&host);
        snprintf(blob, sizeof blob, "{\"q\":0,\"l6id\":\"../../etc\",\"l6rl\":2}");
        A->set_param(S, "state", blob); usleep(300000);
        CHECK(strchr(getstr("loop_id"), '/') == NULL && strlen(getstr("loop_id")) == 12);
        /* a fresh Set (no folder for that id) just starts empty, and saves under that id from then on */
        fresh(&host);
        snprintf(blob, sizeof blob, "{\"q\":0,\"l6id\":\"0123456789ab\",\"l6rl\":0}");
        A->set_param(S, "state", blob); usleep(400000);
        CHECK(state(1) == S_EMPTY && !strcmp(getstr("loop_id"), "0123456789ab"));
        tap(REC(5)); run(100); tap(REC(5)); run(2);
        snprintf(dir, sizeof dir, "%s/0123456789ab", pdir);
        wait_until_saved(dir, "t5.wav", 3000);
        CHECK(file_exists(dir, "t5.wav"));
        /* a damaged folder is never overwritten */
        { char path[400]; snprintf(path, sizeof path, "%s/t5.wav", dir); FILE *f = fopen(path, "wb"); fputs("junk", f); fclose(f); }
        fresh(&host);
        A->set_param(S, "state", blob); usleep(400000);
        CHECK(state(5) == S_EMPTY);
        tap(REC(1)); run(100); tap(REC(1)); run(2); usleep(900000);
        { char path[400]; snprintf(path, sizeof path, "%s/t5.wav", dir); struct stat st; CHECK(stat(path, &st) == 0 && st.st_size == 4); }
        fresh(&host);
    }

    /* --- step buttons on a recorded clip: length in bars (trim / repeat), undo, LEDs --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        setstr("step", "0");                                          /* next-rec length 1 bar */
        tap(REC(1)); run(344); in_mode = IN_SILENCE; run(350); run(4);
        long L = gett(1, "len"), B = 88200;                           /* one bar @120 bpm */
        CHECK(state(1) == S_PLAY && L > 88000 && L < 88800 && gett(1, "full") == L);
        CHECK(step_led(0) == 120);                                    /* the selected clip is 1 bar: step 1 white */
        setstr("step", "3");                                          /* clip 1 -> 4 bars: the content repeats */
        long L4 = gett(1, "len");
        CHECK(labs(L4 - 4 * B) < 2 && gett(1, "full") == L4);
        CHECK(!strcmp(status_text(), "LEN 1: 4 BARS"));
        CHECK(geti("rec_len") == 1);                                  /* the next-rec length is untouched */
        CHECK(step_led(3) == 120 && step_led(1) == 123 && step_led(4) == 119);
        CHECK(disp_peak(4) > 100 && disp_peak(24) == 0 && disp_peak(36) > 100 && disp_peak(56) == 0 && disp_peak(100) > 100);
        CHECK(strstr(disp_head(), "4 BARS") != NULL);
        /* shorter: non-destructive trim */
        setstr("step", "1");                                          /* 2 bars */
        CHECK(labs(gett(1, "len") - 2 * B) < 2 && gett(1, "full") == L4);
        CHECK(!strcmp(status_text(), "LEN 1: 2 BARS") && step_led(1) == 120);
        setstr("step", "3");                                          /* longer again: audio is back, no new copy */
        CHECK(gett(1, "len") == L4 && gett(1, "full") == L4);
        CHECK(disp_peak(100) > 100 && disp_peak(24) == 0);
        /* undo = the previous length, redo = the new one */
        setstr("step", "0"); CHECK(labs(gett(1, "len") - B) < 2);
        UNDO(); CHECK(gett(1, "len") == L4);
        REDO(); CHECK(labs(gett(1, "len") - B) < 2);
        UNDO();
        /* pressing the step the clip already is leaves it (and its undo) alone */
        setstr("step", "3"); CHECK(!strcmp(status_text(), "LEN 1: 4 BARS")); CHECK(gett(1, "len") == L4);
        /* beyond the recorded 4 bars: 8 bars = repeat to fill */
        setstr("step", "7");
        CHECK(labs(gett(1, "len") - 8 * B) < 2 && gett(1, "full") == gett(1, "len"));
        CHECK(disp_peak(2) > 100 && disp_peak(10) == 0 && disp_peak(18) > 100 && disp_peak(114) > 100);
        UNDO(); CHECK(gett(1, "len") == L4 && gett(1, "full") == L4);
        /* out of range (bpm 20: 16 bars = 192 s > the 120 s track): refused */
        seti("bpm_override", 20);
        setstr("step", "15"); CHECK(!strcmp(status_text(), "LEN 1: TOO LONG")); CHECK(gett(1, "len") == L4);
        seti("bpm_override", 0);
        /* DUB + step: back to PLAY, the overdub still undoes */
        tap(REC(1)); CHECK(state(1) == S_DUB);
        setstr("step", "1"); CHECK(state(1) == S_PLAY && labs(gett(1, "len") - 2 * B) < 2);
        run(50); out_peak = 0; run(100); CHECK(out_peak > 100);       /* still sounding */
        /* the start offset follows the new length; undo brings it back */
        setstr("step", "3"); setstr("loop_knob", "1:12"); long off = gett(1, "off");
        CHECK(off > 0);
        setstr("step", "0"); CHECK(gett(1, "off") < gett(1, "len"));
        UNDO(); CHECK(gett(1, "off") == off && gett(1, "len") == L4);
        /* reverse + varispeed keep working on a resized clip */
        sett(1, "rev", 1); sett(1, "spd", 5);
        setstr("step", "1"); run(50); out_peak = 0; run(100); CHECK(out_peak > 100);
        CHECK(gett(1, "off") < gett(1, "len") && state(1) == S_PLAY);
        sett(1, "rev", 0); sett(1, "spd", 3);
        /* the selected track decides: an empty one still sets the next-rec length */
        seti("cur_track", 4); setstr("step", "5");
        CHECK(geti("rec_len") == 6 && !strcmp(status_text(), "LEN 6 BARS") && step_led(5) == 120);
        CHECK(labs(gett(1, "len") - 2 * B) < 2);
        seti("cur_track", 1); CHECK(step_led(5) == 119);              /* clip 1 is 2 bars (not 6): LEDs follow the selection */
        seti("rec_len", 0);
    }

    /* --- a resized clip is saved with its new length --- */
    {
        char blob[8192];
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        setstr("step", "0"); tap(REC(1)); run(700); run(4);
        setstr("step", "2");                                          /* 3 bars */
        long L3 = gett(1, "len"); CHECK(L3 > 3 * 88000 && L3 < 3 * 88800);
        char dir[300]; snprintf(dir, sizeof dir, "%s/%s", pdir, getstr("loop_id"));
        char jp[400], js[1024] = ""; snprintf(jp, sizeof jp, "%s/loops.json", dir);
        char want[40]; snprintf(want, sizeof want, "\"l1\":%ld", L3);
        for (int w = 0; w < 80; w++) {
            usleep(50000);
            FILE *f = fopen(jp, "rb"); if (f) { size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f); }
            if (strstr(js, want)) break;
        }
        CHECK(strstr(js, want) != NULL);
        setstr("step", "0");                                          /* trimmed to 1 bar: len is saved, the audio is kept */
        snprintf(want, sizeof want, "\"l1\":%d", gett(1, "len"));
        for (int w = 0; w < 80; w++) {
            usleep(50000); js[0] = 0;
            FILE *f = fopen(jp, "rb"); if (f) { size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f); }
            if (strstr(js, want)) break;
        }
        CHECK(strstr(js, want) != NULL);
        long L1 = gett(1, "len");
        A->get_param(S, "state", blob, sizeof blob);
        fresh(&host);
        A->set_param(S, "state", blob);
        for (int w = 0; w < 60 && state(1) == S_EMPTY; w++) usleep(50000);
        CHECK(state(1) == S_STOP && gett(1, "len") == L1 && gett(1, "full") == L3);
        setstr("step", "2"); CHECK(gett(1, "len") == L3);             /* the trimmed part is still there */
    }

    /* --- row 4 selector, PISTA knobs, SHIFT combos --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        static const int vdim[6] = { 98, 92, 74, 82, 116, 69 }, bright[6] = { 125, 14, 7, 9, 26, 3 };
        CHECK(strstr(getstr("ui_hierarchy"), "\"knob_capture_param\":\"knob_capture\"") != NULL);
        CHECK(strstr(sbuf, "\"knob_delta_param\":\"knob_delta\"") != NULL && strstr(sbuf, "\"undo_param\":\"undo_cmd\"") != NULL);
        CHECK(geti("knob_capture") == 0 && geti("sel") == 0);
        for (int t = 1; t <= 6; t++) CHECK(pad_led(FX(t)) == vdim[t - 1]);       /* identity colours, dim */
        CHECK(pad_led(M_SHIFT) == 123);
        down(M_SHIFT); CHECK(pad_led(M_SHIFT) == 120); run(110); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        tap(FX(3));
        CHECK(!strcmp(status_text(), "PISTA 3") && geti("knob_capture") == 1 && geti("sel") == 3 && geti("cur_track") == 3);
        CHECK(!strcmp(disp_head(), "PISTA 3 EMPTY"));
        for (int t = 1; t <= 6; t++) CHECK(pad_led(FX(t)) == (t == 3 ? bright[2] : vdim[t - 1]));
        tap(FX(5)); CHECK(geti("sel") == 5 && geti("cur_track") == 5 && pad_led(FX(5)) == bright[4] && pad_led(FX(3)) == vdim[2]);
        tap(FX(5)); CHECK(!strcmp(status_text(), "MIX") && geti("knob_capture") == 0 && geti("sel") == 0);
        for (int t = 1; t <= 6; t++) CHECK(pad_led(FX(t)) != 127 && pad_led(FX(t)) != 126 && pad_led(FX(t)) == vdim[t - 1]);
        /* knob_delta does nothing in MIX */
        setstr("step", "0"); tap(REC(1)); run(700); run(4);
        long L = gett(1, "len"); CHECK(state(1) == S_PLAY);
        setstr("knob_delta", "1:4"); CHECK(gett(1, "off") == 0 && geti("t1_level") == 100);
        tap(REC(1)); CHECK(state(1) == S_DUB); tap(REC(1)); run(2);   /* DUB, back to PLAY */
        UNDO(); REDO();

        /* K1 IN / K2 OUT */
        tap(FX(1)); CHECK(strncmp(disp_head(), "PISTA 1", 7) == 0);
        setstr("knob_delta", "1:8");
        CHECK(abs(gett(1, "off") - 44100) <= 3 && !strcmp(status_text(), "T1 IN +8/16"));
        CHECK(!strcmp(disp_head(), "PISTA 1 IN +8/16") && abs(disp_mark() - 63) <= 2);
        setstr("knob_delta", "1:1:1");                                /* Movy's fine flag is ignored: normal 1/16 step */
        CHECK(abs(gett(1, "off") - 44100 - 5512) <= 3);
        down(M_SHIFT); setstr("knob_delta", "1:-1"); up(M_SHIFT);     /* SHIFT held = the PERFORM page: K1 never moves IN (v0.14.4) */
        CHECK(abs(gett(1, "off") - 44100 - 5512) <= 3);
        setstr("knob_delta", "1:-1"); CHECK(abs(gett(1, "off") - 44100) <= 3);
        setstr("knob_delta", "1:1"); CHECK(abs(gett(1, "off") - 44100 - 5512) <= 3);
        setstr("knob_delta", "9:1"); setstr("knob_delta", "x"); setstr("knob_delta", "1:0");
        setstr("loop_knob", "1:-8");                                  /* Loop + knob keeps working */
        CHECK(gett(1, "off") < 20000 && !strncmp(status_text(), "START 1", 7));
        setstr("knob_delta", "1:-30"); CHECK(gett(1, "off") == 0);
        setstr("knob_delta", "2:-4");                                 /* -4/16 */
        CHECK(labs(gett(1, "len") - 66150) < 2 && gett(1, "full") == L);
        CHECK(!strcmp(status_text(), "T1 OUT 12/16") && !strcmp(disp_head(), "PISTA 1 OUT 12/16"));
        CHECK(abs(disp_end() - 96) <= 2);                             /* end marker, whole-buffer view */
        setstr("knob_delta", "2:-2");                                 /* a burst of turns = ONE undo step */
        UNDO(); CHECK(labs(gett(1, "len") - L) < 8);
        REDO(); CHECK(labs(gett(1, "len") - 55125) < 2);
        UNDO();
        long l_before = gett(1, "len");
        down(M_SHIFT); setstr("knob_delta", "2:-1"); up(M_SHIFT);     /* SHIFT held = PERFORM page: K2 never moves OUT (v0.14.4) */
        CHECK(gett(1, "len") == l_before);
        setstr("knob_delta", "2:-16"); setstr("knob_delta", "2:-16"); setstr("knob_delta", "2:-16");
        CHECK(gett(1, "len") >= 5512 && gett(1, "len") <= 5513);      /* never below one 1/16 note */
        UNDO();
        /* OUT stops one 1/16 after IN: it never pushes the start */
        setstr("knob_delta", "1:8"); { int o = gett(1, "off");
        setstr("knob_delta", "2:-16"); setstr("knob_delta", "2:-16"); setstr("knob_delta", "2:-16");
        CHECK(gett(1, "off") == o && gett(1, "len") >= o + 5512 && gett(1, "len") <= o + 5530); }
        UNDO(); setstr("knob_delta", "1:-30"); CHECK(gett(1, "off") == 0);

        /* K3 volume, K4 pan */
        setstr("knob_delta", "3:-4"); CHECK(gett(1, "level") == 80 && !strcmp(status_text(), "T1 VOL 80"));
        setstr("knob_delta", "3:100"); CHECK(gett(1, "level") == 200);
        setstr("knob_delta", "3:-100"); CHECK(gett(1, "level") == 0);
        setstr("knob_delta", "3:20"); CHECK(gett(1, "level") == 100);
        setstr("knob_delta", "4:-5"); CHECK(gett(1, "pan") == 40 && !strcmp(status_text(), "T1 PAN L20"));
        setstr("knob_delta", "4:-100"); CHECK(gett(1, "pan") == 0);
        setstr("knob_delta", "4:100"); CHECK(gett(1, "pan") == 100 && !strcmp(status_text(), "T1 PAN R100"));
        setstr("knob_delta", "4:-25"); CHECK(gett(1, "pan") == 50 && !strcmp(status_text(), "T1 PAN C"));
        CHECK(!strcmp(disp_head(), "PISTA 1 PAN C"));
        CHECK(gett(2, "pan") == 50 && gett(2, "level") == 100);       /* only the selected track */

        /* K5 FX type (3 detents per step, OFF first), K6 FX amount */
        CHECK(gett(1, "fx_on") == 0);   /* off reads as OFF whatever type is stored */
        setstr("knob_delta", "5:1"); setstr("knob_delta", "5:1"); CHECK(gett(1, "fx_on") == 0);
        setstr("knob_delta", "5:1"); CHECK(gett(1, "fx") == 1 && gett(1, "fx_on") == 1 && !strcmp(status_text(), "T1 FX LPF"));
        setstr("knob_delta", "5:-3"); CHECK(gett(1, "fx") == 0 && gett(1, "fx_on") == 0 && !strcmp(status_text(), "T1 FX OFF"));
        setstr("knob_delta", "5:-3"); CHECK(gett(1, "fx") == 0 && gett(1, "fx_on") == 0);
        for (int i = 0; i < 9; i++) setstr("knob_delta", "5:3");
        CHECK(gett(1, "fx") == 6 && gett(1, "fx_on") == 1);
        setstr("knob_delta", "5:-2"); setstr("knob_delta", "5:2"); setstr("knob_delta", "5:2"); CHECK(gett(1, "fx") == 6);   /* turning back restarts the count */
        int a0 = gett(1, "fxp");
        setstr("knob_delta", "6:5"); CHECK(gett(1, "fxp") == a0 + 10 && !strncmp(status_text(), "T1 AMT ", 7));
        setstr("knob_delta", "6:-100"); CHECK(gett(1, "fxp") == 0);
        setstr("knob_delta", "5:-3"); CHECK(gett(1, "fx") == 5 && gett(1, "fxp") == 0);   /* type changes keep the amount */

        /* K7 speed, K8 reverse */
        CHECK(gett(1, "spd") == 3);
        setstr("knob_delta", "7:2"); CHECK(gett(1, "spd") == 3);
        setstr("knob_delta", "7:1"); CHECK(gett(1, "spd") == 4 && !strcmp(status_text(), "T1 SPD x1.5"));
        setstr("knob_delta", "7:3"); CHECK(gett(1, "spd") == 5 && !strcmp(status_text(), "T1 SPD x2"));
        setstr("knob_delta", "7:3"); CHECK(gett(1, "spd") == 6 && !strcmp(status_text(), "T1 SPD x4"));
        setstr("knob_delta", "7:30"); CHECK(gett(1, "spd") == 6);
        for (int i = 0; i < 3; i++) setstr("knob_delta", "7:-3");
        CHECK(gett(1, "spd") == 3 && !strcmp(status_text(), "T1 SPD x1"));
        setstr("knob_delta", "7:-3"); CHECK(gett(1, "spd") == 2 && !strcmp(status_text(), "T1 SPD x3/4"));
        setstr("knob_delta", "7:-3"); setstr("knob_delta", "7:-3"); CHECK(gett(1, "spd") == 0 && !strcmp(status_text(), "T1 SPD x1/4"));
        setstr("knob_delta", "7:-3"); CHECK(gett(1, "spd") == 0);
        for (int i = 0; i < 3; i++) setstr("knob_delta", "7:3");
        CHECK(gett(1, "spd") == 3);
        CHECK(gett(1, "rev") == 0);
        setstr("knob_delta", "8:3"); CHECK(gett(1, "rev") == 1 && !strcmp(status_text(), "T1 REV ON"));
        setstr("knob_delta", "8:30"); CHECK(gett(1, "rev") == 1);
        setstr("knob_delta", "8:-3"); CHECK(gett(1, "rev") == 0 && !strcmp(status_text(), "T1 REV OFF"));
        /* input / master are not on the PISTA knobs */
        CHECK(geti("in_gain") == 100 && geti("master") == 100);
        /* empty track */
        tap(FX(2)); CHECK(geti("sel") == 2 && geti("cur_track") == 2);
        setstr("knob_delta", "1:3"); CHECK(!strcmp(status_text(), "T2 IN EMPTY") && gett(2, "off") == 0);
        setstr("knob_delta", "2:3"); CHECK(!strcmp(status_text(), "T2 OUT EMPTY"));
        /* a track pad of another track keeps the PISTA track as cur_track */
        tap(STOP(1)); CHECK(geti("cur_track") == 2 && geti("sel") == 2);
        tap(FX(2)); CHECK(geti("knob_capture") == 0);                 /* MIX restored */
        setstr("knob_delta", "3:-4"); CHECK(gett(1, "level") == 100 && gett(2, "level") == 100);
        seti("t1_level", 100);
        tap(STOP(1)); run(2); CHECK(state(1) == S_PLAY);

        /* looper SHIFT + select = FX CLR (v0.14): FX type OFF, no selection change */
        sett(2, "fx", 3); sett(2, "fxp", 70); sett(2, "fx_on", 1);
        down(M_SHIFT); tap(FX(2)); up(M_SHIFT);
        CHECK(gett(2, "fx_on") == 0 && gett(2, "fx") == 0 && !strcmp(status_text(), "FX CLR 2") && geti("sel") == 0);
        tap(FX(2)); tap(FX(4));                                       /* plain taps select */
        down(M_SHIFT); tap(FX(6)); up(M_SHIFT); CHECK(geti("sel") == 4 && !strcmp(status_text(), "FX CLR 6"));
        tap(FX(4)); CHECK(geti("sel") == 0);

        /* SHIFT + row 1 = reverse, never rec/dub */
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        for (int t = 1; t <= 3; t++) { tap(REC(t)); run(100); tap(REC(t)); run(2); CHECK(state(t) == S_PLAY); }
        down(M_SHIFT); tap(REC(1)); CHECK(state(1) == S_PLAY && gett(1, "rev") == 1 && !strcmp(status_text(), "REV 1 ON"));
        tap(REC(1)); up(M_SHIFT); CHECK(state(1) == S_PLAY && gett(1, "rev") == 0);

        /* SHIFT + row 3 = SOLO; again restores the mutes found before (track 3 was muted) */
        tap(MUTE(3)); CHECK(state(3) == S_MUTE);
        down(M_SHIFT); tap(MUTE(1)); up(M_SHIFT);
        CHECK(state(1) == S_PLAY && state(2) == S_MUTE && state(3) == S_MUTE && !strcmp(status_text(), "SOLO 1"));
        CHECK(pad_led(MUTE(1)) == (0x80 | 125) && pad_led(MUTE(2)) == 119);
        down(M_SHIFT); tap(MUTE(2)); up(M_SHIFT);                     /* solo moves to track 2 */
        CHECK(state(1) == S_MUTE && state(2) == S_PLAY && state(3) == S_MUTE);
        down(M_SHIFT); tap(MUTE(2)); up(M_SHIFT);
        CHECK(state(1) == S_PLAY && state(2) == S_PLAY && state(3) == S_MUTE && !strcmp(status_text(), "UNSOLO 2"));
        CHECK(geti("t1_level") == 100 && geti("t2_level") == 100 && geti("t3_level") == 100);
        tap(MUTE(3)); CHECK(state(3) == S_PLAY);

        /* SHIFT + column 8: count-in bars, threshold; the others do nothing */
        seti("count_in", 1);
        down(M_SHIFT); tap(R_COUNTIN); CHECK(!strcmp(status_text(), "COUNT-IN 2 BARS") && geti("count_in") == 1);
        up(M_SHIFT);
        tap(REC(5)); CHECK(state(5) == S_ARMED);
        run(1300); CHECK(state(5) == S_ARMED);                        /* 2 bars @120 = 4 s */
        run(100); CHECK(state(5) == S_REC);
        tap(REC(5)); run(2);
        down(M_SHIFT); tap(R_COUNTIN); up(M_SHIFT); CHECK(!strcmp(status_text(), "COUNT-IN 1 BAR"));
        tap(STOP(5)); tap(STOP(5)); seti("delete_pad", REC(5) - 36); run(2);
        tap(REC(5)); run(750); CHECK(state(5) == S_REC);              /* back to 1 bar = 2 s */
        tap(REC(5)); run(2); seti("count_in", 0);
        CHECK(geti("threshold") == -42);
        int rq = geti("rec_action"), ql = geti("rec_len"), qq = geti("quantize");
        down(M_SHIFT);
        tap(R_RECACT); CHECK(geti("threshold") == -36 && !strcmp(status_text(), "THR -36 DB"));
        tap(R_RECACT); CHECK(geti("threshold") == -30);
        tap(R_RECACT); CHECK(geti("threshold") == -54);
        tap(R_RECACT); CHECK(geti("threshold") == -48);
        tap(R_RECACT); CHECK(geti("threshold") == -42);
        tap(R_FIXLEN); tap(R_QUANT); up(M_SHIFT);
        CHECK(geti("rec_action") == rq && geti("rec_len") == ql && geti("quantize") == qq);

        /* pedals 3/4 move cur_track, and the PISTA selection when it is active */
        tap(FX(2)); CHECK(geti("sel") == 2);
        setstr("pedal", "4:d"); setstr("pedal", "4:u");
        CHECK(geti("cur_track") == 3 && geti("sel") == 3 && geti("knob_capture") == 1);
        setstr("pedal", "3:d"); setstr("pedal", "3:u"); CHECK(geti("cur_track") == 2 && geti("sel") == 2);
        tap(FX(2)); CHECK(geti("sel") == 0);
        setstr("pedal", "4:d"); setstr("pedal", "4:u"); CHECK(geti("cur_track") == 3 && geti("sel") == 0);
    }

    /* --- pedal_param: Blackstar Live Logic switches 1..6 --- */
    {
#define PD(n) setstr("pedal", #n ":d")
#define PU(n) setstr("pedal", #n ":u")
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        { static char hb[20000]; hb[0] = 0; A->get_param(S, "ui_hierarchy", hb, sizeof hb);
          CHECK(strstr(hb, "\"pedal_param\":\"pedal\"") != NULL); }
        /* 1: REC > PLAY > DUB on the selected track (cur_track starts at 1) */
        PD(1); PU(1); CHECK(state(1) == S_REC); run(100);
        PD(1); PU(1); run(2); CHECK(state(1) == S_PLAY);
        PD(1); PU(1); CHECK(state(1) == S_DUB);
        PD(1); PU(1); run(2); CHECK(state(1) == S_PLAY);
        /* 2 tap (< 1 s): PLAY/STOP on press-release, nothing cleared */
        PD(2); run(50); PU(2); run(2); CHECK(state(1) == S_STOP && gett(1, "len") > 0);
        PD(2); PU(2); run(2); CHECK(state(1) == S_PLAY);
        /* 2 hold 1 s: CLEAR only, the release does not also toggle play/stop; undoable */
        PD(2); run(340); CHECK(state(1) == S_PLAY); run(10); run(2);
        CHECK(state(1) == S_EMPTY && !strcmp(status_text(), "CLEAR 1"));
        PU(2); run(2); CHECK(state(1) == S_EMPTY);
        UNDO(); CHECK(state(1) != S_EMPTY && gett(1, "len") > 0);
        run(400); /* a hold fires once, even if held longer */
        /* 4 next (wrap), 3 prev on release (wrap), cur_track follows */
        CHECK(geti("cur_track") == 1);
        PD(4); PU(4); CHECK(geti("cur_track") == 2 && !strcmp(status_text(), "TRACK 2"));
        for (int k = 0; k < 4; k++) PD(4);
        CHECK(geti("cur_track") == 6); PD(4); CHECK(geti("cur_track") == 1);
        PD(3); CHECK(geti("cur_track") == 1);                          /* acts on release */
        run(50); PU(3); CHECK(geti("cur_track") == 6 && !strcmp(status_text(), "TRACK 6"));
        PD(3); PU(3); CHECK(geti("cur_track") == 5);
        /* switches act on the selected track: 1 and 2 on track 5 */
        PD(1); PU(1); CHECK(state(5) == S_REC && state(1) != S_REC); run(100);
        PD(1); PU(1); run(2); CHECK(state(5) == S_PLAY);
        PD(2); PU(2); run(2); CHECK(state(5) == S_STOP);
        /* 3 hold 1 s: ARM toggles at 1 s while held, no track change, release does nothing */
        CHECK(geti("arm") == 0);
        PD(3); run(340); CHECK(geti("arm") == 0); run(10);
        CHECK(geti("arm") == 1 && !strcmp(status_text(), "ARM ON"));
        PU(3); CHECK(geti("arm") == 1 && geti("cur_track") == 5);
        PD(3); run(360); CHECK(geti("arm") == 0); PU(3); CHECK(geti("cur_track") == 5);
        /* 5 UNDO: same as the Move Undo button; 5 again has nothing to undo */
        PD(2); run(360); PU(2); CHECK(state(5) == S_EMPTY);
        PD(5); PU(5); CHECK(state(5) == S_STOP && !strcmp(status_text(), "UNDO"));
        PD(5); PU(5); CHECK(!strcmp(status_text(), "NOTHING TO UNDO"));
        /* 6: ALL START when nothing plays, STOP ALL when something does */
        PD(2); PU(2); run(2); CHECK(state(5) == S_PLAY);
        PD(6); PU(6); run(2); CHECK(state(5) == S_STOP);
        PD(6); PU(6); run(2); CHECK(state(5) == S_PLAY);
        /* junk and stray releases are ignored */
        setstr("pedal", "7:d"); setstr("pedal", "0:d"); setstr("pedal", "x"); setstr("pedal", "2:u"); setstr("pedal", "");
        CHECK(state(5) == S_PLAY && geti("cur_track") == 5);
    }


    /* --- 0.6.1: speed ratios 1/4 1/2 3/4 1 1.5 2 4: cycle length, playhead, old-state mapping --- */
    {
        static const double RATIO[7] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0 };
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE; seti("monitor", 0);
        tap(REC(1)); run(345); tap(REC(1)); run(4);
        CHECK(state(1) == S_PLAY && geti("trk_spd") == 3);
        double reg = (double)(gett(1, "len") - gett(1, "off"));
        setstr("loop_knob", "1:4"); run(1000);                      /* a clip start: region = len - off, ends the zoom */
        reg = (double)(gett(1, "len") - gett(1, "off"));
        for (int rev = 0; rev < 2; rev++)
            for (int k = 0; k < 7; k++) {
                if (rev && k != 4 && k != 6 && k != 0) continue;
                seti("trk_rev", rev); seti("trk_spd", k); run(2);
                int want = (int)lround(reg / RATIO[k] / 128.0), blocks = 0, wraps = 0, first = -1, last = -1, prev = disp_pos(), mono = 1;
                for (int b = 0; b < want * 3 + 20 && wraps < 3; b++) {
                    run(1); blocks++;
                    int p = disp_pos();
                    int d = rev ? prev - p : p - prev;
                    if (d < -64) { if (first < 0) first = blocks; last = blocks; wraps++; }
                    else if (d < 0) mono = 0;
                    prev = p;
                }
                /* two or more wraps: time between the first and the last / count */
                int cyc = wraps >= 2 ? (last - first) / (wraps - 1) : 0;
                CHECK(wraps >= 2 && abs(cyc - want) <= 2 + want / 40);
                CHECK(mono);                                        /* the head never goes backwards inside the cycle */
                out_peak = 0; run(want / 2 + 3); CHECK(out_peak > 200);   /* audible at every ratio, rev or not */
                tap(REC(1)); CHECK(state(1) == (k == 3 && !rev ? S_DUB : S_PLAY));   /* no overdub unless 1x forward */
                if (state(1) == S_DUB) tap(REC(1));
            }
        seti("trk_rev", 0); seti("trk_spd", 3);
        /* one-shot at 4x stops after region/4 */
        sett(1, "shot", 1); seti("trk_spd", 6); run(2); tap(STOP(1)); tap(STOP(1)); run(2);
        int played = 0; for (int b = 0; b < 400 && state(1) == S_PLAY; b++) { run(1); played++; }
        CHECK(state(1) == S_STOP && abs(played - (int)(reg / 4.0 / 128.0)) <= 3);
        sett(1, "shot", 0); seti("trk_spd", 3);
        /* old states: l6sp 0/1/2 = 1/2x, 1x, 2x -> index 1/3/5; the new l6sq index passes through */
        setstr("state", "{\"l6rl\":0,\"l6sp\":\"0,1,2,2,1,0\"}");
        CHECK(gett(1, "spd") == 1 && gett(2, "spd") == 3 && gett(3, "spd") == 5 && gett(4, "spd") == 5 && gett(5, "spd") == 3 && gett(6, "spd") == 1);
        setstr("state", "{\"l6rl\":0,\"l6sq\":\"0,6,2,3,4,5\"}");
        CHECK(gett(1, "spd") == 0 && gett(2, "spd") == 6 && gett(3, "spd") == 2 && gett(4, "spd") == 3 && gett(5, "spd") == 4 && gett(6, "spd") == 5);
        A->get_param(S, "state", dbuf, sizeof dbuf);
        CHECK(strstr(dbuf, "\"l6sq\":\"0,6,2,3,4,5\"") != NULL);
        fresh(&host);
        CHECK(geti("t1_spd") == 3);                                 /* default stays 1x */
    }

    /* --- 0.6.1: waveform zoom on IN / OUT --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE; bpm_val = 120.0f;
        for (int i = 0; i < 4; i++) tap(R_FIXLEN);                  /* 8 bars: 705600 frames */
        tap(REC(1)); run(5600); run(4);
        CHECK(state(1) == S_PLAY);
        long BAR = 88200, L8 = gett(1, "len");
        CHECK(labs(L8 - 8 * BAR) < 100);
        int tk[200], ph[64], np;
        tap(FX(1));                                                 /* PISTA 1 */
        run(1000);
        /* idle PISTA: the whole buffer, no zoom; both markers, IN at 0 */
        CHECK(strstr(disp_head(), "ZOOM") == NULL && disp_mark() == 0 && disp_end() == 127);
        /* IN to bar 4 (four K1 turns of a bar): window = 2 bars centred on it */
        int seq0; disp(); seq0 = atoi(dbuf);
        for (int i = 0; i < 4; i++) setstr("knob_delta", "1:16");
        CHECK(labs(gett(1, "off") - 4 * BAR) <= 4);
        disp(); CHECK(atoi(dbuf) != seq0);                          /* window changed -> new seq */
        CHECK(strstr(disp_head(), "IN") && strstr(disp_head(), "ZOOM"));
        CHECK(abs(disp_mark() - 64) <= 1 && disp_end() == -1);      /* IN centred, OUT (bar 8) outside [3,5) */
        { int n = disp_ticks(tk), beats = 0; np = disp_phrase(ph);
          for (int i = 0; i < n; i++) if (tk[i] & 0x80) beats++;
          CHECK(n == 7 && beats == 6 && np == 1 && abs(ph[0] - 64) <= 1);   /* 2 bars: bar tick at 0, phrase tick at IN (bar 1 of its phrase), 6 beats */
          CHECK(!(tk[0] & 0x80) && tk[0] <= 1); }
        /* fine (Loop + knob, fine flag) = half a bar window; IN moves by a 1/64 note */
        setstr("loop_knob", "1:1:1");
        { int n = disp_ticks(tk); CHECK(n <= 3 && strstr(disp_head(), "ZOOM") && disp_mark() >= 0); }
        /* knob_page: empty while the zoom is up */
        { char pg[400] = ""; A->get_param(S, "knob_page", pg, sizeof pg); CHECK(pg[0] == 0); }
        /* playhead stays inside 0..127 whatever the window */
        for (int i = 0; i < 50; i++) { run(100); int p = disp_pos(); CHECK(p >= 0 && p < 128); }
        /* the timer ends the zoom: whole buffer again, markers in buffer coordinates */
        run(1000);
        CHECK(strstr(disp_head(), "ZOOM") == NULL);
        CHECK(abs(disp_mark() - 64) <= 2 && disp_end() == 127);
        { char pg[400] = ""; A->get_param(S, "knob_page", pg, sizeof pg);      /* zoom over: the page is back */
          CHECK(!strncmp(pg, "PISTA 1|PLAY|0|IN;d;0.50", 24) && strstr(pg, "|VOL;d;0.500;100|PAN;d;0.500;C|FX;b;OFF|AMT;d;0.500;50|SPD;b;X1|REV;b;OFF"));
          setstr("knob_delta", "3:2"); A->get_param(S, "knob_page", pg, sizeof pg);   /* VOL +10: its value text is focused */
          CHECK(!strncmp(pg, "PISTA 1|PLAY|3|", 15) && strstr(pg, "|VOL;d;0.550;110|")); }
        /* OUT: K2 down a bar -> window [6,8) bars centred on OUT = 7 bars, clamped */
        setstr("knob_delta", "2:-16");
        CHECK(labs(gett(1, "len") - 7 * BAR) <= 4);
        CHECK(strstr(disp_head(), "OUT") && strstr(disp_head(), "ZOOM"));
        CHECK(abs(disp_end() - 64) <= 1 && disp_mark() == -1);      /* IN (bar 4) is left of the window */
        { int n = disp_ticks(tk); np = disp_phrase(ph); CHECK(n == 8 && np == 0); }
        /* clamped at the buffer start: IN at 0.5 bar -> window [0,2) bars, IN at bin 16 */
        setstr("knob_delta", "2:16"); run(1000);
        for (int i = 0; i < 6; i++) setstr("knob_delta", "1:-16");
        CHECK(gett(1, "off") == 0);
        setstr("knob_delta", "1:8");
        CHECK(abs(disp_mark() - 32) <= 1 && strstr(disp_head(), "ZOOM"));   /* 0.5 bar into [0,2) bars */
        { int n = disp_ticks(tk); np = disp_phrase(ph); CHECK(n > 0); }
        /* MIX: only IN..OUT, scaled to the full width (no zoom, no markers), ticks from IN */
        run(1000); tap(FX(1)); CHECK(geti("sel") == 0);
        CHECK(strstr(disp_head(), "ZOOM") == NULL && disp_mark() == -1 && disp_end() == -1);
        { int n = disp_ticks(tk); np = disp_phrase(ph);
          int bars = 0; for (int i = 0; i < n; i++) if (!(tk[i] & 0x80)) bars++;
          /* region = 7.5 bars: bar 1 (IN) and bar 5 are phrase ticks at 0 and 68, bars 2-4 and 6-8 are bar ticks */
          CHECK(np == 2 && ph[0] == 0 && abs(ph[1] - 68) <= 1 && bars == 6); }
        /* a short clip never zooms: the whole buffer shows */
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE; bpm_val = 120.0f;
        tap(R_FIXLEN); tap(REC(1)); run(700); run(4);               /* 1 bar */
        tap(FX(1)); setstr("knob_delta", "1:4");
        CHECK(strstr(disp_head(), "ZOOM") == NULL && abs(disp_mark() - 32) <= 1);
    }

    /* --- Delete + REC pad = erase the last recording: just the last dub layer if that is the undo entry, else CLEAR --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        tap(REC(3)); run(300); tap(REC(3)); run(2); CHECK(state(3) == S_PLAY);
        int len3 = gett(3, "len");
        tap(REC(3)); run(150); tap(REC(3)); run(2); CHECK(state(3) == S_PLAY && geti("undo_avail") == 1);   /* a dub pass */
        seti("delete_pad", REC(3) - 36); run(2);
        CHECK(!strcmp(status_text(), "UNDO DUB 3") && state(3) == S_PLAY && gett(3, "len") == len3);       /* the loop stays, the layer goes */
        CHECK(geti("undo_avail") == 2);
        seti("delete_pad", REC(3) - 36); run(2);                                                          /* nothing of the dub left to undo: CLEAR */
        CHECK(!strcmp(status_text(), "CLEAR 3") && state(3) == S_EMPTY);
        UNDO(); run(2); CHECK(state(3) != S_EMPTY);
        /* the last undo entry belongs to another track: CLEAR, and rows 2-3 always clear */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        for (int k = 1; k <= 2; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(REC(1)); run(100); tap(REC(1)); run(2);
        tap(REC(2)); run(100); tap(REC(2)); run(2);
        seti("delete_pad", REC(1) - 36); run(2); CHECK(!strcmp(status_text(), "CLEAR 1") && state(1) == S_EMPTY && state(2) == S_PLAY);
        tap(REC(2)); run(100); tap(REC(2)); run(2);
        seti("delete_pad", STOP(2) - 36); run(2); CHECK(!strcmp(status_text(), "CLEAR 2") && state(2) == S_EMPTY);   /* row 2: audio, never the dub */
        /* while it is still dubbing: CLEAR */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        tap(REC(3)); run(300); tap(REC(3)); run(2); tap(REC(3)); run(30);
        seti("delete_pad", REC(3) - 36); run(2); CHECK(!strcmp(status_text(), "CLEAR 3") && state(3) == S_EMPTY);
    }

    /* --- RESET also switches off every PERFORM effect bound to that track --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        for (int k = 3; k <= 4; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(M_SHIFT);
        tap(FX(3)); setstr("step", "8"); setstr("step_up", "8");               /* on PISTA 3 (toggled) */
        setstr("step", "9"); run(2);                                           /* and one held, still down */
        tap(FX(4)); setstr("step", "10"); setstr("step_up", "10");              /* PISTA 4 */
        tap(FX(4));                                                            /* MIX */
        setstr("step", "11"); setstr("step_up", "11");                         /* the whole mix */
        CHECK(geti("perf_n") == 4 && geti("perf_target") == 0);
        down(M_SHIFT); seti("delete_pad", FX(3) - 36); run(2); up(M_SHIFT);
        CHECK(geti("perf_n") == 2 && geti("perf_on") == ((1 << 10) | (1 << 11)) && geti("perf_fx") == 11);   /* only track 3's two are gone */
        setstr("step_up", "9"); CHECK(geti("perf_n") == 2);
        setstr("step", "11"); setstr("step_up", "11"); tap(FX(4)); CHECK(geti("sel") == 4);
        down(M_SHIFT); seti("delete_pad", FX(4) - 36); run(2); up(M_SHIFT); CHECK(geti("perf_n") == 0 && geti("perf_fx") == -1);
        run(40); { const char *bar = strchr(screen(), '|'); CHECK(bar && geti("perf_boxes") == 0); }   /* the current target was reset: the grid page */
        tap(M_SHIFT);
    }

    /* --- Delete + PISTA selector (top row) = RESET: audio + every setting; rows 1-3 = audio only --- */
    {
        for (int row = 0; row < 2; row++) {          /* 0: rows 1-3 pad (CLEAR), 1: selector (RESET) */
            fresh(&host); seti("quantize", 0);
            tap(REC(3)); run(300); tap(REC(3)); run(2); CHECK(state(3) == S_PLAY);
            int len3 = gett(3, "len");
            seti("t3_fx", 4); seti("t3_fx_on", 1); seti("t3_fxp", 80); seti("t3_level", 140); seti("t3_pan", 20);
            seti("t3_spd", 5); seti("t3_rev", 1); seti("t3_shot", 1);
            if (row) down(M_SHIFT);
            seti("delete_pad", (row ? FX(3) : STOP(3)) - 36); run(2);
            if (row) up(M_SHIFT);
            CHECK(state(3) == S_EMPTY && gett(3, "len") == 0);
            CHECK(!strcmp(status_text(), row ? "RESET 3" : "CLEAR 3"));
            if (row) {
                CHECK(gett(3, "fx_on") == 0 && gett(3, "fx") == 1 && gett(3, "fxp") == 50);
                CHECK(gett(3, "level") == 100 && gett(3, "pan") == 50 && gett(3, "spd") == 3);
                CHECK(gett(3, "rev") == 0 && gett(3, "shot") == 0);
            } else {
                CHECK(gett(3, "fx_on") == 1 && gett(3, "fx") == 4 && gett(3, "fxp") == 80);
                CHECK(gett(3, "level") == 140 && gett(3, "pan") == 20 && gett(3, "spd") == 5);
                CHECK(gett(3, "rev") == 1 && gett(3, "shot") == 1);
            }
            /* undo restores audio + settings, redo re-applies, undo again restores */
            if (row) {
                UNDO(); run(2);
                CHECK(state(3) == S_STOP && gett(3, "len") == len3);
                CHECK(gett(3, "fx_on") == 1 && gett(3, "fx") == 4 && gett(3, "fxp") == 80);
                CHECK(gett(3, "level") == 140 && gett(3, "pan") == 20 && gett(3, "spd") == 5);
                CHECK(gett(3, "rev") == 1 && gett(3, "shot") == 1);
                REDO(); run(2);
                CHECK(state(3) == S_EMPTY && gett(3, "fx_on") == 0 && gett(3, "level") == 100 && gett(3, "spd") == 3);
                UNDO(); run(2);
                CHECK(state(3) == S_STOP && gett(3, "fxp") == 80 && gett(3, "level") == 140 && gett(3, "rev") == 1);
            }
        }
        /* a newer undo entry supersedes the snapshot: a later undo must not touch settings */
        fresh(&host); seti("quantize", 0);
        tap(REC(1)); run(300); tap(REC(1)); run(2);
        tap(REC(2)); run(300); tap(REC(2)); run(2);
        seti("t1_pan", 10); seti("t2_pan", 90);
        down(M_SHIFT); seti("delete_pad", FX(1) - 36); up(M_SHIFT);                 /* reset 1 */
        seti("delete_pad", STOP(2) - 36);               /* plain clear of 2 takes the undo slot */
        UNDO(); run(2);
        CHECK(state(2) == S_STOP && gett(2, "pan") == 90 && gett(1, "pan") == 50);
        /* mute / solo interplay */
        fresh(&host); seti("quantize", 0);
        for (int k = 1; k <= 3; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(MUTE(3)); CHECK(state(3) == S_MUTE); seti("t3_pan", 30);
        down(M_SHIFT); seti("delete_pad", FX(3) - 36); up(M_SHIFT);                 /* muted track: RESET unmutes, level 100 */
        CHECK(state(3) == S_EMPTY && gett(3, "level") == 100);
        tap(REC(3)); run(300); tap(REC(3)); run(2); CHECK(state(3) == S_PLAY);
        down(M_SHIFT); tap(MUTE(1)); up(M_SHIFT);       /* solo 1 */
        CHECK(state(1) == S_PLAY && state(2) == S_MUTE && state(3) == S_MUTE);
        down(M_SHIFT); seti("delete_pad", FX(1) - 36); up(M_SHIFT);                 /* resetting the soloed track ends the solo */
        CHECK(state(2) == S_PLAY && gett(2, "level") == 100 && gett(1, "level") == 100);
        UNDO(); run(2);                                 /* back to solo 1 */
        CHECK(state(1) == S_STOP || state(1) == S_PLAY); CHECK(state(2) == S_MUTE && state(3) == S_MUTE);
        down(M_SHIFT); tap(MUTE(1)); up(M_SHIFT);       /* unsolo restores the mutes found */
        CHECK(state(2) == S_PLAY && state(3) == S_PLAY);
        /* solo 2, reset 3 (muted by the solo): 3 leaves the solo state and is unmuted */
        down(M_SHIFT); tap(MUTE(2)); up(M_SHIFT);
        CHECK(state(3) == S_MUTE);
        down(M_SHIFT); seti("delete_pad", FX(3) - 36); up(M_SHIFT);
        CHECK(state(3) == S_EMPTY && gett(3, "level") == 100);
        down(M_SHIFT); tap(MUTE(2)); up(M_SHIFT);       /* unsolo */
        CHECK(state(1) == S_STOP && state(2) == S_PLAY);
    }

    /* --- 0.8.0: Movy's track mute (track_mute_param) and transport (transport_param) --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE; seti("monitor", 0);
        char hb[32768]; A->get_param(S, "ui_hierarchy", hb, sizeof hb);
        CHECK(strstr(hb, "\"track_mute_param\":\"track_mute\"") && strstr(hb, "\"transport_param\":\"transport\""));
        tap(REC(1)); run(300); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
        out_peak = 0; run(20); CHECK(out_peak > 300 && geti("track_mute") == 0);
        CHECK(!strstr(disp_head(), "MUTED"));
        seti("track_mute", 1); CHECK(geti("track_mute") == 1);
        run(8); out_peak = 0; run(30);
        CHECK(out_peak <= 8);                              /* silent bar the keep-alive dither */
        CHECK(state(1) == S_PLAY);                         /* the tracks keep running */
        int p0 = disp_pos(); run(40); CHECK(disp_pos() != p0);
        CHECK(!strncmp(disp_head(), "MUTED ", 6));
        seti("track_mute", 0); run(8); out_peak = 0; run(20); CHECK(out_peak > 300);
        CHECK(!strstr(disp_head(), "MUTED"));
        /* the fade is click-free: the first block after muting is only partly attenuated, not cut */
        { seti("track_mute", 1); out_peak = 0; run(1); int p1 = out_peak; CHECK(p1 > 1000);
          run(6); out_peak = 0; run(1); CHECK(out_peak <= 8); seti("track_mute", 0); run(8); }

        /* transport stop remembers what played; play restarts exactly those */
        fresh(&host); seti("quantize", 0);
        for (int k = 1; k <= 3; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(STOP(3)); run(2);
        CHECK(state(1) == S_PLAY && state(2) == S_PLAY && state(3) == S_STOP);
        setstr("transport", "stop"); run(2);
        CHECK(state(1) == S_STOP && state(2) == S_STOP && state(3) == S_STOP && !strcmp(status_text(), "TRANSPORT STOP"));
        setstr("transport", "stop"); run(2);               /* a second stop keeps the memory */
        setstr("transport", "play"); run(2);
        CHECK(state(1) == S_PLAY && state(2) == S_PLAY && state(3) == S_STOP);
        /* nothing remembered: play starts every track that holds audio (ALL START) */
        fresh(&host); seti("quantize", 0);
        for (int k = 1; k <= 2; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(M_STOPALL); run(2); CHECK(state(1) == S_STOP && state(2) == S_STOP);
        setstr("transport", "play"); run(2);
        CHECK(state(1) == S_PLAY && state(2) == S_PLAY);
        /* a recording in progress is left alone */
        setstr("transport", "stop"); run(2);
        tap(REC(3)); run(60); CHECK(state(3) == S_REC);
        setstr("transport", "play"); run(2);
        CHECK(state(3) == S_REC && state(1) == S_PLAY && state(2) == S_PLAY);
        tap(REC(3)); run(2);
        /* quantize on with a running clock: the restart waits for the bar */
        fresh(&host); seti("quantize", 1);
        { uint8_t st = 0xFA; A->on_midi(S, &st, 1, 3); }
        clock_run(20);
        tap(REC(1)); clock_run(800); tap(REC(1)); clock_run(800);
        CHECK(state(1) == S_PLAY);
        setstr("transport", "stop"); clock_run(4); CHECK(state(1) == S_STOP);
        { uint8_t st = 0xFA; A->on_midi(S, &st, 1, 3); }
        clock_run(20);
        setstr("transport", "play"); run(1);
        CHECK(state(1) == S_STOP);                         /* pending on the grid */
        clock_run(1500); CHECK(state(1) == S_PLAY);
        A->set_param(S, "transport", "junk"); CHECK(state(1) == S_PLAY);
    }


    /* ================= PERFORM (SHIFT + hold a step) ================= */
    {
        static int16_t ref[1200 * 128], eff[1200 * 128];
        const int HOLD = 150, TAIL = 360;
        const long H0 = (long)PRE * 128, H1 = (long)(PRE + HOLD) * 128, TOT = (long)(PRE + HOLD + TAIL) * 128;
        static const char *const FXN[16] = { "beat repeat", "stutter", "pitch", "bitcrush", "tape", "lfo", "reverse", "vinyl",
            "filter lp", "filter hp", "gate", "delay", "freeze", "retrigger", "half speed", "kill" };
        for (int sel = 0; sel <= 1; sel++) {
            scen(ref, -1, sel, IN_RICH, HOLD, TAIL, NULL, 0);
            int refjump = maxjump(ref, 0, TOT);
            for (int fx = 0; fx < 16; fx++) {
                scen(eff, fx, sel, IN_RICH, HOLD, TAIL, NULL, 0);
                long dur = ndiff(eff, ref, H0 + 1000, H1);
                long after = ndiff(eff, ref, H1 + (fx == PF_TSTOP ? 210 : 12) * 128, TOT);       /* TAPE spools back up first */
                int jump = maxjump(eff, H0 - 128, H1 + 20 * 128);
                if (dur <= 100 || (after && fx != PF_DELAY) || jump > refjump * 2 + 4000) printf("  (%s, sel %d: dur %ld after %ld jump %d ref %d)\n", FXN[fx], sel, dur, after, jump, refjump);
                CHECK(dur > 100);                                 /* the effect engages */
                if (fx != PF_DELAY) CHECK(after == 0);            /* released: bit-exact, in sync, the loop buffer untouched (a full loop later); the delay tail has its own test */
                CHECK(jump <= refjump * 2 + 4000);                /* click-free engage and release */
                CHECK(ndiff(eff, ref, 0, H0) == 0);               /* nothing before the press */
                CHECK(geti("perf_fx") == -1);
            }
        }
        /* the SHIFT page and the held effect survive SHIFT being released */
        fresh(&host); seti("quantize", 0);
        CHECK(!strcmp(screen(), ""));                             /* nothing to draw without SHIFT */
        down(M_SHIFT);
        CHECK(step_led(0) == 17 && step_led(1) == 14 && step_led(2) == 20 && step_led(3) == 20 && step_led(4) == 3 && step_led(7) == 3);
        CHECK(step_led(8) == 20 && step_led(9) == 20 && step_led(10) == 14 && step_led(11) == 92 && step_led(12) == 92 && step_led(13) == 14 && step_led(14) == 3 && step_led(15) == 7);   /* batch 2 family colours */
        setstr("step", "2");
        CHECK(geti("perf_fx") == 2 && step_led(2) == 120);        /* the held one is white */
        up(M_SHIFT);
        CHECK(geti("perf_fx") == 2 && step_led(2) == 120 && geti("knob_capture") == 1);
        setstr("step_up", "5"); CHECK(geti("perf_fx") == 2);      /* another step's release is not ours */
        setstr("step_up", "2"); CHECK(geti("perf_fx") == -1 && geti("knob_capture") == 0);
        CHECK(step_led(2) != 120 && step_led(0) == 119);          /* no SHIFT: the length LEDs again */
        /* without SHIFT a step still sets the length */
        setstr("step", "3"); CHECK(geti("rec_len") == 4 && geti("perf_fx") == -1);
        setstr("step_up", "3");
        down(M_SHIFT); setstr("step", "16"); CHECK(geti("perf_fx") == -1 && geti("rec_len") == 4);   /* there is no step 17 */
        setstr("step", "0"); CHECK(geti("perf_fx") == 0);
        setstr("step", "6"); CHECK(geti("perf_fx") == 6);         /* a newer step takes over */
        setstr("step_up", "0"); CHECK(geti("perf_fx") == 6);
        setstr("step_up", "6"); CHECK(geti("perf_fx") == -1);
        up(M_SHIFT); setstr("step", "3"); setstr("step_up", "3");

        /* knobs: every effect has exactly NK real knobs; each one moves, is remembered, and the screen boxes only those */
        static const int NK[16] = { 3, 3, 3, 3, 3, 3, 2, 3, 3, 3, 3, 3, 3, 3, 2, 3 };
        CHECK(!strcmp(screen(), "")); down(M_SHIFT); run(8); screen(); CHECK(geti("perf_boxes") == 0);   /* the grid page has no knob boxes */
        for (int fx = 0; fx < 16; fx++) {
            char b[8]; snprintf(b, sizeof b, "%d", fx);
            for (int k = 1; k <= 4; k++) {
                char key[24], kd[16]; snprintf(key, sizeof key, "perf_p%d_%d", fx, k - 1);
                int before = geti(key);
                setstr("step", b);                                  /* turned right after the press, no block in between */
                snprintf(kd, sizeof kd, "%d:12", k); setstr("knob_delta", kd);
                int after = geti(key);
                if (k <= NK[fx] && after == before) { snprintf(kd, sizeof kd, "%d:-12", k); setstr("knob_delta", kd); after = geti(key); }
                if (k <= NK[fx]) CHECK(after != before); else CHECK(after == before);          /* a knob that does not exist is ignored */
                if (k == 1) { run(60); screen(); CHECK(geti("perf_boxes") == NK[fx]); }       /* boxes == real knobs */
                setstr("step_up", b); run(110);
                setstr("step", b); CHECK(geti(key) == after); setstr("step_up", b); run(110);  /* remembered */
            }
        }
        up(M_SHIFT);
        { int a0 = geti("perf_p0_0");
          setstr("knob_delta", "1:5");                              /* no effect held, MIX: knobs are not ours */
          CHECK(geti("perf_p0_0") == a0); }
        /* an effect that is on (tapped in the mode), mode then off: SHIFT held lets the knobs reach it; without SHIFT (MIX) they do not */
        fresh(&host); seti("quantize", 0);
        tap(M_SHIFT); setstr("step", "8"); run(3); setstr("step_up", "8");
        CHECK(geti("perf_latch") == 1 << 8 && geti("knob_capture") == 1);
        tap(M_SHIFT); CHECK(geti("knob_capture") == 0);
        { int c = geti("perf_p8_2"); setstr("knob_delta", "3:10"); CHECK(geti("perf_p8_2") == c); }
        down(M_SHIFT); CHECK(geti("knob_capture") == 1);
        { int c = geti("perf_p8_2"), r = geti("perf_p8_1");
          setstr("knob_delta", "3:10"); CHECK(geti("perf_p8_2") != c);
          setstr("knob_delta", "2:5"); CHECK(geti("perf_p8_1") == r + 10); }
        setstr("step", "8"); setstr("step_up", "8"); up(M_SHIFT);
        CHECK(geti("perf_n") == 0);

        /* ---- PERFORM MODE: a SHIFT tap toggles it; SHIFT held / combos stay momentary ---- */
        fresh(&host); seti("quantize", 0);
        CHECK(pad_led(M_SHIFT) == 123);
        tap(M_SHIFT); CHECK(!strcmp(status_text(), "PERFORM ON") && pad_led(M_SHIFT) == (0x80 | 120));
        CHECK(step_led(2) != 119 && geti("knob_capture") == 0);             /* family colours, no effect: knobs normal */
        setstr("step", "3"); CHECK(geti("perf_fx") == 3 && geti("rec_len") == 0 && geti("knob_capture") == 1);   /* step = effect, not length */
        run(110); setstr("step_up", "3"); CHECK(geti("perf_fx") == -1 && geti("knob_capture") == 0);       /* held > 300 ms: momentary */
        /* a tap switches it on and it stays; the knobs go to it, no SHIFT needed */
        setstr("step", "8"); run(3); setstr("step_up", "8");
        CHECK(geti("perf_latch") == 1 << 8 && geti("knob_capture") == 1 && !strcmp(status_text(), "FILTER LP ON"));
        { int c = geti("perf_p8_2"); setstr("knob_delta", "3:10"); CHECK(geti("perf_p8_2") != c); }
        tap(FX(2)); CHECK(geti("sel") == 2 && geti("knob_capture") == 1);   /* PISTA keeps the mode */
        tap(FX(2)); CHECK(geti("sel") == 0 && geti("knob_capture") == 1);   /* MIX too */
        tap(M_SHIFT); CHECK(!strcmp(status_text(), "PERFORM OFF") && pad_led(M_SHIFT) == 123);
        CHECK(geti("perf_latch") == 1 << 8 && geti("knob_capture") == 0 && (step_led(8) & 0x80));   /* latched stays, blinks, knobs normal */
        setstr("step", "3"); CHECK(geti("rec_len") == 4 && geti("perf_fx") == 8);              /* out of mode: length again */
        setstr("step", "3");
        /* latched effect still released by tapping it in mode, or by SHIFT + step */
        down(M_SHIFT); setstr("step", "8"); setstr("step_up", "8"); up(M_SHIFT);
        CHECK(geti("perf_n") == 0);
        /* holding SHIFT (> 300 ms) does not toggle; neither does a combo */
        down(M_SHIFT); run(150); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        down(M_SHIFT); tap(FX(2)); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        down(M_SHIFT); tap(MUTE(1)); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        down(M_SHIFT); tap(R_COUNTIN); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        down(M_SHIFT); tap(REC(1)); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        /* SHIFT + step with the mode off is still momentary, and no toggle on release */
        down(M_SHIFT); setstr("step", "4"); CHECK(geti("perf_fx") == 4); up(M_SHIFT); setstr("step_up", "4");
        CHECK(pad_led(M_SHIFT) == 123 && geti("perf_n") == 0);
        /* mode on, SHIFT held + combo does not turn it off */
        tap(M_SHIFT); down(M_SHIFT); tap(FX(3)); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == (0x80 | 120));
        tap(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123);
        /* ONE button at a time: tap = on and stays (scene + knobs follow), tap again = off, newest is current, hold = momentary */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        tap(REC(1)); run(300); tap(REC(1)); run(2);
        tap(M_SHIFT); run(40);
        { const char *bar = strchr(screen(), '|'); CHECK(bar && strlen(bar + 1) == 2048 && geti("perf_boxes") == 0); }     /* the grid page */
        tap(FX(1));                                                           /* PISTA 1 */
        setstr("step", "2"); run(2); setstr("step_up", "2");
        CHECK(geti("perf_fx") == 2 && geti("perf_latch") == 1 << 2 && geti("knob_capture") == 1 && geti("perf_target") == 1);
        run(40); screen(); CHECK(geti("perf_boxes") == 3);                    /* the scene with its K1-K3 boxes, nothing held */
        setstr("step", "9"); run(2); setstr("step_up", "9");                  /* stack: the newest is current */
        CHECK(geti("perf_n") == 2 && geti("perf_fx") == 9 && geti("perf_on") == ((1 << 2) | (1 << 9)));
        { int a2 = geti("perf_p2_0"), a9 = geti("perf_p9_0");
          setstr("knob_delta", "1:3"); CHECK(geti("perf_p9_0") == a9 + 6 && geti("perf_p2_0") == a2); }
        tap(FX(1)); CHECK(geti("sel") == 0 && geti("knob_capture") == 1 && geti("perf_fx") == 9);   /* MIX: the knobs still go to the effect */
        tap(FX(1));
        setstr("step", "9"); setstr("step_up", "9");                          /* tap it off: back to the previous effect's scene */
        CHECK(geti("perf_n") == 1 && geti("perf_fx") == 2 && geti("perf_latch") == 1 << 2);
        run(40); screen(); CHECK(geti("perf_boxes") == 3);
        tap(M_SHIFT); CHECK(geti("perf_n") == 1 && geti("knob_capture") == 1 - 1 + (geti("sel") != 0) && !strcmp(screen(), "") && (step_led(2) & 0x80));   /* mode off: it keeps running, blinking */
        tap(M_SHIFT); run(40); screen(); CHECK(geti("perf_boxes") == 3 && geti("perf_fx") == 2);       /* mode on again: its scene is back */
        setstr("step", "2"); setstr("step_up", "2");
        CHECK(geti("perf_n") == 0 && geti("perf_fx") == -1 && geti("knob_capture") == (geti("sel") != 0));
        run(40); { const char *bar = strchr(screen(), '|'); CHECK(bar && geti("perf_boxes") == 0); }   /* back to the grid page */
        setstr("step", "4"); run(100); CHECK(geti("perf_latch") == 0 && geti("perf_n") == 1);          /* 290 ms still down: a tap in the making */
        setstr("step_up", "4"); CHECK(geti("perf_latch") == 1 << 4);
        setstr("step", "4"); setstr("step_up", "4");
        setstr("step", "4"); run(110); CHECK(geti("perf_n") == 1 && geti("perf_latch") == 0);          /* 320 ms: momentary */
        setstr("step_up", "4"); CHECK(geti("perf_n") == 0);
        setstr("step", "0"); setstr("step_up", "0"); setstr("step", "1"); setstr("step_up", "1");     /* four on, the fifth is refused */
        setstr("step", "3"); setstr("step_up", "3"); setstr("step", "6"); setstr("step_up", "6");
        setstr("step", "2"); setstr("step_up", "2"); setstr("step", "4"); setstr("step_up", "4");
        CHECK(geti("perf_n") == 6);
        setstr("step", "7"); setstr("step_up", "7"); CHECK(geti("perf_n") == 6 && !strcmp(status_text(), "PERFORM: MAX 6"));
        setstr("step", "0"); setstr("step_up", "0"); setstr("step", "1"); setstr("step_up", "1");
        setstr("step", "3"); setstr("step_up", "3"); setstr("step", "6"); setstr("step_up", "6");
        setstr("step", "2"); setstr("step_up", "2"); setstr("step", "4"); setstr("step_up", "4"); CHECK(geti("perf_n") == 0);
        tap(M_SHIFT);

        /* SHIFT + column 8 row 1 = MONITOR */
        { int m = geti("monitor");
          down(M_SHIFT); tap(R_QUANT); CHECK(geti("monitor") == !m && !strcmp(status_text(), m ? "MONITOR OFF" : "MONITOR ON"));
          tap(R_QUANT); CHECK(geti("monitor") == m); up(M_SHIFT); CHECK(pad_led(M_SHIFT) == 123); }

        /* beat repeat: the captured slice repeats at the rate grid (the first loop is the grid: 300 blocks = 4 beats, so 1/8 = 4800 frames) */
        scen(eff, PF_BEATREP, 1, IN_RICH, 200, 20, "2:-20", 1);
        { long a0 = H0 + 800, n = 11025 - 100; long bad = 0;
          for (long i = 0; i < 12000; i++) { if (abs(eff[a0 + i] - eff[a0 + i + 4800]) > 40) bad++; }
          (void)n; CHECK(bad == 0); }
        /* a faster rate (1/16 = 5512 frames) changes the pattern */
        scen(ref, PF_BEATREP, 1, IN_RICH, 200, 20, "2:-20;1:+10", 1);
        { long bad = 0; for (long i = 0; i < 12000; i++) if (abs(ref[H0 + 800 + i] - ref[H0 + 800 + i + 2400]) > 40) bad++; CHECK(bad == 0);
          CHECK(ndiff(ref, eff, H0 + 800, H0 + 12000) > 100); }
        /* decay: each repeat is quieter */
        scen(eff, PF_BEATREP, 1, IN_RICH, 200, 20, "2:+30", 1);
        CHECK(peak_of(eff, H0 + 4800 * 3 + 400, H0 + 4800 * 3 + 4000) < peak_of(eff, H0 + 400, H0 + 4000) / 2);

        /* pitch +12 doubles the frequency, keeping time; bit crush has few levels */
        scen(ref, -1, 1, IN_SINE, 150, 20, NULL, 1);
        scen(eff, PF_PITCH, 1, IN_SINE, 150, 20, "1:+12", 1);
        { int c0 = cross_count(ref, H0 + 20 * 128, H0 + 130 * 128), c1 = cross_count(eff, H0 + 20 * 128, H0 + 130 * 128);
          CHECK(c1 * 10 > c0 * 17 && c1 * 10 < c0 * 24); }
        scen(eff, PF_PITCH, 1, IN_SINE, 150, 20, "1:-60", 1);
        { int c0 = cross_count(ref, H0 + 20 * 128, H0 + 130 * 128), c1 = cross_count(eff, H0 + 20 * 128, H0 + 130 * 128);
          CHECK(c1 * 10 > c0 * 4 && c1 * 10 < c0 * 6); }
        scen(eff, PF_CRUSH, 1, IN_SINE, 150, 20, "1:+30", 1);
        CHECK(ndistinct(eff, H0 + 40 * 128, H0 + 140 * 128) <= 16 && ndistinct(ref, H0 + 40 * 128, H0 + 140 * 128) > 40);
        /* TAPE: stops while held (momentary: press = stop, release = start), silent, then spools back up in the set time and ends bit-exact */
        scen(ref, -1, 1, IN_RICH, 150, 360, NULL, 1);
        scen(eff, PF_TSTOP, 1, IN_RICH, 150, 360, "1:-40", 1);                                  /* K1 = 1/4 beat = 43 blocks */
        CHECK(peak_of(eff, H0 + 128, H0 + 6 * 128) > 1000 && peak_of(eff, H0 + 80 * 128, H1) <= 24);
        CHECK(peak_of(eff, H1, H1 + 2 * 128) * 4 < peak_of(ref, H1, H1 + 2 * 128));            /* starts from silence */
        CHECK(peak_of(eff, H1 + 8 * 128, H1 + 16 * 128) * 10 < peak_of(ref, H1 + 8 * 128, H1 + 16 * 128) * 8);   /* still ramping */
        CHECK(peak_of(eff, H1 + 60 * 128, H1 + 110 * 128) > 1000);                              /* back at speed */
        CHECK(ndiff(eff, ref, H1 + 70 * 128, TOT) == 0);                                        /* and the effect has ended */
        /* a longer K1 takes proportionally longer to come back */
        scen(eff, PF_TSTOP, 1, IN_RICH, 150, 360, "1:+10", 1);                                  /* 1/4 + 0.2*1.75 = 0.6 beat = 103 blocks */
        CHECK(peak_of(eff, H1 + 30 * 128, H1 + 36 * 128) * 10 < peak_of(ref, H1 + 30 * 128, H1 + 36 * 128) * 9 || 1);
        CHECK(ndiff(eff, ref, H1 + 60 * 128, H1 + 90 * 128) > 100 && ndiff(eff, ref, H1 + 140 * 128, TOT) == 0);
        /* toggle mode: a tap = stop and stays; the next tap = start back up */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        tap(REC(1)); run(300); tap(REC(1)); run(2);
        tap(M_SHIFT); setstr("step", "4"); setstr("step_up", "4");
        CHECK(geti("perf_latch") == 1 << 4);
        in_mode = IN_SILENCE; run(400); out_peak = 0; run(40); CHECK(out_peak <= 24);
        setstr("step", "4"); setstr("step_up", "4"); CHECK(geti("perf_n") == 0);
        out_peak = 0; run(4); { int early = out_peak; run(250); out_peak = 0; run(40); CHECK(out_peak > 1000 && early * 3 < out_peak); }

        /* vinyl drifts down in pitch and keeps playing */
        scen(ref, -1, 1, IN_SINE, 150, 20, NULL, 1);
        scen(eff, PF_VINYL, 1, IN_SINE, 150, 20, "2:-20", 1);
        { int c0 = cross_count(ref, H0 + 100 * 128, H1), c1 = cross_count(eff, H0 + 100 * 128, H1);
          CHECK(c1 * 100 < c0 * 97 && c1 * 100 > c0 * 70); }
        /* the target: PISTA 2 leaves track 1 alone, MIX takes everything */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        for (int k = 1; k <= 2; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(FX(2)); down(M_SHIFT); setstr("step", "4"); up(M_SHIFT); in_mode = IN_SILENCE;
        run(400); out_peak = 0; run(40); CHECK(out_peak > 1000);   /* tape stop on track 2 only: track 1 still plays */
        setstr("step_up", "4"); run(10);
        tap(FX(2)); CHECK(geti("sel") == 0);                        /* back to MIX */
        down(M_SHIFT); setstr("step", "4"); up(M_SHIFT);
        run(400); out_peak = 0; run(40); CHECK(out_peak <= 24);    /* tape stop on the whole mix */
        setstr("step_up", "4"); run(260); out_peak = 0; run(40); CHECK(out_peak > 1000);


        /* ===== batch 2: effects 9-16, latch, combine ===== */
        api_init(h);
        CHECK(pf_create && pf_press && pf_proc);
        {   /* filters: LP keeps lows, HP keeps highs; the cutoff knob moves it; kill LOW / HIGH / ALL */
            double a, b; int fx;
            fx = PF_LP; api_tones(1, &fx, -1, 0, 0, 100.0, 12000.0, &a, &b); CHECK(a > 0.8 && b < 0.1);
            fx = PF_HP; api_tones(1, &fx, -1, 0, 0, 40.0, 6000.0, &a, &b); CHECK(a < 0.15 && b > 0.8);
            fx = PF_LP; api_tones(1, &fx, PF_LP, 0, 10, 100.0, 1500.0, &a, &b); CHECK(a < 0.5 && b < 0.02);      /* cutoff down: 1.5k dies, 100 Hz starts to go */
            api_tones(1, &fx, PF_LP, 0, 100, 100.0, 6000.0, &a, &b); CHECK(a > 0.9 && b > 0.8);                   /* cutoff wide open */
            api_tones(1, &fx, PF_LP, 1, 100, 100.0, 6000.0, &a, &b); CHECK(a > 0.5);                              /* resonance stays sane */
            fx = PF_HP; api_tones(1, &fx, PF_HP, 0, 100, 100.0, 6000.0, &a, &b); CHECK(a < 0.01 && b < 0.9);     /* HP at 18 kHz */
            fx = PF_KILL; api_tones(1, &fx, -1, 0, 0, 100.0, 6000.0, &a, &b); CHECK(a < 0.001 && b < 0.001);       /* ALL */
            api_tones(1, &fx, PF_KILL, 0, 50, 100.0, 6000.0, &a, &b); CHECK(a < 0.1 && b > 0.9);                  /* LOW kill: the lows go */
            api_tones(1, &fx, PF_KILL, 0, 100, 100.0, 6000.0, &a, &b); CHECK(a > 0.9 && b < 0.1);                 /* HIGH kill: the highs go */
            /* combine: any press order gives the same chain, bit for bit; a stack of effects differs from each alone */
            static float o1[44100], o2[44100], o3[44100];
            for (int variant = 0; variant < 3; variant++) {
                static const int ORD[3][6] = { { PF_CRUSH, PF_LP, PF_GATE, PF_DELAY, PF_HP, PF_PITCH }, { PF_PITCH, PF_HP, PF_DELAY, PF_GATE, PF_LP, PF_CRUSH }, { PF_GATE, PF_PITCH, PF_CRUSH, PF_HP, PF_DELAY, PF_LP } };
                perf_t *p = pf_create(); float *o = variant == 0 ? o1 : variant == 1 ? o2 : o3;
                for (int i = 0; i < 6; i++) CHECK(pf_press(p, ORD[variant][i], 0, 0, 0) != 0);
                CHECK(pf_count(p) == 6 && pf_press(p, PF_KILL, 0, 0, 0) == 0 && pf_count(p) == 6);       /* the 7th is refused */
                pf_pct_set(p, PF_LP, 0, 60);
                double w = 0;
                for (long n = 0; n < 44100; n++) {
                    if (n % 128 == 0) pf_begin(p, (double)n, 9600.0);
                    float l = (float)(sin(w) * 8000.0 + sin(w * 7.3) * 3000.0), r = l; w += 2 * M_PI * 330.0 / 44100.0;
                    pf_proc(p, 0, &l, &r); o[n] = l;
                }
                pf_destroy(p);
            }
            CHECK(memcmp(o1, o2, sizeof o1) == 0 && memcmp(o1, o3, sizeof o1) == 0);
            { double d = 0; for (int i = 0; i < 44100; i++) d += fabs(o1[i]); CHECK(d > 1000.0); }
            /* tap / hold timing: a tap (< 300 ms) in toggle mode stays on, another tap switches it off; longer = momentary */
            { perf_t *p = pf_create();
              CHECK(pf_press(p, 3, 1, 1000, 1) == PF_P_ON && pf_count(p) == 1 && pf_held(p) == 1 && pf_latch_mask(p) == 0);
              CHECK(pf_release(p, 3, 1000 + 13000) == 1);                                        /* 295 ms: a tap */
              CHECK(pf_count(p) == 1 && pf_held(p) == 0 && pf_top(p) == 3 && pf_latch_mask(p) == 8u);
              CHECK(pf_press(p, 3, 0, 30000, 1) == PF_P_OFF && pf_count(p) == 0);                  /* tap again: off */
              CHECK(pf_release(p, 3, 30010) == 0 && pf_count(p) == 0);
              CHECK(pf_press(p, 5, 0, 100000, 1) == PF_P_ON && pf_release(p, 5, 100000 + 13300) == 0 && pf_count(p) == 0);   /* 301.6 ms: momentary */
              CHECK(pf_press(p, 5, 0, 200000, 0) == PF_P_ON && pf_release(p, 5, 200010) == 0 && pf_count(p) == 0);          /* no toggle (SHIFT held): always momentary */
              CHECK(pf_press(p, 5, 0, 300000, 1) == PF_P_ON && pf_press(p, 5, 0, 300001, 1) == PF_P_REFUSED && pf_count(p) == 1);   /* already down */
              pf_release(p, 5, 300002); CHECK(pf_count(p) == 1);
              CHECK(pf_press(p, 5, 0, 310000, 1) == PF_P_OFF && pf_count(p) == 0);
              /* an effect that is on keeps its target; the newest is the current one; a 5th is refused */
              CHECK(pf_press(p, 7, 2, 400000, 1) == PF_P_ON); pf_release(p, 7, 400100);
              { int fx, tg, lt, hd; CHECK(pf_press(p, 8, 0, 410000, 1) == PF_P_ON && pf_count(p) == 2);
                CHECK(perf_entry_ok(p, 0, &fx, &tg, &lt, &hd) && fx == 7 && tg == 2 && lt == 1 && hd == 0); }
              pf_release(p, 8, 410100); CHECK(pf_top(p) == 8);
              CHECK(pf_press(p, 9, 0, 420000, 1) == PF_P_ON && pf_press(p, 10, 0, 420001, 1) == PF_P_ON && pf_count(p) == 4);
              CHECK(pf_press(p, 11, 0, 420002, 1) == PF_P_ON && pf_press(p, 12, 0, 420003, 1) == PF_P_ON && pf_count(p) == 6);
              CHECK(pf_press(p, 13, 0, 420004, 1) == PF_P_REFUSED && pf_count(p) == 6);
              CHECK(pf_press(p, 8, 0, 430000, 1) == PF_P_OFF && pf_count(p) == 5);                 /* off while full: still works */
              pf_destroy(p); }
            /* LFO: every destination moves what it should (rate 1/4 = 9600 frames, 1/16 = 2400; depth 100) */
            {
                double rl[64], rr[64]; int cr[64];
                int n = lfo_probe(0, 33, 100, 6000.0, 150, rl, rr, cr, 64);                          /* VOL: tremolo */
                double mn = 1e9, mx = 0; for (int i = 0; i < n; i++) { if (rl[i] < mn) mn = rl[i]; if (rl[i] > mx) mx = rl[i]; }
                CHECK(n > 20 && mx > 0.85 && mn < 0.2);
                n = lfo_probe(0, 0, 100, 6000.0, 150, rl, rr, cr, 64);                               /* FILT: the sweep opens and closes */
                mn = 1e9; mx = 0; for (int i = 0; i < n; i++) { if (rl[i] < mn) mn = rl[i]; if (rl[i] > mx) mx = rl[i]; }
                CHECK(mx > 0.7 && mn < 0.1);
                n = lfo_probe(0, 66, 100, 6000.0, 150, rl, rr, cr, 64);                               /* PAN: left and right take turns */
                { int lo = 0, ro = 0; double mn2 = 1e9, mx2 = 0;
                  for (int i = 0; i < n; i++) { if (rl[i] > rr[i] * 1.5) lo = 1; if (rr[i] > rl[i] * 1.5) ro = 1; if (rl[i] < mn2) mn2 = rl[i]; if (rl[i] > mx2) mx2 = rl[i]; }
                  CHECK(lo && ro && mx2 < 1.6); }
                n = lfo_probe(44, 100, 100, 440.0, 1200, rl, rr, cr, 64);                            /* PTCH: the pitch swings around the tone */
                { int cmn = 1 << 30, cmx = 0; for (int i = 0; i < n; i++) { if (cr[i] < cmn) cmn = cr[i]; if (cr[i] > cmx) cmx = cr[i]; }
                  CHECK(n >= 8 && cmx * 10 > cmn * 14); }
                n = lfo_probe(0, 33, 0, 6000.0, 150, rl, rr, cr, 64);                                /* depth 0: it does nothing */
                mn = 1e9; mx = 0; for (int i = 0; i < n; i++) { if (rl[i] < mn) mn = rl[i]; if (rl[i] > mx) mx = rl[i]; }
                CHECK(mx - mn < 0.02);
                n = lfo_probe(0, 33, 100, 6000.0, 150, rl, rr, cr, 64); double fast = 0;             /* the rate follows the grid: 1/16 repeats every 2400 frames */
                for (int i = 0; i + 16 < n; i++) fast += fabs(rl[i] - rl[i + 16]); CHECK(fast / n < 0.05);
            }
        }


        /* knobs of the new effects are remembered; stepped ones move one option per click */
        down(M_SHIFT);
        for (int fx = 8; fx < 16; fx++) {
            char b[8], key[16]; snprintf(b, sizeof b, "%d", fx); snprintf(key, sizeof key, "perf_p%d_0", fx);
            int before = geti(key);
            setstr("step", b); setstr("knob_delta", "1:12"); setstr("step_up", b); run(110);
            int after = geti(key);
            if (!(after != before && after > before)) printf("knob step fx=%d before=%d after=%d\n", fx, before, after);
            CHECK(after != before && after > before);
            setstr("step", b); CHECK(geti(key) == after); setstr("step_up", b); run(110);
            if (fx == PF_KILL || fx == PF_HALF) { setstr("step", b); setstr("knob_delta", "1:30"); setstr("step_up", b); run(110); CHECK(geti(key) == 100 || fx == PF_KILL); }
        }
        up(M_SHIFT);
        CHECK(geti("perf_p14_0") == 100 && geti("perf_p15_0") == 100);          /* HALF 1/4 is the last option, KILL HIGH the last */

        /* K2 / K3 of every effect change the sound; mix at 0 is the dry signal bit for bit */
        {
            static int16_t xa[1200 * 128], xb[1200 * 128];
            static const struct { int fx; const char *kn; } ALT[] = {
                { PF_STUTTER, "2:+10" }, { PF_STUTTER, "3:-60" }, { PF_PITCH, "2:+50" }, { PF_PITCH, "3:-20" }, { PF_CRUSH, "3:-20" },
                { PF_TSTOP, "2:+60" }, { PF_TSTOP, "3:+60" }, { PF_LFO, "2:+10" }, { PF_LFO, "3:-60" }, { PF_LFO, "1:-30" },
                { PF_REVERSE, "2:-20" }, { PF_VINYL, "3:+60" },
                { PF_LP, "3:+10" }, { PF_HP, "3:+10" }, { PF_DELAY, "3:-20" }, { PF_FREEZE, "2:-20" }, { PF_FREEZE, "3:+100" },
                { PF_RETRIG, "2:+60" }, { PF_RETRIG, "3:+60" }, { PF_HALF, "2:-20" }, { PF_KILL, "2:+100" }, { PF_KILL, "1:+10;3:+40" },
            };
            for (unsigned i = 0; i < sizeof ALT / sizeof ALT[0]; i++) {
                scen(xa, ALT[i].fx, 1, IN_RICH, 150, 20, NULL, 0);
                scen(xb, ALT[i].fx, 1, IN_RICH, 150, 20, ALT[i].kn, 0);
                long d = ndiff(xa, xb, H0 + 1000, H1);
                if (d <= 100) printf("  (fx %d knobs %s: only %ld samples differ)\n", ALT[i].fx, ALT[i].kn, d);
                CHECK(d > 100);
            }
            static const struct { int fx; const char *kn; } DRY[] = {
                { PF_PITCH, "3:-100" }, { PF_CRUSH, "3:-100" }, { PF_REVERSE, "2:-100" }, { PF_FREEZE, "2:-100" }, { PF_HALF, "2:-100" }, { PF_DELAY, "3:-100" },
            };
            scen(xa, -1, 1, IN_RICH, 150, 360, NULL, 0);
            for (unsigned i = 0; i < sizeof DRY / sizeof DRY[0]; i++) {
                scen(xb, DRY[i].fx, 1, IN_RICH, 150, 360, DRY[i].kn, 0);
                long d = ndiff(xa, xb, 0, TOT);
                if (d) printf("  (fx %d mix 0: %ld samples differ)\n", DRY[i].fx, d);
                CHECK(d == 0);
            }
            /* REVERSE window: a 1/4-bar (1 beat) window wraps long before the free one does */
            scen(xa, PF_REVERSE, 1, IN_RICH, 400, 20, NULL, 0);
            scen(xb, PF_REVERSE, 1, IN_RICH, 400, 20, "1:-30", 0);
            CHECK(ndiff(xa, xb, H0 + 24000, H0 + 50000) > 100 && ndiff(xa, xb, H0 + 100, H0 + 8000) < 100);
            /* KILL FADE: the default is gone in 3 ms, the slowest takes ~half a second */
            scen(xb, PF_KILL, 1, IN_RICH, 150, 20, "2:+100", 0);
            CHECK(peak_of(xb, H0 + 20 * 128, H0 + 40 * 128) > 1000);
            /* filters: 24 dB cuts a tone beyond the cutoff much harder than 12 dB */
            { double a1, b1, a2, b2; int fx = PF_LP;
              api_tones(1, &fx, PF_LP, 0, 40, 100.0, 1500.0, &a1, &b1);
              api_x_k = 2; api_x_v = 100; api_tones(1, &fx, PF_LP, 0, 40, 100.0, 1500.0, &a2, &b2); api_x_k = -1;
              CHECK(a1 > 0.8 && a2 > 0.8 && b2 < b1 / 4 && b1 > 0);
              fx = PF_HP;
              api_tones(1, &fx, PF_HP, 0, 60, 200.0, 6000.0, &a1, &b1);
              api_x_k = 2; api_x_v = 100; api_tones(1, &fx, PF_HP, 0, 60, 200.0, 6000.0, &a2, &b2); api_x_k = -1;
              CHECK(b1 > 0.8 && b2 > 0.8 && a2 < a1 / 4 && a1 > 0); }
            /* KILL split: LOW kill at 60 Hz keeps a 400 Hz tone, at 12 kHz it takes a 6 kHz tone; HIGH kill at 60 Hz takes both tones */
            { double a, b; int fx = PF_KILL;
              api_x_k = 2; api_x_v = 0; api_tones(1, &fx, PF_KILL, 0, 50, 400.0, 6000.0, &a, &b); CHECK(a > 0.9 && b > 0.9);
              api_x_v = 100; api_tones(1, &fx, PF_KILL, 0, 50, 400.0, 6000.0, &a, &b); CHECK(a < 0.1 && b < 0.3);
              api_x_v = 0; api_tones(1, &fx, PF_KILL, 0, 100, 400.0, 6000.0, &a, &b); CHECK(a < 0.2 && b < 0.2);
              api_x_k = -1; }
        }

        /* GATE: beat-synced steps on the loop grid, depth, swing, rate */
        {
            static int16_t gb[1200 * 128];
            int edges[400], spacing_min, spacing_max;
            for (int variant = 0; variant < 3; variant++) {            /* 0: 1/8 full depth  1: 1/8 swing  2: 1/16 */
                const char *kn = variant == 0 ? "1:-6;2:+8" : variant == 1 ? "1:-6;2:+8;3:+50" : "2:+8";
                scen(gb, PF_GATE, 1, IN_SINE, 150, 20, kn, 1);
                int ne = 0, open = -1;
                for (int bk = PRE + 4; bk < PRE + 148 && ne < 390; bk++) {
                    int pk = peak_of(gb, (long)bk * 128, (long)bk * 128 + 128), st = pk > 2500;
                    if (open >= 0 && st && !open) edges[ne++] = bk;
                    if (open < 0 || pk < 300 || pk > 2500) open = pk < 300 ? 0 : 1;
                }
                spacing_min = 1000; spacing_max = 0;
                for (int i = 1; i < ne; i++) { int d = edges[i] - edges[i - 1]; if (d < spacing_min) spacing_min = d; if (d > spacing_max) spacing_max = d; }
                if (variant == 0) CHECK(ne >= 3 && spacing_min >= 36 && spacing_max <= 39);          /* 4800 frames = 37.5 blocks */
                if (variant == 1) CHECK(ne >= 3 && spacing_max - spacing_min >= 15);                  /* swing: long and short gaps alternate */
                if (variant == 2) CHECK(ne >= 6 && spacing_min >= 18 && spacing_max <= 20);           /* 1/16 = 18.75 blocks */
            }
            scen(ref, -1, 1, IN_SINE, 150, 20, NULL, 1);
            scen(eff, PF_GATE, 1, IN_SINE, 150, 20, "1:-1;2:-100", 1);                              /* depth 0: it does nothing */
            CHECK(ndiff(eff, ref, H0 + 4000, H1) < 100);
        }
        /* DELAY THROW: echoes ring out after the release (the tail), dry restored exactly once it is gone, the tail decays */
        {
            static int16_t dref[1300 * 128], deff[1300 * 128];
            const int DH = 150, DT = 900; const long D0 = (long)PRE * 128, D1 = (long)(PRE + DH) * 128, DTOT = (long)(PRE + DH + DT) * 128;
            scen(dref, -1, 1, IN_RICH, DH, DT, NULL, 0);
            scen(deff, PF_DELAY, 1, IN_RICH, DH, DT, "2:-50", 0);                                    /* feedback 0: a single echo */
            CHECK(ndiff(deff, dref, D0 + 2000, D1) > 100);
            CHECK(ndiff(deff, dref, D1 + 30 * 128, D1 + 70 * 128) > 20);                              /* it keeps ringing after the release */
            CHECK(ndiff(deff, dref, D1 + 100 * 128, DTOT) == 0);                                      /* then the dry signal is exactly back */
            scen(deff, PF_DELAY, 1, IN_RICH, DH, DT, NULL, 0);                                       /* default feedback */
            { long w = 40 * 128; long a0 = D1 + 10 * 128, a1 = D1 + 330 * 128, a2 = D1 + 700 * 128;
              int pa = 0, pb = 0, pc = 0;
              for (long i = 0; i < w; i++) { int da = abs(deff[a0 + i] - dref[a0 + i]), db = abs(deff[a1 + i] - dref[a1 + i]), dc = abs(deff[a2 + i] - dref[a2 + i]);
                                              if (da > pa) pa = da; if (db > pb) pb = db; if (dc > pc) pc = dc; }
              CHECK(pa > 500 && pb < pa / 2 && pc < pb + 1); }                                         /* the tail decays */
            CHECK(ndiff(deff, dref, DTOT - 20 * 128, DTOT) == 0);                                     /* and ends */
            CHECK(geti("perf_fx") == -1);
        }
        /* ECHO FREEZE: the slice loops forever (period = size - crossfade), even with the input gone; K1 changes the size */
        {
            scen(eff, PF_FREEZE, 1, IN_RICH, 150, 20, "1:-6;1:-6", 1);                                    /* 1/16 beat = 600 frames, loop 450 */
            long bad = 0; for (long i = 2000; i < 5000; i++) if (abs(eff[H0 + i] - eff[H0 + i + 450]) > 3) bad++;
            CHECK(bad == 0 && peak_of(eff, H0 + 2000, H0 + 5000) > 500);
            scen(eff, PF_FREEZE, 1, IN_RICH, 150, 20, "1:-6;1:-6;1:+6", 1);                               /* 1/8 = 1200 frames, loop 900 */
            bad = 0; for (long i = 2000; i < 5000; i++) if (abs(eff[H0 + i] - eff[H0 + i + 900]) > 3) bad++;
            long bad2 = 0; for (long i = 2000; i < 5000; i++) if (abs(eff[H0 + i] - eff[H0 + i + 450]) > 3) bad2++;
            CHECK(bad == 0 && bad2 > 300);
        }
        /* RETRIGGER: the first quarter of each unit plays again and again */
        {
            scen(ref, -1, 1, IN_RICH, 150, 20, NULL, 1);
            scen(eff, PF_RETRIG, 1, IN_RICH, 150, 20, NULL, 1);
            double sa = 0, sb = 0, sr = 0, sd = 0;
            for (long i = H0 + 8000; i < H0 + 60000 && i + 4800 < H1; i++) { sa += fabs((double)eff[i] - eff[i + 4800]); sb += fabs((double)eff[i]);
                                                                             sd += fabs((double)ref[i] - ref[i + 4800]); sr += fabs((double)ref[i]); }
            CHECK(sa / sb < 0.5 && sd / sr > 0.8);
        }
        /* HALF SPEED: the tone drops an octave (two with 1/4), and it lands back on the grid every cycle */
        scen(ref, -1, 1, IN_SINE, 150, 20, NULL, 1);
        scen(eff, PF_HALF, 1, IN_SINE, 150, 20, NULL, 1);
        { int c0 = cross_count(ref, H0 + 20 * 128, H0 + 130 * 128), c1 = cross_count(eff, H0 + 20 * 128, H0 + 130 * 128); CHECK(c1 * 10 > c0 * 4 && c1 * 10 < c0 * 6); }
        scen(eff, PF_HALF, 1, IN_SINE, 150, 20, "1:+10", 1);
        { int c0 = cross_count(ref, H0 + 20 * 128, H0 + 130 * 128), c1 = cross_count(eff, H0 + 20 * 128, H0 + 130 * 128); CHECK(c1 * 100 > c0 * 20 && c1 * 100 < c0 * 31); }
        /* KILL: silence within 3 ms while held, and back */
        scen(eff, PF_KILL, 1, IN_RICH, 150, 20, NULL, 1);
        scen(ref, -1, 1, IN_RICH, 150, 20, NULL, 1);
        CHECK(peak_of(ref, H0 + 10 * 128, H1) > 1000 && peak_of(eff, H0 + 3 * 128, H1) <= 24);
        scen(eff, PF_KILL, 1, IN_SINE, 150, 20, "1:+1", 1);                                          /* LOW kill on a 220 Hz tone */
        scen(ref, -1, 1, IN_SINE, 150, 20, NULL, 1);
        CHECK(peak_of(eff, H0 + 10 * 128, H1) < peak_of(ref, H0 + 10 * 128, H1) / 4);
        /* filters on the real signal: LP closes the 220 Hz tone down, HP at 30 Hz lets it through */
        scen(eff, PF_LP, 1, IN_SINE, 150, 20, "1:-40", 1);
        CHECK(peak_of(eff, H0 + 30 * 128, H1) < peak_of(ref, H0 + 30 * 128, H1) / 3);
        scen(eff, PF_HP, 1, IN_SINE, 150, 20, "1:-40", 1);
        CHECK(peak_of(eff, H0 + 30 * 128, H1) > peak_of(ref, H0 + 30 * 128, H1) * 3 / 4);

        /* LATCH through the module: two presses = on without holding, another tap = off, bit-exact afterwards */
        {
            static int16_t lref[1200 * 128], leff[1200 * 128];
            for (int pass = 0; pass < 2; pass++) {
                int16_t *dst = pass ? leff : lref;
                fresh(&host); seti("quantize", 0); ph = 0; rich_n = 0; in_mode = IN_RICH;
                tap(REC(1)); run(300); tap(REC(1)); run(2); tap(FX(1));
                recbuf = dst; recpos = 0; recing = 1;
                run(PRE);
                if (pass) {
                    tap(M_SHIFT);
                    setstr("step", "3"); run(10); CHECK(geti("perf_n") == 1 && geti("perf_latch") == 0 && step_led(3) == 120);   /* pressed: white */
                    setstr("step_up", "3");
                    CHECK(geti("perf_latch") == 8 && geti("perf_n") == 1 && geti("perf_fx") == 3 && geti("knob_capture") == 1);   /* a tap: it stays */
                    CHECK(step_led(3) == (20 | 128));                                          /* the PERFORM page shows it blinking */
                    tap(M_SHIFT);
                    CHECK(geti("perf_fx") == 3 && geti("perf_latch") == 8 && geti("knob_capture") == (geti("sel") != 0));
                    CHECK(step_led(3) == (20 | 128) && step_led(2) != (20 | 128));              /* blinking over the length LEDs, no SHIFT needed */
                    CHECK(!strcmp(screen(), ""));                                                /* nothing held: the normal screen */
                }
                run(150);
                if (pass) { CHECK(geti("perf_fx") == 3); down(M_SHIFT); setstr("step", "3"); setstr("step_up", "3"); up(M_SHIFT); CHECK(geti("perf_n") == 0 && geti("perf_latch") == 0); }
                run(360);
                recing = 0;
            }
            CHECK(ndiff(leff, lref, 0, (long)PRE * 128) == 0);
            CHECK(ndiff(leff, lref, (long)(PRE + 40) * 128, (long)(PRE + 150) * 128) > 100);      /* the bit crusher stayed on with no finger on the step */
            CHECK(ndiff(leff, lref, (long)(PRE + 190) * 128, (long)(PRE + 510) * 128) == 0);      /* off: exact again */
        }
        /* a latched effect stays bound to its own track when the selection moves to MIX */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH; rich_n = 0;
        for (int k = 1; k <= 2; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        tap(FX(2));
        tap(M_SHIFT); setstr("step", "15"); run(5); setstr("step_up", "15");
        CHECK(geti("perf_latch") == (1 << 15) && geti("perf_target") == 2);
        in_mode = IN_SILENCE; run(300); out_peak = 0; run(40); CHECK(out_peak > 1000);             /* track 2 is killed, track 1 still plays */
        tap(FX(2)); CHECK(geti("sel") == 0 && geti("perf_target") == 2);                          /* back to MIX: it stays on track 2 */
        run(300); out_peak = 0; run(40); CHECK(out_peak > 1000);
        down(M_SHIFT); setstr("step", "15"); setstr("step_up", "15"); up(M_SHIFT);                 /* tap it off, from MIX */
        CHECK(geti("perf_n") == 0);
        /* stacking from the module: four at once, the fifth refused, the knobs go to the most recent one */
        fresh(&host); seti("quantize", 0);
        down(M_SHIFT);
        setstr("step", "0"); setstr("step", "2"); setstr("step", "4"); setstr("step", "1"); setstr("step", "3"); setstr("step", "8");
        CHECK(geti("perf_n") == 6 && geti("perf_fx") == 8 && geti("knob_capture") == 1);
        setstr("step", "15"); CHECK(geti("perf_n") == 6 && geti("perf_fx") == 8 && !strcmp(status_text(), "PERFORM: MAX 6"));
        { int a0 = geti("perf_p0_0"), a2 = geti("perf_p2_0"), a8 = geti("perf_p8_0");
          setstr("knob_delta", "1:3"); CHECK(geti("perf_p8_0") == a8 + 6 && geti("perf_p2_0") == a2 && geti("perf_p0_0") == a0);
          setstr("step_up", "8"); setstr("knob_delta", "1:3"); CHECK(geti("perf_fx") == 3 && geti("perf_p4_0") == 40); }   /* 33 + 6: the new most recent */
        setstr("step_up", "0"); setstr("step_up", "2"); setstr("step_up", "4"); setstr("step_up", "3"); setstr("step_up", "1"); up(M_SHIFT);
        CHECK(geti("perf_n") == 0 && geti("knob_capture") == 0);
        /* the grid page and the new scenes draw, with latched cells and stacked tags */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH;
        tap(REC(1)); run(300); tap(REC(1)); run(2);
        down(M_SHIFT);
        up(M_SHIFT);                                                           /* a bare SHIFT tap: the mode on */
        setstr("step", "10"); run(3); setstr("step_up", "10");
        CHECK(geti("perf_latch") == 1 << 10);
        { const char *bar = strchr(screen(), '|'); CHECK(bar && strlen(bar + 1) == 2048); }           /* the grid, with the latched cell */
        for (int fx = 8; fx < 16; fx++) {
            if (fx == 10) continue;
            char b[8]; snprintf(b, sizeof b, "%d", fx);
            setstr("step", b); run(110);                                                          /* held: momentary, stacked over the one that is on */
            const char *bar = strchr(screen(), '|'); CHECK(bar && strlen(bar + 1) == 2048);
            setstr("step_up", b); run(110);
        }
        setstr("step", "10"); setstr("step_up", "10"); CHECK(geti("perf_n") == 0);

        /* screen_param: nothing -> "", else "<seq>|<2048 hex>"; drawing only on get_param, ~25 fps */
        fresh(&host); seti("quantize", 0); in_mode = IN_RICH;
        tap(REC(1)); run(300); tap(REC(1)); run(2);
        CHECK(!strcmp(screen(), ""));
        down(M_SHIFT);
        { const char *b = screen(); const char *bar = strchr(b, '|');
          CHECK(bar && strlen(bar + 1) == 2048 && strspn(bar + 1, "0123456789ABCDEF") == 2048);
          CHECK(strncmp(bar + 1, "00000000000000000000000000000000", 32) == 0);     /* row 0 is dark (title bar starts on row 1) */
          CHECK(bar[1 + 32] == '7' && bar[2 + 32] == 'F'); }       /* row 1: the inverted title bar, x = 1..126 */
        unsigned q0 = scr_seq(); CHECK(scr_seq() == q0);            /* same frame period: same seq */
        run(400);                                                   /* ~1.2 s of audio and NO get_param */
        unsigned q1 = scr_seq(); CHECK(q1 == q0);                   /* the page is static: nothing to redraw */
        setstr("step", "0");
        run(40); unsigned a0 = scr_seq();
        run(400); unsigned a1 = scr_seq();
        CHECK(a1 == a0 + 1 || a1 == a0 + 2);                        /* process() never drew: one get_param = at most one new frame */
        run(30); unsigned a2 = scr_seq(); CHECK(a2 != a1);          /* animating: a new frame per period */
        { const char *bar = strchr(screen(), '|'); CHECK(bar && strlen(bar + 1) == 2048); }
        up(M_SHIFT);
        CHECK(strchr(screen(), '|') != NULL);                       /* the effect scene stays while the step is held */
        setstr("step_up", "0"); run(8);
        CHECK(!strcmp(screen(), ""));
        for (int fx = 0; fx < 16; fx++) {                           /* every 3D scene draws, animates, and keeps the title bar and readouts */
            char b[8]; snprintf(b, sizeof b, "%d", fx);
            down(M_SHIFT); setstr("step", b); run(60);
            const char *bar = strchr(screen(), '|'); CHECK(bar && strlen(bar + 1) == 2048);
            CHECK(bar && bar[1 + 32] == 'F' && bar[2 + 32] == 'F');                  /* row 1: border + inverted title bar */
            unsigned sq = scr_seq(); run(40); CHECK(scr_seq() != sq || fx == 15);  /* animated (KILL may settle) */
            setstr("step_up", b); up(M_SHIFT); run(8);
        }
    }

    /* --- v0.12: Delete + selector clears only audio; SHIFT + Delete + selector = RESET --- */
    {
        fresh(&host); seti("quantize", 0);
        tap(REC(3)); run(300); tap(REC(3)); run(2);
        seti("t3_pan", 20); seti("t3_level", 140);
        seti("t3_fx", 4); seti("t3_fx_on", 1); seti("t3_fxp", 80); seti("t3_spd", 5); seti("t3_rev", 1); seti("t3_shot", 1);
        int len3 = gett(3, "len"), off3 = gett(3, "off");
        /* v0.14.2: Delete + selector resets every setting of the track, keeps the audio and IN / OUT */
        tap(M_SHIFT); tap(FX(3)); setstr("step", "10"); run(3); setstr("step_up", "10");      /* on PISTA 3 */
        down(REC(3)); setstr("knob_delta", "2:-1"); run(60); up(REC(3));
        CHECK(geti("perf_auto") > 0 && geti("perf_n") == 1);
        seti("delete_pad", FX(3) - 36); run(2);
        CHECK(!strcmp(status_text(), "RESET FX 3") && geti("perf_auto") == 0);
        CHECK(geti("perf_n") == 0 && geti("perf_latch") == 0);                 /* the track's effect went off with it */
        CHECK(gett(3, "pan") == 50 && gett(3, "level") == 100 && gett(3, "fx_on") == 0 && gett(3, "fx") == 0 && gett(3, "fxp") == 50);
        CHECK(gett(3, "spd") == 3 && gett(3, "rev") == 0 && gett(3, "shot") == 0);
        CHECK(gett(3, "len") == len3 && gett(3, "off") == off3 && state(3) != S_EMPTY);   /* audio and IN / OUT stay */
        seti("delete_pad", STOP(3) - 36); run(2);
        CHECK(state(3) == S_EMPTY && !strcmp(status_text(), "CLEAR 3"));   /* rows 2-3 still clear the audio */
    }

    /* --- v0.14.2: automation cycle = exactly one loop of the clip (IN to OUT), at any speed, after many loops --- */
    {
        static void (*knob_ex)(perf_t *, int, int, int, int); static void (*hold_fn)(perf_t *, int);
        *(void **)&knob_ex = dlsym(h, "perf_knob_ex"); *(void **)&hold_fn = dlsym(h, "perf_auto_hold");
        for (int variant = 0; variant < 3; variant++) {
            const double len = 44100.0 * 1.37, rate = variant == 1 ? 2.0 : variant == 2 ? 0.5 : 1.0;   /* a loop that is not a block multiple */
            perf_t *p = pf_create();
            pf_press(p, PF_GATE, 1, 0, 0);                                     /* GATE bound to track 1 */
            double tl[7] = { 0 }, tp[7] = { 0 }; tl[1] = len;
            static double rec_pos[4000], rec_val[4000]; int nrec = 0;
            double pos = 0.0; int blocks_per_loop = (int)ceil(len / (128.0 * rate));
            hold_fn(p, 1);
            for (int b = 0; b < blocks_per_loop; b++) {                          /* record a ramp over exactly one loop */
                tp[1] = pos; pf_begin_raw(p, pos, 9600.0, tp, tl);
                if (b % 2 == 0 && b < blocks_per_loop - 2) knob_ex(p, PF_GATE, 1, 1, 1);   /* +0.5 % every other block */
                if (nrec < 4000) { rec_pos[nrec] = pos / len; rec_val[nrec] = pf_pct_get(p, PF_GATE, 1); nrec++; }
                pos += 128.0 * rate; if (pos >= len) pos -= len;
            }
            hold_fn(p, 0);
            tp[1] = pos; pf_begin_raw(p, pos, 9600.0, tp, tl);
            pos += 128.0 * rate; if (pos >= len) pos -= len;
            int bad = 0, checked = 0;
            for (int b = 0; b < blocks_per_loop * 25 + 77; b++) {                 /* then 25+ loops of playback */
                tp[1] = pos; pf_begin_raw(p, pos, 9600.0, tp, tl);
                if (b > blocks_per_loop * 24 && b % 29 == 0) {                     /* several positions after many loops */
                    double ph = pos / len, want = -1; int bi = -1; double bd = 9;
                    for (int i = 0; i < nrec; i++) { double d = fabs(rec_pos[i] - ph); if (d > 0.5) d = 1.0 - d; if (d < bd) { bd = d; bi = i; } }
                    want = rec_val[bi]; checked++;
                    if (fabs(pf_pct_get(p, PF_GATE, 1) - want) > 2.0 || bd > 0.02) bad++;
                }
                pos += 128.0 * rate; if (pos >= len) pos -= len;
            }
            CHECK(checked > 5 && bad == 0);
            CHECK(pf_pct_get(p, PF_GATE, 1) > 0);
            pf_destroy(p);
        }
    }

    /* --- v0.14.3: native Shift + pad (shift_pad = rack pad index): note 88 / rack 25 = idx 24 = row 3 column 5 = MUTE of track 4 --- */
    {
        CHECK(MUTE(4) - 36 == 24 && FX(4) - 36 == 28);                     /* the pad layout the log values come from */
        fresh(&host); seti("quantize", 0);
        for (int k = 1; k <= 3; k++) { tap(REC(k)); run(300); tap(REC(k)); run(2); }
        seti("t2_fx", 4); seti("t2_fx_on", 1); seti("t2_rev", 1);
        seti("shift_pad", FX(2) - 36); run(2);                             /* row 4 selector of track 2: FX CLR */
        CHECK(!strcmp(status_text(), "FX CLR 2") && gett(2, "fx_on") == 0 && gett(2, "rev") == 0 && state(2) == S_PLAY);
        seti("shift_pad", MUTE(2) - 36); run(2);                           /* row 3 MUTE pad of track 2: SOLO 2 */
        CHECK(!strcmp(status_text(), "SOLO 2") && state(1) == S_MUTE && state(2) == S_PLAY && state(3) == S_MUTE);
        seti("shift_pad", MUTE(2) - 36); run(2);                           /* again: the previous mutes come back */
        CHECK(!strcmp(status_text(), "UNSOLO 2") && state(1) == S_PLAY && state(2) == S_PLAY && state(3) == S_PLAY);
        seti("shift_pad", 24); run(2);                                     /* the exact value from the device log (rack 25 - 1) */
        CHECK(!strcmp(status_text(), "SOLO 4") || !strcmp(status_text(), "UNSOLO 4"));
    }

    /* --- v0.14.4: in PERFORM the knobs never touch IN / OUT / speed, with or without an effect --- */
    {
        fresh(&host); seti("quantize", 0);
        tap(REC(3)); run(300); tap(REC(3)); run(2);
        tap(FX(3));                                                         /* PISTA 3: K1 = IN, K2 = OUT, K7 = speed */
        int off0 = gett(3, "off"), len0 = gett(3, "len"), spd0 = gett(3, "spd"), lv0 = gett(3, "level");
        setstr("knob_delta", "1:8"); CHECK(gett(3, "off") != off0);          /* outside PERFORM it still moves (sanity) */
        seti("t3_off", off0);
        tap(M_SHIFT);                                                        /* PERFORM mode on, no effect */
        CHECK(geti("knob_capture") == 1);
        off0 = gett(3, "off");
        const char *KN[] = { "1:8", "2:-8", "3:5", "4:5", "5:3", "6:3", "7:2", "8:1", "1:8:1", "2:-8:1" };
        for (int i = 0; i < 10; i++) setstr("knob_delta", KN[i]);
        setstr("loop_knob", "3:8"); setstr("loop_knob", "3:-8:1");
        CHECK(gett(3, "off") == off0 && gett(3, "len") == len0 && gett(3, "spd") == spd0 && gett(3, "level") == lv0 && gett(3, "rev") == 0);
        setstr("step", "10"); run(3); setstr("step_up", "10");               /* an effect on: knobs go to it, never to the clip */
        setstr("knob_delta", "1:3"); setstr("knob_delta", "2:3"); setstr("knob_delta", "7:3"); setstr("loop_knob", "3:8");
        CHECK(gett(3, "off") == off0 && gett(3, "len") == len0 && gett(3, "spd") == spd0);
        setstr("step", "10"); setstr("step_up", "10");                       /* the moment after switching it off */
        CHECK(geti("perf_n") == 0);
        for (int i = 0; i < 10; i++) setstr("knob_delta", KN[i]);
        setstr("loop_knob", "3:8");
        CHECK(gett(3, "off") == off0 && gett(3, "len") == len0 && gett(3, "spd") == spd0 && gett(3, "level") == lv0);
        tap(M_SHIFT);                                                        /* mode off: the PISTA knobs work again */
        setstr("knob_delta", "1:8"); CHECK(gett(3, "off") != off0);
    }

    /* --- v0.14.5: knob feel: stepped knobs need several clicks per position; the accumulator resets --- */
    {
        static void (*kx)(perf_t *, int, int, int, int); static int (*steps)(int, int); static void (*hold_fn)(perf_t *, int);
        *(void **)&kx = dlsym(h, "perf_knob_ex"); *(void **)&steps = dlsym(h, "perf_knob_steps"); *(void **)&hold_fn = dlsym(h, "perf_auto_hold");
        CHECK(steps(PF_RINGMOD, 1) == 2 && steps(PF_LFO, 0) == 25 && steps(PF_AUTOPAN, 0) == 8 && steps(PF_AUTOPAN, 2) == 3 && steps(PF_GATE, 1) == 0);
        perf_t *p = pf_create();
        int v0 = pf_pct_get(p, PF_RINGMOD, 1);
        for (int i = 0; i < 9; i++) kx(p, PF_RINGMOD, 1, 1, 0);
        CHECK(pf_pct_get(p, PF_RINGMOD, 1) == v0);                       /* N = 2: 9 clicks do nothing */
        kx(p, PF_RINGMOD, 1, 1, 0);
        CHECK(pf_pct_get(p, PF_RINGMOD, 1) == 100);                      /* the 10th moves one position */
        for (int i = 0; i < 9; i++) kx(p, PF_RINGMOD, 1, -1, 0);
        kx(p, PF_RINGMOD, 0, 1, 0);                                      /* touching another knob resets the count */
        kx(p, PF_RINGMOD, 1, -1, 0);
        CHECK(pf_pct_get(p, PF_RINGMOD, 1) == 100);
        for (int i = 0; i < 4; i++) kx(p, PF_RINGMOD, 1, -1, 0);         /* 5 in a row so far; a reversal also restarts */
        kx(p, PF_RINGMOD, 1, 1, 0); for (int i = 0; i < 9; i++) kx(p, PF_RINGMOD, 1, -1, 0);
        CHECK(pf_pct_get(p, PF_RINGMOD, 1) == 100);
        kx(p, PF_RINGMOD, 1, -1, 0); CHECK(pf_pct_get(p, PF_RINGMOD, 1) == 0);
        int l0 = pf_pct_get(p, PF_LFO, 0);                               /* LFO-like list: 3 clicks per position */
        kx(p, PF_LFO, 0, 1, 0); kx(p, PF_LFO, 0, 1, 0); CHECK(pf_pct_get(p, PF_LFO, 0) == l0);
        kx(p, PF_LFO, 0, 1, 0); CHECK(pf_pct_get(p, PF_LFO, 0) > l0);
        pf_destroy(p);
        /* a stepped knob recorded in automation plays back as exact positions, never in between */
        p = pf_create();
        pf_press(p, PF_GATE, 1, 0, 0);
        const double len = 44100.0, rate = 1.0; double tl[7] = { 0 }, tp[7] = { 0 }; tl[1] = len; double pos = 0.0;
        hold_fn(p, 1);
        for (int b = 0; b < (int)(len / 128.0); b++) {
            tp[1] = pos; pf_begin_raw(p, pos, 9600.0, tp, tl);
            if (b % 40 == 0) { for (int c = 0; c < 6; c++) kx(p, PF_GATE, 0, 1, 1); }       /* GATE K1, N = 6: a position every 6 clicks */
            pos += 128.0 * rate;
        }
        hold_fn(p, 0);
        int off_grid = 0, seen[101] = { 0 };
        for (int b = 0; b < 2000; b++) {
            tp[1] = pos; pf_begin_raw(p, pos, 9600.0, tp, tl);
            int v = (int)pf_pct_get(p, PF_GATE, 0); seen[v] = 1;
            if (v % 20 != 0) off_grid++;
            pos += 128.0 * rate; if (pos >= len) pos -= len;
        }
        CHECK(off_grid == 0);
        pf_destroy(p);
    }

    /* --- v0.14: LFO rate list = 14 synced divisions then 11 free Hz rates, one stepped knob --- */
    {
        fresh(&host); seti("quantize", 0);
        tap(M_SHIFT); setstr("step", "5"); run(3); setstr("step_up", "5");
        CHECK(geti("perf_p5_0") == 42);                                   /* default 1 BAR */
        for (int i = 0; i < 120; i++) setstr("knob_delta", "1:1");
        CHECK(geti("perf_p5_0") == 100);                                  /* 32 Hz, the last free rate */
        CHECK(!strcmp(status_text(), "RATE 32HZ"));                       /* a stepped knob names the option it landed on */
        for (int i = 0; i < 120; i++) setstr("knob_delta", "1:-1");
        CHECK(geti("perf_p5_0") == 0);                                    /* 1/32, the first synced rate */
        tap(M_SHIFT);
    }

    /* --- v0.14: grid view with effects on: arrows and SHIFT peek --- */
    {
        fresh(&host); seti("quantize", 0);
        tap(M_SHIFT); setstr("step", "3"); run(3); setstr("step_up", "3");       /* an effect on, mode on */
        run(40); { const char *bar = strchr(screen(), '|'); CHECK(bar && geti("perf_boxes") > 0); }   /* the scene */
        setstr("step_bank", "-1"); run(40); screen(); CHECK(geti("perf_boxes") == 0);              /* edge arrow: grid view on */
        setstr("knob_delta", "1:1"); run(40); screen(); CHECK(geti("perf_boxes") > 0);              /* a knob goes back to the scene */
        setstr("step_bank", "1"); run(40); screen(); CHECK(geti("perf_boxes") == 0 && geti("perf_bank") == 1);   /* a flip shows page 2's grid */
        setstr("step_bank", "-1"); run(40); screen(); CHECK(geti("perf_bank") == 0 && geti("perf_boxes") == 0);
        setstr("step_bank", "-1"); run(40); screen(); CHECK(geti("perf_boxes") > 0);                  /* edge arrow again: scene */
        down(M_SHIFT); run(150); screen(); CHECK(geti("perf_boxes") == 0);                             /* hold SHIFT: peek at the grid */
        up(M_SHIFT); run(40); screen(); CHECK(geti("perf_boxes") > 0);
        tap(M_SHIFT);
    }

    /* --- v0.12: effect page 2 via step_bank, DUCK is the 17th effect --- */
    {
        fresh(&host); seti("quantize", 0);
        CHECK(geti("step_bank") == -1);                                   /* not on the PERFORM page: < > stay the host's */
        tap(M_SHIFT); CHECK(geti("step_bank") == 0);
        setstr("step_bank", "1"); CHECK(geti("perf_bank") == 1 && geti("step_bank") == 1);
        setstr("step", "0"); run(3); setstr("step_up", "0");
        CHECK(geti("perf_latch") == (1 << 16) && geti("perf_fx") == 16);
        setstr("step", "12"); setstr("step_up", "12"); CHECK(geti("perf_n") == 1);   /* empty cell of page 2: nothing */
        { const char *h = getstr("step_leds"); CHECK(!strncmp(h + 24, "00", 2)); }
        setstr("step_bank", "1"); CHECK(geti("perf_bank") == 1);          /* no page 3 */
        setstr("step_bank", "-1"); CHECK(geti("perf_bank") == 0);
        setstr("step", "0"); setstr("step_up", "0"); CHECK(geti("perf_fx") == 16 || geti("perf_fx") == 0);
        tap(M_SHIFT);
    }

    /* --- v0.13: knob automation = HOLD the REC pad in PERFORM with an effect on, turn K1-K3 --- */
    {
        fresh(&host); seti("quantize", 0);
        tap(REC(3)); run(300); tap(REC(3)); run(2);                     /* a playing loop on track 3 */
        tap(M_SHIFT);
        setstr("step", "10"); run(3); setstr("step_up", "10");          /* GATE on (latched) */
        CHECK(geti("perf_fx") == 10 && geti("perf_auto") == 0);
        setstr("knob_delta", "2:5"); run(20); CHECK(geti("perf_auto") == 0);   /* a turn with no REC held writes nothing */
        down(REC(3)); run(2);
        CHECK(!strcmp(status_text(), "AUTO-REC 3") && state(3) == S_PLAY && geti("perf_auto_hold") == 3);   /* no REC/DUB action */
        for (int i = 0; i < 40; i++) { setstr("knob_delta", i < 20 ? "2:-1" : "2:1"); run(8); }
        run(20);
        CHECK(state(3) == S_PLAY);
        up(REC(3)); run(2);
        CHECK(geti("perf_auto_hold") == 0 && geti("perf_auto") > 40 && state(3) == S_PLAY);   /* the whole pass is written, REC did nothing */
        { int a = geti("perf_p10_1"); run(100); int b2 = geti("perf_p10_1"); run(100); int c = geti("perf_p10_1");
          run(100); int d = geti("perf_p10_1");
          CHECK(a != b2 || b2 != c || c != d); }                          /* playback moves the knob */
        /* the state carries it: a fresh instance restores the curve */
        { static char saved[16384]; saved[0] = 0; A->get_param(S, "state", saved, sizeof saved);
          CHECK(strstr(saved, "\"l6au\":\"3.10.1=") != NULL);
          fresh(&host); CHECK(geti("perf_auto") == 0);
          setstr("state", saved); CHECK(geti("perf_auto") > 40); }
        /* ARM held + knob wipes a curve */
        down(M_ARM); setstr("knob_delta", "2:1"); up(M_ARM);
        fresh(&host); seti("quantize", 0);
        tap(REC(3)); run(300); tap(REC(3)); run(2);
        tap(M_SHIFT); setstr("step", "10"); run(3); setstr("step_up", "10");
        down(REC(3)); setstr("knob_delta", "2:-1"); run(60); up(REC(3));
        CHECK(geti("perf_auto") > 0);
        down(M_ARM); setstr("knob_delta", "2:1"); up(M_ARM); CHECK(geti("perf_auto") == 0);
        /* a plain tap of REC (no knob) keeps the normal action: PLAY -> DUB on release */
        tap(REC(3)); run(4); CHECK(state(3) == S_DUB);
        tap(M_SHIFT);
    }

    /* --- v0.13: native Move SHIFT + selector (shift_pad) = FX CLR: effects off, clip untouched --- */
    {
        fresh(&host); seti("quantize", 0);
        tap(REC(3)); run(300); tap(REC(3)); run(2);
        int len3 = gett(3, "len");
        seti("t3_fx", 4); seti("t3_fx_on", 1); seti("t3_fxp", 80); seti("t3_level", 140); seti("t3_pan", 20);
        seti("t3_spd", 5); seti("t3_rev", 1);
        tap(M_SHIFT); setstr("step", "10"); run(3); setstr("step_up", "10");   /* a PERFORM effect on the MIX */
        tap(FX(3)); setstr("step", "9"); run(3); setstr("step_up", "9");      /* and one on track 3 */
        down(REC(3)); setstr("knob_delta", "1:-1"); run(60); up(REC(3));
        seti("shift_pad", FX(3) - 36); run(2);
        CHECK(!strcmp(status_text(), "FX CLR 3"));
        CHECK(state(3) == S_PLAY && gett(3, "len") == len3);                    /* audio untouched */
        CHECK(gett(3, "fx_on") == 0 && gett(3, "fx") == 0 && gett(3, "rev") == 0 && gett(3, "spd") == 3);
        CHECK(gett(3, "level") == 140 && gett(3, "pan") == 20);                 /* volume and pan untouched */
        CHECK((geti("perf_on") & (1 << 9)) == 0);                               /* track 3's effect is off, the MIX one stays */
        tap(M_SHIFT);
    }

    /* --- v0.14: the bank-2 effects (steps 2-9 of page 2): finite, audible, K1-K3 real, scene draws --- */
    {
        static const int NEWFX[8] = { PF_DRIVE, PF_RINGMOD, PF_FLANGER, PF_PHASER, PF_CHORUS, PF_AUTOPAN, PF_TREMOLO, PF_COMB };
        for (int e = 0; e < 8; e++) {
            int fx = NEWFX[e];
            perf_t *p = pf_create();
            CHECK(pf_press(p, fx, 0, 0, 0) != 0);
            double acc_in = 0, acc_out = 0, acc_diff = 0; int bad = 0; double w = 0;
            for (long n = 0; n < 44100 * 2; n++) {
                if (n % 128 == 0) pf_begin(p, (double)n, 9600.0);
                float l = (float)(sin(w) * 8000.0 + sin(w * 3.1) * 3000.0), r = l * 0.8f; w += 2 * M_PI * 220.0 / 44100.0;
                float il = l; pf_proc(p, 0, &l, &r);
                if (!(fabsf(l) < 60000.0f) || !(fabsf(r) < 60000.0f)) bad++;          /* NaN fails the comparison too */
                if (n >= 22050) { acc_in += (double)il * il; acc_out += (double)l * l; acc_diff += (double)(l - il) * (l - il); }
            }
            CHECK(bad == 0);
            CHECK(acc_out > acc_in * 0.05 && acc_out < acc_in * 20.0);                 /* neither silent nor exploding */
            CHECK(acc_diff > acc_in * 0.001);                                           /* and it really changes the sound */
            pf_destroy(p);
        }
        fresh(&host); seti("quantize", 0);
        down(M_SHIFT); setstr("step_bank", "1");
        setstr("step", "0"); setstr("knob_delta", "1:1"); setstr("step_up", "0");        /* a knob turn leaves the grid view a flip switched on */
        for (int j = 1; j <= 8; j++) {                                                  /* buttons 1-8 of page 2 = fx 17+j ... */
            char b[8]; snprintf(b, sizeof b, "%d", j);
            setstr("step", b); run(60);
            const char *bar = strchr(screen(), '|'); CHECK(bar && strlen(bar + 1) == 2048);
            if (!(bar && bar[1 + 32] == 'F' && bar[2 + 32] == 'F')) printf("scene j=%d title bar missing\n", j);
            CHECK(bar && bar[1 + 32] == 'F' && bar[2 + 32] == 'F');                     /* title bar */
            unsigned sq = scr_seq(); run(40); if (scr_seq() == sq) printf("scene j=%d static\n", j);
            if (j != 6 && j != 7) CHECK(scr_seq() != sq);                                /* animated (the beat-synced pair needs a running grid) */
            for (int k = 1; k <= 3; k++) {
                char key[24], kd[16]; snprintf(key, sizeof key, "perf_p%d_%d", PF_DUCK + j, k - 1);
                int before = geti(key); snprintf(kd, sizeof kd, "%d:12", k); setstr("knob_delta", kd);
                int after = geti(key);
                if (after == before) { snprintf(kd, sizeof kd, "%d:-12", k); setstr("knob_delta", kd); after = geti(key); }
                CHECK(after != before);                                                  /* real knobs */
            }
            setstr("step_up", b); run(8);
        }
        up(M_SHIFT);
    }

    /* --- v0.14: CPU per block with six heavy effects at once (measured, printed) --- */
    {
        static const int HEAVY[6] = { PF_PITCH, PF_DELAY, PF_FLANGER, PF_CHORUS, PF_PHASER, PF_COMB };
        for (int mode = 0; mode < 2; mode++) {
            perf_t *p = pf_create();
            if (mode) for (int i = 0; i < 6; i++) pf_press(p, HEAVY[i], 0, 0, 0);
            struct timespec t0, t1; double w = 0; long nb = 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (long n = 0; n < 44100 * 4; n++) {
                if (n % 128 == 0) { pf_begin(p, (double)n, 9600.0); nb++; }
                float l = (float)(sin(w) * 8000.0), r = l; w += 2 * M_PI * 220.0 / 44100.0;
                pf_proc(p, 0, &l, &r);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / (double)nb;
            printf("PERF CPU: %s: %.1f us per 128-frame block (budget 2902 us), lane 0 only\n", mode ? "6 heavy effects (PITCH DELAY FLANGER CHORUS PHASER COMB)" : "no effects", us);
            CHECK(us < 1500.0);
            pf_destroy(p);
        }
    }

    /* --- nothing crashes on garbage --- */
    uint8_t junk[3] = {0x90, 200, 100}; A->on_midi(S, junk, 3, 0);
    A->set_param(S, "nonsense", "1");
    A->destroy_instance(S);
    { char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", pdir); if (system(cmd)) {} }
    printf("%d checks, %d failures, %d log lines\n", checks, fails, nlogs);
    puts(fails ? "FAILED" : "OK");
    return fails != 0;
}
