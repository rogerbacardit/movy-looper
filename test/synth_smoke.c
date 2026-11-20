/* Host tests for the looper-606 sound_generator (dlopen'd .so/.dylib).
 * usage: synth_smoke <plugin> <hierarchy-out.json> */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    down(M_ARM); tap(STOP(1)); up(M_ARM);
    CHECK(geti("t1_rev") == 1); CHECK(geti("arm") == 0); CHECK(state(1) == S_PLAY);
    down(M_ARM); tap(STOP(1)); up(M_ARM); CHECK(geti("t1_rev") == 0); CHECK(state(1) == S_PLAY);
    /* ARM + row 1 can never record/dub: it is swallowed during the 1 s window */
    down(M_ARM); tap(REC(1)); up(M_ARM);
    CHECK(state(1) == S_PLAY && geti("t1_rev") == 0 && geti("arm") == 1);
    tap(M_ARM); CHECK(geti("arm") == 0); run(400);

    /* --- ARM + threshold --- */
    tap(M_ARM); CHECK(geti("arm") == 1); run(400);                 /* past the 1 s REVERSE window */
    in_mode = IN_SILENCE; tap(REC(2)); CHECK(state(2) == S_ARMED);
    run(100); CHECK(state(2) == S_ARMED);                          /* below threshold */
    in_mode = IN_SINE; run(3); CHECK(state(2) == S_REC);           /* crossed threshold */
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
    seti("delete_pad", M_UNDO - 36); seti("delete_pad", 99);            /* master column / junk: ignored */

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

    /* --- CLEAR is undoable (and redoable) --- */
    fresh(&host); seti("quantize", 0);
    tap(REC(1)); run(300); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    int len1 = geti("t1_len"); CHECK(len1 > 0);
    tap(STOP(1)); CHECK(state(1) == S_STOP);
    seti("delete_pad", FX(1) - 36); CHECK(state(1) == S_EMPTY); CHECK(geti("t1_len") == 0);
    CHECK(geti("undo_avail") == 1);
    tap(M_UNDO); run(2);
    CHECK(state(1) == S_STOP); CHECK(geti("t1_len") == len1);
    tap(STOP(1)); run(2); CHECK(state(1) == S_PLAY);
    out_peak = 0; run(200); CHECK(out_peak > 500);                  /* audio really came back */
    tap(M_UNDO); CHECK(state(1) == S_EMPTY);                        /* redo = clear again */
    tap(M_UNDO); CHECK(state(1) == S_STOP && geti("t1_len") == len1);

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
    /* ARM is note-on only too; ARM tap + row-2 tap within 1 s = REVERSE, ARM unchanged */
    fresh(&host); seti("quantize", 0);
    down(M_ARM); CHECK(geti("arm") == 1);
    run(400); down(REC(1)); CHECK(state(1) == S_ARMED); down(REC(1)); CHECK(state(1) == S_EMPTY);
    tap(REC(1)); run(200); tap(REC(1)); run(2); CHECK(state(1) == S_PLAY);
    down(M_ARM); CHECK(geti("arm") == 0); down(STOP(1)); up(STOP(1));
    CHECK(geti("t1_rev") == 1 && geti("arm") == 1 && state(1) == S_PLAY);   /* arm restored, no stop */

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
        tap(M_UNDO); STATUS(); CHECK(!strcmp(txt, "UNDO/REDO"));

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

    /* --- nothing crashes on garbage --- */
    uint8_t junk[3] = {0x90, 200, 100}; A->on_midi(S, junk, 3, 0);
    A->set_param(S, "nonsense", "1");
    A->destroy_instance(S);
    printf("%d checks, %d failures, %d log lines\n", checks, fails, nlogs);
    puts(fails ? "FAILED" : "OK");
    return fails != 0;
}
