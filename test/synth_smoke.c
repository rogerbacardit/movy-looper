/* Host tests for the looper-606 sound_generator (dlopen'd .so/.dylib).
 * usage: synth_smoke <plugin> <hierarchy-out.json> */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
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

enum { IN_SINE, IN_SILENCE, IN_LOUD };
static int in_mode = IN_SINE; static double ph; static int in_dc = 0;
static void fill_input(void) {
    int16_t *in = (int16_t *)(mem + 2304);
    for (int i = 0; i < 128; i++) {
        double a = in_mode == IN_SILENCE ? 0.0 : in_mode == IN_LOUD ? 0.5 : 0.25;
        int16_t v = (int16_t)(sin(ph) * a * 32767.0); ph += 2 * M_PI * 220.0 / 44100.0;
        if (in_dc) v = (int16_t)in_dc;
        in[2 * i] = in[2 * i + 1] = v;
    }
}
static int16_t out[256]; static int out_peak;
static void run(int blocks) {
    for (int b = 0; b < blocks; b++) {
        fill_input(); A->render_block(S, out, 128);
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
static const char *disp_head(void) { disp(); return strrchr(dbuf, '|') + 1; }
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
#define M_ALL   note_of(1, 0)
#define M_STOPALL note_of(2, 0)
#define M_ARM   note_of(3, 0)
#define M_MODE  note_of(4, 0)
#define UNDO()  setstr("undo_cmd", "undo")
#define REDO()  setstr("undo_cmd", "redo")
#define R_QUANT note_of(1, 7)
#define R_FIXLEN note_of(2, 7)
#define R_RECACT note_of(3, 7)
#define R_COUNTIN note_of(4, 7)

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
    seti("delete_pad", M_MODE - 36); seti("delete_pad", 99);            /* master column / junk: ignored */

    /* --- count-in --- */
    tap(R_COUNTIN); CHECK(geti("count_in") == 1);
    tap(REC(5)); CHECK(state(5) == S_ARMED);
    run(600); CHECK(state(5) == S_ARMED);                           /* < 2 s */
    run(150); CHECK(state(5) == S_REC);                             /* 4 beats @120 = 2 s */
    tap(REC(5)); run(2);
    tap(R_COUNTIN); CHECK(geti("count_in") == 0);

    /* --- fixed length: 1 bar @120 = 2 s --- */
    tap(M_STOPALL);
    for (int i = 0; i < 5; i++) tap(R_FIXLEN);                      /* full cycle -> Free */
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
    tap(FX(2)); CHECK(geti("t2_fx_on") == 1); CHECK(geti("cur_track") == 2);
    CHECK(geti("fx_type") >= 1);
    seti("fx_type", 3); CHECK(geti("t2_fx") == 3);
    seti("fx_amt", 60); CHECK(geti("t2_fxp") == 60);
    seti("trk_pan", 20); CHECK(geti("t2_pan") == 20);
    seti("trk_shot", 1); CHECK(geti("t2_shot") == 1);
    tap(FX(2)); CHECK(geti("t2_fx_on") == 0);

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
    seti("delete_pad", FX(1) - 36); CHECK(state(1) == S_EMPTY); CHECK(geti("t1_len") == 0);
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
    CHECK(geti("trk_spd") == 1);
    int base_c = crossings(300);
    CHECK(base_c > 50);
    seti("trk_spd", 2); CHECK(geti("t1_spd") == 2);
    int c2 = crossings(300);   CHECK(c2 > base_c * 17 / 10 && c2 < base_c * 23 / 10);
    tap(REC(1)); CHECK(state(1) == S_PLAY);                         /* no overdub while varispeed */
    seti("trk_spd", 0); CHECK(geti("t1_spd") == 0);
    int ch = crossings(300);   CHECK(ch > base_c * 4 / 10 && ch < base_c * 6 / 10);
    seti("trk_rev", 1); seti("trk_spd", 2);
    int cr = crossings(300);   CHECK(cr > base_c * 17 / 10 && cr < base_c * 23 / 10);
    out_peak = 0; run(100); CHECK(out_peak > 500);                  /* reverse + varispeed audible */
    seti("trk_rev", 0); seti("trk_spd", 1);
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
        tap(REC(1)); STATUS(); CHECK(!strcmp(txt, "DUB 1")); CHECK(LEDOF(RACKPAD(1, 1)) == 20);   /* dub = violet */
        CHECK(LEDOF(RACKPAD(2, 1)) == 126);
        tap(REC(1));
        tap(STOP(1)); STATUS(); CHECK(!strcmp(txt, "STOP 1"));
        CHECK(LEDOF(RACKPAD(2, 1)) == 99);                       /* stopped = dim track colour on row 2 */
        seti("delete_pad", REC(1) - 36); STATUS(); CHECK(!strcmp(txt, "CLEAR 1"));
        CHECK(LEDOF(RACKPAD(1, 1)) == 98 && LEDOF(RACKPAD(2, 1)) == 0);
        /* settings + master pads */
        CHECK(LEDOF(RACKPAD(1, 7)) == 92);                       /* quantize off */
        tap(R_QUANT); STATUS(); CHECK(!strcmp(txt, "QUANT ON")); CHECK(LEDOF(RACKPAD(1, 7)) == 14);
        tap(R_COUNTIN); STATUS(); CHECK(!strcmp(txt, "COUNT-IN ON")); CHECK(LEDOF(RACKPAD(4, 7)) == 7);
        tap(R_RECACT); STATUS(); CHECK(!strcmp(txt, "REC>DUB")); CHECK(LEDOF(RACKPAD(3, 7)) == 20);
        tap(R_FIXLEN); STATUS(); CHECK(!strcmp(txt, "LEN 1 BAR")); CHECK(LEDOF(RACKPAD(2, 7)) == 123);
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
        #define DISPLAY() do { b[0] = 0; A->get_param(S, "display", b, sizeof b); } while (0)
        #define DSEQ()  atoi(b)
        #define DPOS()  atoi(strchr(b, '|') + 1)
        #define DPEAKS(o) do { const char *q = strchr(strchr(b, '|') + 1, '|') + 1; \
                               size_t n = strcspn(q, "|"); memcpy(o, q, n); (o)[n] = 0; } while (0)
        #define DHEAD() (strrchr(b, '|') + 1)
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
        seti("trk_spd", 2);
        p1 = disp_pos(); run(5); p2 = disp_pos();
        back = (p1 - p2 + 128) % 128;
        CHECK(back >= 3 && back <= 9);                              /* 2x reverse: about twice as fast, still leftwards */
        seti("trk_rev", 0);
        p1 = disp_pos(); run(5); p2 = disp_pos();
        fwd = (p2 - p1 + 128) % 128;
        CHECK(fwd >= 3 && fwd <= 9);                                /* 2x forward */
        seti("trk_spd", 0);
        p1 = disp_pos(); run(10); p2 = disp_pos();
        fwd = (p2 - p1 + 128) % 128;
        CHECK(fwd >= 1 && fwd <= 5);                                /* 1/2x: 10 blocks = 5 blocks at 1x */
        seti("trk_spd", 1);
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
        seti("rec_len", 99); CHECK(geti("rec_len") == 16);
        seti("rec_len", 0);
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
        setstr("loop_knob", "1:-1");                                  /* wraps below zero */
        CHECK(abs(gett(1, "off") - (L - 5512)) <= 3);
        CHECK(strstr(status_text(), "START 1 +") != NULL);
        setstr("loop_knob", "1:1"); CHECK(gett(1, "off") <= 3);       /* and back to ~0 (a frame of rounding) */
        /* fine mode: 1/64 note, free of the 1/16 grid; coarse snaps back */
        setstr("loop_knob", "1:8");
        setstr("loop_knob", "1:1:1");
        CHECK(abs(gett(1, "off") - (int)(33 * 1378.125 + 0.5)) <= 3);
        CHECK(strstr(status_text(), " MS") != NULL);
        setstr("loop_knob", "1:1");
        CHECK(abs(gett(1, "off") - (int)(9 * 5512.5 + 0.5)) <= 3);
        setstr("loop_knob", "1:-9");
        /* other tracks: empty = refused with a message; knobs 7/8 and junk = nothing */
        setstr("loop_knob", "3:2"); CHECK(gett(3, "off") == 0); CHECK(!strcmp(status_text(), "START 3 EMPTY"));
        setstr("loop_knob", "7:1"); setstr("loop_knob", "0:1"); setstr("loop_knob", "x"); setstr("loop_knob", "1:0");
        CHECK(gett(1, "off") <= 3);
        /* reverse + varispeed still play from the moved start */
        setstr("loop_knob", "1:8"); seti("cur_track", 1); seti("trk_rev", 1);
        tap(STOP(1)); tap(STOP(1)); run(2);
        out_peak = 0; run(20); CHECK(out_peak > 500);                 /* reversed from buffer L/2 - 1: the loud half */
        seti("trk_rev", 0);

        /* overdub writes where it plays: with the start at L/2 a dub from the top lands at bin 64 */
        CHECK(disp_peak(64) == 0 && disp_peak(66) == 0);
        tap(STOP(1)); tap(REC(1));
        run(2); CHECK(state(1) == S_DUB);
        in_mode = IN_LOUD; run(10); in_mode = IN_SILENCE;
        setstr("loop_knob", "1:1"); CHECK(!strcmp(status_text(), "START 1 LOCKED"));   /* not while dubbing */
        CHECK(abs(gett(1, "off") - 44100) <= 3);
        tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
        tap(STOP(1)); run(1);
        CHECK(disp_peak(64) > 0);                                     /* the dub is at buffer L/2... */
        CHECK(disp_peak(10) > 100 && disp_peak(100) == 0);            /* ...and the rest of the loop is as recorded */
        /* undo of the dub restores the quiet half, clear + undo restores the start */
        UNDO(); run(200); CHECK(disp_peak(64) == 0);
        REDO(); run(200);
        seti("delete_pad", REC(1) - 36); CHECK(state(1) == S_EMPTY);
        CHECK(gett(1, "off") == 0 && disp_mark() == -1);
        UNDO(); CHECK(state(1) == S_STOP); CHECK(abs(gett(1, "off") - 44100) <= 3);
        seti("delete_pad", REC(1) - 36); UNDO();
        /* a moved start is shown on the display even after the readout fades */
        in_mode = IN_SINE; run(1200); in_mode = IN_SILENCE;
        CHECK(abs(disp_mark() - 63) <= 2 && strncmp(disp_head(), "START", 5) != 0);
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
        sett(1, "rev", 1); sett(1, "spd", 2);
        setstr("step", "1"); run(50); out_peak = 0; run(100); CHECK(out_peak > 100);
        CHECK(gett(1, "off") < gett(1, "len") && state(1) == S_PLAY);
        sett(1, "rev", 0); sett(1, "spd", 1);
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

    /* --- MODE pad: MIX > START > END > FX > PAN, knob capture, per-mode knobs --- */
    {
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        CHECK(strstr(getstr("ui_hierarchy"), "\"knob_capture_param\":\"knob_capture\"") != NULL);
        CHECK(strstr(sbuf, "\"knob_delta_param\":\"knob_delta\"") != NULL && strstr(sbuf, "\"undo_param\":\"undo_cmd\"") != NULL);
        CHECK(geti("knob_capture") == 0 && pad_led(M_MODE) == 122);
        static const int col[5] = { 122, 14, 7, 23, 17 };
        static const char *const nm[5] = { "MIX", "START", "END", "FX", "PAN" };
        for (int k = 1; k <= 5; k++) {
            tap(M_MODE);
            int m = k % 5; char want[24]; snprintf(want, sizeof want, "MODE %s", nm[m]);
            CHECK(!strcmp(status_text(), want));
            CHECK(geti("knob_capture") == (m != 0) && pad_led(M_MODE) == col[m]);
            CHECK(pad_led(M_MODE) != 127 && pad_led(M_MODE) != 126);  /* never red / green */
        }
        CHECK(geti("knob_capture") == 0);
        /* the pad no longer undoes anything */
        tap(REC(1)); run(100); tap(REC(1)); run(2);
        CHECK(geti("undo_avail") == 1); tap(M_MODE); tap(M_MODE); tap(M_MODE); tap(M_MODE); tap(M_MODE);
        CHECK(state(1) == S_PLAY && gett(1, "len") > 0);
        /* knob_delta does nothing in MIX */
        setstr("knob_delta", "1:4"); CHECK(gett(1, "off") == 0 && geti("t1_level") == 100);
        /* UNDO / REDO come from undo_cmd only; nothing to redo yet */
        REDO(); CHECK(!strcmp(status_text(), "NOTHING TO REDO"));
        setstr("undo_cmd", "bogus"); CHECK(state(1) == S_PLAY);
        UNDO(); CHECK(state(1) == S_EMPTY && !strcmp(status_text(), "UNDO"));
        REDO(); CHECK(state(1) == S_STOP && !strcmp(status_text(), "REDO"));
        UNDO(); UNDO(); CHECK(!strcmp(status_text(), "NOTHING TO UNDO") && state(1) == S_EMPTY);
        REDO();
        tap(STOP(1)); run(2); setstr("step", "0");

        /* START: knob N = start offset of track N (grid 1/16, Shift fine) */
        fresh(&host); seti("quantize", 0); in_mode = IN_SINE;
        setstr("step", "0"); tap(REC(1)); run(700); run(4);
        long L = gett(1, "len"); CHECK(state(1) == S_PLAY);
        tap(M_MODE);                                                  /* START */
        CHECK(disp_mark() == 0 && strncmp(disp_head(), "START 1", 7) == 0);   /* the marker is shown even at 0 */
        setstr("knob_delta", "1:8");
        CHECK(abs(gett(1, "off") - 44100) <= 3 && !strcmp(status_text(), "START 1 +8/16"));
        CHECK(!strcmp(disp_head(), "START 1 +8/16") && abs(disp_mark() - 63) <= 2);
        setstr("knob_delta", "1:1:1");                                /* fine: 1/64 note */
        CHECK(abs(gett(1, "off") - 44100 - 1378) <= 3);
        setstr("knob_delta", "1:1"); CHECK(abs(gett(1, "off") - 44100 - 5512) <= 3);   /* back on the 1/16 grid */
        setstr("knob_delta", "2:3"); CHECK(!strcmp(status_text(), "START 2 EMPTY") && gett(2, "off") == 0);
        setstr("knob_delta", "9:1"); setstr("knob_delta", "x"); setstr("knob_delta", "1:0");
        setstr("loop_knob", "1:-8");                                  /* Loop + knob still works in any mode */
        CHECK(gett(1, "off") < 20000);

        /* END: knob N = loop end/length of track N */
        tap(M_MODE);
        setstr("knob_delta", "1:-4");                                 /* -4/16 */
        CHECK(labs(gett(1, "len") - 66150) < 2 && gett(1, "full") == L);
        CHECK(!strcmp(status_text(), "END 1 12/16") && !strcmp(disp_head(), "END 1 12/16"));
        CHECK(abs(disp_end() - 96) <= 2);                             /* end marker on the whole-buffer view */
        CHECK(disp_mark() >= 0);                                      /* start marker too (offset != 0 earlier or fine) */
        setstr("knob_delta", "1:-2");                                 /* a burst of turns = ONE undo step */
        UNDO(); CHECK(labs(gett(1, "len") - L) < 8);
        REDO(); CHECK(labs(gett(1, "len") - 55125) < 2);
        UNDO();
        setstr("knob_delta", "1:-16"); setstr("knob_delta", "1:-16");   /* never below one 1/16 note */
        CHECK(gett(1, "len") >= 5512 && gett(1, "len") <= 5513);
        setstr("knob_delta", "1:-16"); CHECK(gett(1, "len") >= 5512);
        setstr("knob_delta", "1:16"); setstr("knob_delta", "1:16");
        CHECK(gett(1, "len") > 5512 * 8);
        setstr("knob_delta", "1:16"); setstr("knob_delta", "1:16"); setstr("knob_delta", "1:16");   /* past the recorded end: repeats */
        CHECK(gett(1, "len") > L && gett(1, "full") == gett(1, "len"));
        UNDO(); CHECK(gett(1, "full") == L);
        setstr("knob_delta", "2:1"); CHECK(!strcmp(status_text(), "END 2 EMPTY"));
        tap(M_MODE); tap(M_MODE); tap(M_MODE);                        /* FX, PAN, MIX */
        CHECK(geti("knob_capture") == 0);
        tap(M_MODE); tap(M_MODE); tap(M_MODE);                        /* START, END, FX */
        CHECK(!strcmp(status_text(), "MODE FX"));

        /* FX: knob N = FX amount of track N, Shift = FX type */
        int a0 = gett(2, "fxp");
        setstr("knob_delta", "2:5"); CHECK(gett(2, "fxp") == a0 + 10);
        setstr("knob_delta", "2:-100"); CHECK(gett(2, "fxp") == 0);
        for (int i = 0; i < 8; i++) setstr("knob_delta", "2:-1:1");   /* down to Off first (clamped) */
        CHECK(gett(2, "fx") == 0 && gett(2, "fx_on") == 0 && !strcmp(status_text(), "FX 2 OFF"));
        setstr("knob_delta", "2:1:1"); CHECK(gett(2, "fx") == 1 && gett(2, "fx_on") == 1 && !strcmp(status_text(), "FX 2 LPF"));
        setstr("knob_delta", "2:1:1"); CHECK(gett(2, "fx") == 2);
        for (int i = 0; i < 9; i++) setstr("knob_delta", "2:1:1");
        CHECK(gett(2, "fx") == 6);
        setstr("knob_delta", "2:-1:1"); setstr("knob_delta", "2:-1:1");
        for (int i = 0; i < 9; i++) setstr("knob_delta", "2:-1:1");
        CHECK(gett(2, "fx") == 0 && gett(2, "fx_on") == 0);
        setstr("knob_delta", "3:5"); CHECK(gett(3, "fxp") == 100 || gett(3, "fxp") == a0 + 10);
        CHECK(!strncmp(disp_head(), "FX 3 ", 5));

        /* PAN */
        tap(M_MODE);
        CHECK(!strcmp(status_text(), "MODE PAN"));
        setstr("knob_delta", "3:-5"); CHECK(gett(3, "pan") == 40 && !strcmp(status_text(), "PAN 3 L20"));
        setstr("knob_delta", "3:-100"); CHECK(gett(3, "pan") == 0);
        setstr("knob_delta", "3:100"); CHECK(gett(3, "pan") == 100 && !strcmp(status_text(), "PAN 3 R100"));
        setstr("knob_delta", "3:-25"); CHECK(gett(3, "pan") == 50 && !strcmp(status_text(), "PAN 3 C"));
        setstr("knob_delta", "1:-5"); setstr("knob_delta", "1:5");
        CHECK(!strcmp(disp_head(), "PAN 1 C"));
        /* knobs 7 and 8 stay input volume and master in every mode */
        setstr("knob_delta", "7:-2"); CHECK(geti("in_gain") == 90);
        setstr("knob_delta", "8:-3"); CHECK(geti("master") == 85);
        setstr("knob_delta", "8:100"); CHECK(geti("master") == 200);
        tap(M_MODE); CHECK(geti("knob_capture") == 0);
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
