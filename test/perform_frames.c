/* Host tool: drives the module through every PERFORM effect and dumps the
 * screen_param framebuffer as PBM files (scripts/frames2png.py upscales them).
 * usage: perform_frames <plugin> <out-dir> */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "plugin_api_v1.h"

static uint8_t mem[4096];
static float fake_bpm(void) { return 120.0f; }
static host_api_v1_t host = { .sample_rate = 44100, .frames_per_block = 128, .mapped_memory = mem,
    .audio_out_offset = 256, .audio_in_offset = 2304, .get_bpm = fake_bpm };
static plugin_api_v2_t *A; static void *S;
static double ph; static int16_t out[256];
static int silent;

static void run(int blocks) {
    int16_t *in = (int16_t *)(mem + 2304);
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 128; i++) {
            static long n; double env = exp(-(double)(n % 11025) / 2500.0);
            double v = silent ? 0.0 : (sin(ph) * 0.5 + sin(ph * 1.5) * 0.3) * env * 0.6;
            ph += 2 * M_PI * 110.0 / 44100.0; n++;
            in[2 * i] = in[2 * i + 1] = (int16_t)(v * 32767.0);
        }
        A->render_block(S, out, 128);
    }
}
static void note(int n, int on) { uint8_t m[3] = { on ? 0x90 : 0x80, (uint8_t)n, 100 }; A->on_midi(S, m, 3, 0); }
static void tap(int n) { note(n, 1); note(n, 0); }
static int note_of(int row, int col) { return 36 + (row - 1) * 4 + (col % 4) + 16 * (col / 4); }

static void dump(const char *dir, const char *name) {
    static char b[4200];
    b[0] = 0; A->get_param(S, "screen", b, sizeof b);
    char *bar = strchr(b, '|');
    if (!bar || strlen(bar + 1) != 2048) { printf("%s: no frame (%zu)\n", name, strlen(b)); return; }
    char path[400]; snprintf(path, sizeof path, "%s/%s.pbm", dir, name);
    FILE *f = fopen(path, "wb"); fprintf(f, "P4\n128 64\n");
    for (int i = 0; i < 1024; i++) { char h[3] = { bar[1 + i * 2], bar[2 + i * 2], 0 }; fputc(strtol(h, NULL, 16), f); }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    mkdir(argv[2], 0755);
    setenv("LOOPER606_PERSIST_DIR", "/tmp/looper606-frames", 1);
    void *h = dlopen(argv[1], RTLD_NOW);
    move_plugin_init_v2_fn init = (move_plugin_init_v2_fn)dlsym(h, "move_plugin_init_v2");
    A = init(&host); S = A->create_instance("/tmp/m", NULL);
    A->set_param(S, "quantize", "0");
    run(10); tap(note_of(1, 1)); run(300); tap(note_of(1, 1)); run(4);
    tap(note_of(4, 1));                                       /* PISTA 1 */
    note(note_of(4, 0), 1);                                   /* SHIFT held */
    run(8); dump(argv[2], "page");
    char nm[64], st[8];
    for (int fx = 0; fx < 8; fx++) {
        snprintf(st, sizeof st, "%d", fx);
        A->set_param(S, "step", st);
        for (int f = 0; f < 4; f++) {
            run(f == 0 ? 10 : 50 + 20 * f);
            snprintf(nm, sizeof nm, "fx%d_%d", fx + 1, f);
            dump(argv[2], nm);
        }
        A->set_param(S, "step_up", st);
        run(20);
    }
    note(note_of(4, 0), 0);
    return 0;
}
