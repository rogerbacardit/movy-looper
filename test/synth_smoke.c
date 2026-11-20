/* Host tests for the movy-looper sound_generator (dlopen'd .so/.dylib).
 * usage: synth_smoke <plugin> <hierarchy-out.json> */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_api_v1.h"

static uint8_t mem[4096];
static float fake_bpm(void) { return 120.0f; }
static int nlogs, got_clock_log;
static void fake_log(const char *m) { nlogs++; if (strstr(m, "first MIDI clock")) got_clock_log = 1; }
static host_api_v1_t host = { .sample_rate = 44100, .frames_per_block = 128,
    .mapped_memory = mem, .audio_out_offset = 256, .audio_in_offset = 2304,
    .get_bpm = fake_bpm, .log = fake_log };

static plugin_api_v2_t *A; static void *S;
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

enum { IN_SINE, IN_SILENCE, IN_LOUD };
static int in_mode = IN_SINE; static double ph;
static void fill_input(void) {
    int16_t *in = (int16_t *)(mem + 2304);
    for (int i = 0; i < 128; i++) {
        double a = in_mode == IN_SILENCE ? 0.0 : in_mode == IN_LOUD ? 0.5 : 0.25;
        int16_t v = (int16_t)(sin(ph) * a * 32767.0); ph += 2 * M_PI * 220.0 / 44100.0;
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
/* pad shortcuts: track t = 1..6 -> col t */
#define REC(t)  note_of(1, t)
#define STOP(t) note_of(2, t)
#define MUTE(t) note_of(3, t)
#define FX(t)   note_of(4, t)
#define M_ALL   note_of(1, 0)
#define M_STOPALL note_of(2, 0)
#define M_ARM   note_of(3, 0)
#define M_UNDO  note_of(4, 0)
#define R_QUANT note_of(1, 7)
#define R_FIXLEN note_of(2, 7)
#define R_RECACT note_of(3, 7)
#define R_COUNTIN note_of(4, 7)

int main(int argc, char **argv) {
    (void)argc;
    void *h = dlopen(argv[1], RTLD_NOW); CHECK(h);
    move_plugin_init_v2_fn init = (move_plugin_init_v2_fn)dlsym(h, "move_plugin_init_v2"); CHECK(init);
    A = init(&host);
    S = A->create_instance("/tmp/modules/sound_generators/movy-looper", NULL); CHECK(S);

    /* --- hierarchy + params --- */
    static char hb[40000];
    CHECK(A->get_param(S, "ui_hierarchy", hb, sizeof hb) > 1000);
    FILE *f = fopen(argv[2], "w"); fputs(hb, f); fclose(f);
    seti("t6_level", 77); CHECK(geti("t6_level") == 77);          /* track 6 reaches the core */
    seti("t6_level", 100);
    seti("threshold", 12); CHECK(geti("threshold") == 12); seti("threshold", 8);
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
    down(M_ARM); tap(REC(1)); up(M_ARM);
    CHECK(geti("t1_rev") == 1); CHECK(geti("arm") == 0); CHECK(state(1) == S_PLAY);
    down(M_ARM); tap(REC(1)); up(M_ARM); CHECK(geti("t1_rev") == 0);

    /* --- ARM + threshold --- */
    tap(M_ARM); CHECK(geti("arm") == 1);
    in_mode = IN_SILENCE; tap(REC(2)); CHECK(state(2) == S_ARMED);
    run(100); CHECK(state(2) == S_ARMED);                          /* below threshold */
    in_mode = IN_SINE; run(3); CHECK(state(2) == S_REC);           /* crossed threshold */
    run(200); tap(REC(2)); run(2); CHECK(state(2) == S_PLAY);
    tap(M_ARM); CHECK(geti("arm") == 0);
    in_mode = IN_SILENCE; tap(REC(3)); tap(REC(3));                /* arm off: plain REC, then cancel */
    in_mode = IN_SINE; tap(M_ARM); tap(REC(4)); CHECK(state(4) == S_ARMED);
    tap(REC(4)); CHECK(state(4) == S_EMPTY);                        /* tap again cancels arm */
    tap(M_ARM);

    /* --- clear: hold STOP 1 s; undo restores nothing in core (documented) --- */
    tap(REC(3)); CHECK(state(3) != S_EMPTY); run(10); tap(STOP(3));
    tap(M_STOPALL);
    down(STOP(2)); run(300); CHECK(state(2) == S_PLAY || state(2) == S_STOP); run(100); up(STOP(2));
    CHECK(state(2) == S_EMPTY);                                     /* ~1.16 s hold cleared it */

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
    tap(M_UNDO); run(20);

    /* --- clock: first clock logged --- */
    clock_run(50); CHECK(got_clock_log);

    /* --- nothing crashes on garbage --- */
    uint8_t junk[3] = {0x90, 200, 100}; A->on_midi(S, junk, 3, 0);
    A->set_param(S, "nonsense", "1");
    A->destroy_instance(S);
    printf("%d checks, %d failures, %d log lines\n", checks, fails, nlogs);
    puts(fails ? "FAILED" : "OK");
    return fails != 0;
}
