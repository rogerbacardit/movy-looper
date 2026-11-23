#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mark_core.h"
#include "perform_ui.h"
#include "wire3d.h"

#define CX 44
#define CY 36
#define PI_F 3.14159265f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static unsigned rnd(perf_ui_t *u) { u->rnd = u->rnd * 1664525u + 1013904223u; return u->rnd >> 8; }

/* ---- shared HUD furniture ---- */
static void title(gfx_t *g, const char *l, const char *r) {
    gfx_frect(g, 1, 1, 126, 8, GFX_ON);
    gfx_text(g, 4, 2, l, GFX_OFF);
    gfx_text(g, 124 - gfx_text_w(r), 2, r, GFX_OFF);
}
static void frame(gfx_t *g) {
    gfx_rect(g, 0, 0, 128, 64, GFX_ON);
    for (int i = 0; i < 4; i++) {                              /* corner "+" marks */
        int x = i & 1 ? 85 : 4, y = i & 2 ? 58 : 14;
        gfx_cross(g, x, y, 2, GFX_ON);
    }
}
static void panel_sep(gfx_t *g) { gfx_dline(g, 89, 11, 89, 62, 1, 2, GFX_ON); }
static int s_boxes;                                            /* boxes drawn in the last frame (tests) */
int perf_ui_boxes(void) { return s_boxes; }
/* a box is a KNOB: a filled tab with its number on the left, then the label and the value. Nothing else is boxed. */
static void box(gfx_t *g, int y, int knob, const char *lab, const char *val) {
    char num[4]; snprintf(num, sizeof num, "%d", knob);
    gfx_rect(g, 92, y, 33, 14, GFX_ON);
    gfx_frect(g, 92, y, 7, 14, GFX_ON);
    gfx_text_c(g, 95, y + 5, num, GFX_OFF);
    gfx_text(g, 101, y + 2, lab, GFX_ON);
    gfx_text(g, 122 - gfx_text_w(val), y + 8, val, GFX_ON);
    s_boxes++;
}
static void level_bar(gfx_t *g, float lvl) {
    gfx_rect(g, 92, 56, 33, 4, GFX_ON);
    gfx_frect(g, 93, 57, (int)lroundf(clampf(lvl * 2.0f, 0.0f, 1.0f) * 31.0f), 2, GFX_ON);
}
/* ---- scenes: 3D wireframe, one per effect. All of them are drawn into the ~82 x 49 px
 * viewport left of the readouts; far edges are dotted (the depth cue), thin 1 px lines only. ---- */
static void vp(w3_t *w, gfx_t *g, float unit, float cam) {       /* viewport: `unit` px per world unit at z = 0 */
    w3_init(w, g, (float)CX, 36.0f, unit * cam, cam);
    w3_clip(w, 3, 12, 86, 61);
}
static float ease_out(float t) { t = clampf(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t); }

static void scene_beatrep(gfx_t *g, perf_t *p, const perf_vis_t *v, float tsec) {
    static const int NR[6] = { 2, 3, 4, 5, 3, 4 };
    int n = NR[(int)perf_pval(p, PF_BEATREP, 0)];
    float decay = perf_pval(p, PF_BEATREP, 1), gain = powf(decay, (float)(v->rep > 40 ? 40 : v->rep));
    int act = v->rep % n;
    w3_t w; vp(&w, g, 13.5f, 6.0f);
    w3_view(&w, 0.5f + 0.1f * sinf(tsec * 0.4f), tsec * 0.45f, 0.0f);
    float lv = 1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.08f;
    for (int i = 0; i < n; i++) {                                 /* outer cube first; each repeat lights the next one in */
        float s = (1.0f - (float)i * 0.6f / (float)(n - 1)) * lv;
        if (i == act) s *= 1.0f + 0.16f * gain * (1.0f - v->phase);
        w3_xf x = W3_XF(0, 0, 0, (float)i * 0.3f, (float)i * 0.55f, (float)i * 0.2f, 1.0f);
        w3_box(&w, &x, s, s, s, i > act ? W3_FAINT : i == 0 ? W3_AUTO : W3_SOLID);
    }
    int cnt = act + 1;                                            /* repeat counter */
    for (int i = 0; i < n; i++) {
        int x = 33 + i * 7 - n * 3;
        if (i < cnt) gfx_frect(g, x, 57, 4, 2, GFX_ON); else gfx_rect(g, x, 57, 4, 2, GFX_ON);
    }
}

static void scene_stutter(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v, float tsec) {
    float beats = perf_pval(p, PF_STUTTER, 0);
    float prog = clampf((float)(v->k / (beats * v->beat)), 0.0f, 1.0f);
    int act = v->rep & 3;
    float gap = 0.78f + 0.42f * sinf(u->rot[1]);                   /* spreads and collapses, faster as it accelerates */
    w3_t w; vp(&w, g, 13.0f, 7.0f);
    w3_view(&w, 0.6f, 0.0f, 0.0f);
    float lv = 1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.06f;
    w3_line(&w, NULL, (v3){ 0, -1.5f * gap - 0.15f, 0 }, (v3){ 0, 1.5f * gap + 0.15f, 0 }, W3_FAINT);
    for (int i = 0; i < 4; i++) {
        w3_xf x = W3_XF(0, ((float)i - 1.5f) * gap, 0, 0, tsec * 0.3f + (float)i * 0.32f, 0, lv);
        if (i == act) w3_box(&w, &x, 0.95f, 0.05f + 0.07f * (1.0f - v->phase), 0.95f, W3_AUTO);
        else w3_poly(&w, &x, 4, 0.95f * 1.41421356f, 0.0f, 0.7853982f, W3_AUTO);
    }
    int on = (int)(prog * 8.0f + 0.5f);                           /* acceleration meter */
    for (int i = 0; i < 8; i++) { int x = 30 + i * 4; if (i < on) gfx_frect(g, x, 57, 3, 2, GFX_ON); else gfx_dline(g, x, 58, x + 2, 58, 1, 1, GFX_ON); }
}

static void scene_pitch(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    int st = (int)perf_pval(p, PF_PITCH, 0);
    float ratio = powf(2.0f, ((float)st + perf_pval(p, PF_PITCH, 1) / 100.0f) / 12.0f), hh = 0.72f * ratio;
    w3_t w; vp(&w, g, 14.0f, 6.0f);
    w3_view(&w, 0.5f, u->rot[0] + 0.6f, 0.0f);
    float lv = 1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.06f;
    w3_box(&w, NULL, 0.72f * lv, hh * lv, 0.72f * lv, W3_AUTO);
    w3_line(&w, NULL, (v3){ 0, -hh - 0.5f, 0 }, (v3){ 0, hh + 0.5f, 0 }, W3_FAINT);
    for (int i = -12; i <= 12; i += 4) {                          /* ruler: -12 .. +12 semitones */
        int y = 36 - i * 3 / 2;
        gfx_line(g, 81, y, i == 0 ? 85 : 83, y, GFX_ON);
    }
    int my = 36 - st * 3 / 2;
    gfx_line(g, 79, my, 76, my - 2, GFX_ON); gfx_line(g, 79, my, 76, my + 2, GFX_ON); gfx_line(g, 76, my - 2, 76, my + 2, GFX_ON);
}

static void scene_crush(gfx_t *g, perf_t *p, const perf_vis_t *v, float tsec) {
    float bits = perf_pval(p, PF_CRUSH, 0), ds = perf_pval(p, PF_CRUSH, 1);
    float k = (bits - 2.0f) / 14.0f;                              /* 1 = clean, 0 = crushed */
    int lat = 1 + (int)lroundf(k * 4.0f), lon = 2 + (int)lroundf(k * 4.0f), segs = 8 + (int)lroundf(k * 16.0f);
    w3_t w; vp(&w, g, 21.0f, 6.0f);
    w.snap = 1.0f + floorf(ds / 7.0f);                            /* vertices on a coarse grid with downsample */
    w3_view(&w, 0.45f, tsec * 0.5f, 0.35f);
    float lv = 1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.06f;
    w3_xf x = W3_XF(0, 0, 0, 0, 0, 0, lv);
    w3_sphere(&w, &x, 1.0f, lat, lon, segs, W3_AUTO);
}

static void scene_tape(gfx_t *g, perf_ui_t *u, const perf_vis_t *v) {
    float sp = clampf(v->speed, 0.0f, 1.0f), pack0 = 0.5f + 0.5f * sp;
    w3_t w; vp(&w, g, 17.0f, 6.0f);
    w3_view(&w, 0.9f, 0.0f, 0.0f);
    for (int r = 0; r < 2; r++) {
        float tx = r ? 1.1f : -1.1f;
        w3_xf x = W3_XF(tx, 0, 0, 0, u->rot[0], 0, 1.0f);
        w3_cylinder(&w, &x, 14, 0.82f, 0.14f, W3_AUTO);
        float pk = 0.42f + 0.24f * (r ? pack0 : 1.0f - pack0);
        w3_poly(&w, &x, 14, pk, 0.14f, 0.0f, W3_DOT);
        w3_poly(&w, &x, 6, 0.2f, 0.14f, 0.0f, W3_SOLID);
        for (int s = 0; s < 3; s++) {
            float a = (float)s * 2.0943951f;
            w3_line(&w, &x, (v3){ cosf(a) * 0.2f, 0.14f, sinf(a) * 0.2f }, (v3){ cosf(a) * 0.5f, 0.14f, sinf(a) * 0.5f }, W3_SOLID);
        }
    }
    w3_line(&w, NULL, (v3){ -1.1f, 0.14f, 0.82f }, (v3){ 1.1f, 0.14f, 0.82f }, W3_SOLID);   /* the tape, front side */
    w3_line(&w, NULL, (v3){ -1.1f, 0.14f, -0.82f }, (v3){ 1.1f, 0.14f, -0.82f }, W3_DOT);
    int wd = (int)lroundf(sp * 30.0f);                            /* speed bar */
    gfx_rect(g, 29, 57, 30, 3, GFX_ON); gfx_frect(g, 29, 57, wd, 3, GFX_ON);
}

static void scene_lfo(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    float depth = perf_pval(p, PF_LFO, 2), A = 0.35f + 0.85f * depth, ph = v->phase;
    static const char *const DST[4] = { "FILT", "VOL", "PAN", "PTCH" };
    int dst = (int)perf_pval(p, PF_LFO, 1);
    w3_t w; vp(&w, g, 15.0f, 6.0f);
    w3_view(&w, 0.45f, 0.55f + 0.25f * sinf(u->rot[0]), 0.0f);
    enum { N = 24 };
    v3 pa[N + 1], pb[N + 1];
    for (int i = 0; i <= N; i++) {                                  /* a sine ribbon; its phase is the LFO phase, the bead sits at x = 0 */
        float x = -2.4f + 4.8f * (float)i / (float)N, y = A * sinf(2.0f * PI_F * (ph + 0.2f * x));
        pa[i] = (v3){ x, y, 0.7f }; pb[i] = (v3){ x, y, -0.7f };
    }
    for (int i = 0; i < N; i++) {
        w3_line(&w, NULL, pa[i], pa[i + 1], W3_SOLID);
        w3_line(&w, NULL, pb[i], pb[i + 1], W3_AUTO);
        if ((i & 3) == 0) w3_line(&w, NULL, pa[i], pb[i], W3_DOT);
    }
    w3_line(&w, NULL, (v3){ -2.4f, 0, 0 }, (v3){ 2.4f, 0, 0 }, W3_FAINT);
    float by = A * sinf(2.0f * PI_F * ph), r = 0.28f + 0.1f * clampf(v->level * 2.0f, 0.0f, 1.0f);
    w3_xf x = W3_XF(0, by, 0, 1.5707963f, 0, 0, 1.0f);              /* the ring that rides the wave */
    w3_disc(&w, &x, r, 16, W3_SOLID);
    w3_line(&w, NULL, (v3){ 0, by, 0.7f }, (v3){ 0, by, -0.7f }, W3_SOLID);
    gfx_text(g, 11, 56, DST[dst], GFX_ON);
    int bx = 30 + (int)lroundf(v->phase * 38.0f);                   /* position inside the cycle */
    gfx_dline(g, 30, 59, 68, 59, 1, 2, GFX_ON); gfx_frect(g, bx - 1, 57, 3, 3, GFX_ON);
}

static void scene_reverse(gfx_t *g, perf_ui_t *u, const perf_vis_t *v) {
    w3_t w; vp(&w, g, 22.0f, 1.0f);
    w3_view(&w, 0.12f * sinf(u->rot[0] * 0.2f), 0.18f * cosf(u->rot[0] * 0.13f), sinf(u->rot[0] * 0.15f) * 0.5f);
    w.cue_z = 2.2f;
    float lv = 1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.06f;
    w3_xf x = W3_XF(0, 0, 0.2f, 0, 0, 0, lv);
    w3_tunnel(&w, &x, 7, 1.0f, 6.0f, u->rot[0] * 0.35f, W3_AUTO);
}

static void scene_vinyl(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    float cr = perf_pval(p, PF_VINYL, 1);
    w3_t w; vp(&w, g, 16.5f, 6.0f);
    w3_view(&w, 1.0f, 0.0f, 0.0f);
    w3_xf x = W3_XF(-0.25f, 0, 0, 0, u->rot[0], 0, 1.0f);
    w3_disc(&w, &x, 1.45f, 28, W3_AUTO);
    x.t.y = -0.07f; w3_disc(&w, &x, 1.45f, 28, W3_AUTO); x.t.y = 0;
    w3_disc(&w, &x, 1.15f, 24, W3_DOT);
    w3_disc(&w, &x, 0.85f, 20, W3_DOT);
    w3_disc(&w, &x, 0.5f, 14, W3_SOLID);
    w3_line(&w, &x, (v3){ 0, 0, 0.15f }, (v3){ 0, 0, 0.5f }, W3_SOLID);        /* label mark: shows the spin */
    w3_line(&w, &x, (v3){ 1.3f, 0, 0 }, (v3){ 1.45f, 0, 0 }, W3_SOLID);        /* rim notch */
    v3 piv = { 1.9f, 0.1f, -1.0f }, bend = { 1.35f, 0.1f, 0.15f }, ndl = { 0.65f, 0.0f, 0.5f };
    w3_line(&w, NULL, piv, bend, W3_SOLID); w3_line(&w, NULL, bend, ndl, W3_SOLID);   /* tonearm */
    w3_line(&w, NULL, ndl, (v3){ ndl.x, ndl.y + 0.12f, ndl.z }, W3_SOLID);
    float sx, sy;
    if (w3_project(&w, w3_pt(&w, NULL, piv), &sx, &sy)) gfx_circle(g, (int)lroundf(sx), (int)lroundf(sy), 2, GFX_ON);
    if (w3_project(&w, w3_pt(&w, NULL, ndl), &sx, &sy)) {
        int nd = (int)lroundf(cr * 5.0f);                          /* crackle at the needle */
        for (int i = 0; i < nd; i++) gfx_px(g, (int)lroundf(sx) - 3 + (int)(rnd(u) % 7), (int)lroundf(sy) + 2 + (int)(rnd(u) % 3), GFX_ON);
    }
    (void)v;
}

static void scene_filter(gfx_t *g, perf_t *p, const perf_vis_t *v, int hp, float tsec) {
    float c = clampf(v->aux, 0.0f, 1.0f), q = perf_pval(p, v->fx, 1);
    float yh = 52.0f - c * 36.0f;                                 /* horizon follows the cutoff */
    w3_t w; vp(&w, g, 20.0f, 4.0f);
    w3_grid(&w, yh, hp ? -1 : 1, fmodf(tsec * 0.7f, 1.0f), q * 4.0f, tsec * 3.0f);
    gfx_cross(g, 84, (int)lroundf(yh), 1, GFX_ON);
    gfx_text(g, 7, hp ? 56 : 14, perf_pval(p, v->fx, 2) > 0.5f ? (hp ? "HP 24" : "LP 24") : (hp ? "HP 12" : "LP 12"), GFX_ON);
}

static int gate_cells(int idx) { static const int N[6] = { 4, 8, 16, 32, 12, 24 }; return N[idx]; }
static void scene_gate(gfx_t *g, perf_t *p, const perf_vis_t *v, float tsec) {
    int idx = (int)perf_pval(p, PF_GATE, 0), n = gate_cells(idx);
    float depth = perf_pval(p, PF_GATE, 1), swing = perf_pval(p, PF_GATE, 2);
    int cur = (v->rep % n) * 8 / n;                               /* the playhead, mapped onto the 8 cubes */
    float open = clampf(v->speed, 0.0f, 1.0f);
    w3_t w; vp(&w, g, 12.0f, 9.0f);
    w3_view(&w, 0.62f, 0.72f + 0.12f * sinf(tsec * 0.4f), 0.0f);
    for (int i = 0; i < 8; i++) {
        int col = i & 3, row = i >> 2;
        int lit = (((col + (swing > 0.5f ? row : 0)) & 1) == 0) || i == cur;      /* the gate pattern */
        float lift = i == cur ? 0.35f * open : 0.0f, h = lit ? 0.42f : 0.3f;
        w3_xf x = W3_XF(((float)col - 1.5f) * 1.55f, lift, ((float)row - 0.5f) * 1.55f, 0, 0, 0, 1.0f);
        w3_box(&w, &x, h, h, h, lit ? W3_SOLID : (depth < 0.5f ? W3_DOT : W3_DASH));
    }
    gfx_dline(g, 6, 59, 6 + (int)lroundf(clampf(v->level * 2.5f, 0, 1) * 70.0f), 59, 1, 1, GFX_ON);
}

static void scene_delay(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    float fb = perf_pval(p, PF_DELAY, 1);
    double Td = (double)v->phase * 131072.0; if (Td < 2000.0) Td = 2000.0;
    float u = (float)fmod(v->k, Td) / (float)Td;
    w3_t w; vp(&w, g, 16.0f, 3.0f);
    w3_view(&w, 0.0f, 0.0f, 0.0f);
    for (int j = 0; j < 6; j++) {                                 /* one frame per echo, receding up and to the right */
        float gain = powf(0.3f + 0.7f * fb / 0.88f, (float)j), z = ((float)j + u) * 1.35f;
        if (gain < 0.1f) continue;
        w3_xf x = W3_XF(-1.3f + z * 0.5f, -0.75f + z * 0.32f, z, 0, 0, 0, 1.0f);
        w3_square(&w, &x, 0.95f, gain > 0.5f ? W3_SOLID : gain > 0.25f ? W3_DASH : W3_DOT);
    }
    float core = 0.1f + 0.14f * clampf(v->level * 3.0f, 0.0f, 1.0f);
    w3_xf c = W3_XF(-1.3f, -0.75f, -0.1f, 0.4f, 0.6f, 0, 1.0f);
    w3_box(&w, &c, core, core, core, W3_SOLID);
    gfx_text(g, 12, 56, v->held ? "THROW" : "", GFX_ON);
}

static void scene_freeze(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    int idx = (int)perf_pval(p, PF_FREEZE, 0);
    float grow = ease_out((float)(v->k / (0.9 * (double)MARK_SR)));
    float R = (0.8f + 0.1f * (float)idx) * (0.4f + 0.6f * grow);
    w3_t w; vp(&w, g, 27.0f, 6.0f);
    w3_view(&w, 0.4f, u->rot[0] * 0.5f, 0.18f);
    w3_octa(&w, NULL, R * (1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.05f), W3_AUTO);
    w3_xf x = W3_XF(0, 0, 0, 0, -u->rot[0] * 0.9f, 0, 1.0f);
    w3_octa(&w, &x, R * 0.42f, W3_DOT);
}

static void scene_retrig(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    int jj = v->rep & 3;
    (void)p;
    float base = 0.5f + (float)jj * 0.35f;                        /* start pose of this retrigger, then a drift that snaps back */
    float s = 0.85f * (v->phase < 0.12f ? 1.1f : 1.0f);
    w3_t w; vp(&w, g, 12.0f, 6.0f);
    w3_view(&w, 0.5f, base + v->phase * v->phase * 1.5f, 0.0f);
    w3_box(&w, NULL, s, s, s, W3_AUTO);
    w3_view(&w, 0.5f, base, 0.0f);
    w3_box(&w, NULL, 1.2f, 1.2f, 1.2f, W3_FAINT);                 /* the pose it snaps back to */
    for (int i = 0; i < 4; i++) { int x = 30 + i * 7; if (i <= jj) gfx_frect(g, x, 57, 5, 2, GFX_ON); else gfx_rect(g, x, 57, 5, 2, GFX_ON); }
}

static void scene_half(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    int q = (int)perf_pval(p, PF_HALF, 0);
    w3_t w; vp(&w, g, 19.0f, 6.0f);
    w3_view(&w, 0.4f, u->rot[0], 0.38f);
    float lv = 1.0f + clampf(v->level * 2.0f, 0.0f, 1.0f) * 0.05f;
    w3_xf x = W3_XF(0, 0, 0, 0, 0, 0, lv);
    w3_sphere(&w, &x, 1.0f, 3, 4, 20, W3_AUTO);
    w3_view(&w, 0.4f, 0.0f, 0.38f);                               /* the orbit ring, with a slow bead and a normal-speed bead */
    w3_disc(&w, NULL, 1.3f, 32, W3_FAINT);
    for (int b = 0; b < 2; b++) {
        float a = b ? u->rot[1] : u->rot[0], sx, sy;
        if (w3_project(&w, w3_pt(&w, NULL, (v3){ cosf(a) * 1.3f, 0, sinf(a) * 1.3f }), &sx, &sy)) {
            int ix = (int)lroundf(sx), iy = (int)lroundf(sy);
            if (b) gfx_px(g, ix, iy, GFX_ON); else gfx_frect(g, ix - 1, iy - 1, 3, 3, GFX_ON);
        }
    }
    gfx_text(g, 7, 56, q ? "X1/4" : "X1/2", GFX_ON);
}

static void scene_kill(gfx_t *g, perf_t *p, const perf_vis_t *v, float tsec) {
    int m = (int)perf_pval(p, PF_KILL, 0);
    float ks = (float)(v->k / (double)MARK_SR), e = ease_out(ks / 0.35f);
    w3_t w; vp(&w, g, 17.0f, 6.0f);
    w3_view(&w, 0.5f, tsec * 0.6f + e * 1.2f, 0.0f);
    if (m == 0) {                                                  /* ALL: the cube implodes to a point */
        float s = 1.0f - e;
        if (s > 0.02f) w3_box(&w, NULL, s, s, s, W3_AUTO);
        gfx_cross(g, CX, 36, 2, GFX_ON);
    } else {                                                       /* LOW / HIGH: that half collapses flat */
        float hk = 0.04f + (1.0f - e) * 0.96f;
        float hl = m == 1 ? hk : 1.0f, hh = m == 1 ? 1.0f : hk;
        w3_xf lo = W3_XF(0, -hl * 0.5f, 0, 0, 0, 0, 1.0f), hi = W3_XF(0, hh * 0.5f, 0, 0, 0, 0, 1.0f);
        w3_box(&w, &lo, 0.9f, hl * 0.5f, 0.9f, m == 1 ? (hk < 0.1f ? W3_DOT : W3_AUTO) : W3_AUTO);
        w3_box(&w, &hi, 0.9f, hh * 0.5f, 0.9f, m == 2 ? (hk < 0.1f ? W3_DOT : W3_AUTO) : W3_AUTO);
    }
    float ph = fmodf(ks, 0.9f) / 0.9f;                             /* shock ring on the floor */
    w3_view(&w, 0.8f, 0.0f, 0.0f);
    if (ks > 0.25f || m != 0) {
        w3_disc(&w, NULL, 0.35f + ph * 1.6f, 24, ph < 0.5f ? W3_DOT : W3_FAINT);
        float ph2 = fmodf(ph + 0.5f, 1.0f);
        w3_disc(&w, NULL, 0.35f + ph2 * 1.6f, 24, ph2 < 0.5f ? W3_DOT : W3_FAINT);
    }
}

/* small tags for the other stacked effects */
static void others(gfx_t *g, const perf_vis_t *v) {
    for (int i = 0; i < v->nother; i++) {
        int y = 17 + i * 10;
        gfx_frect(g, 2, y, 16, 9, GFX_ON);
        gfx_text_c(g, 10, y + 2, perf_short(v->other[i]), GFX_OFF);
    }
}

static void panel(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    int fx = v->fx, n = perf_nparams(fx), y = 12;
    char t[16];
    s_boxes = 0;
    panel_sep(g);
    for (int k = 0; k < n && k < 3; k++, y += 15) { perf_ptext(p, fx, k, t, sizeof t); box(g, y, k + 1, perf_plabel(fx, k), t); }
    /* purely informative values: plain text, no box (nothing here turns) */
    if (n < 3) {
        if (fx == PF_REVERSE) gfx_text(g, 95, y + 2, "DIR <<", GFX_ON);
        else if (fx == PF_HALF) { snprintf(t, sizeof t, "X%.2f", (double)v->speed); gfx_text(g, 95, y + 2, t, GFX_ON); }
    }
    level_bar(g, v->level);
}

/* ---- the PERFORM grid page: 4 x 4 cells, "NN XXX" each, so every name has room ---- */
static void page_grid(gfx_t *g, perf_t *p, int held, const char *tgt) {
    static const struct { int on, off; } STRIPE[6] = { { 1, 1 }, { 1, 0 }, { 3, 1 }, { 2, 2 }, { 1, 3 }, { 4, 2 } };
    title(g, "PERFORM", tgt);
    unsigned latched = perf_latch_mask(p), on = perf_on_mask(p);
    for (int i = 0; i < PF_NFX; i++) {
        int col = i & 3, row = i >> 2, x = col * 32, y = 10 + row * 13, w = col == 3 ? 32 : 33, h = 13;   /* boxes share their borders */
        char num[4]; snprintf(num, sizeof num, "%d", i + 1);
        int hot = i == held;
        if (hot) gfx_frect(g, x, y, w, h, GFX_ON); else gfx_rect(g, x, y, w, h, GFX_ON);
        int c = hot ? GFX_OFF : GFX_ON, fam = perf_family(i);
        gfx_text(g, x + 3 + (i < 9 ? 5 : 0), y + 3, num, c);
        gfx_text(g, x + 14, y + 3, perf_short(i), c);
        gfx_dline(g, x + 3, y + 9, x + w - 4, y + 9, STRIPE[fam].on, STRIPE[fam].off, c);
        if ((latched >> i) & 1u) { gfx_frect(g, x + w - 7, y + 2, 5, 5, c); gfx_px(g, x + w - 6, y + 4, !c); gfx_px(g, x + w - 5, y + 5, !c); gfx_px(g, x + w - 4, y + 3, !c); }   /* latched: a flag with a check */
        else if ((on >> i) & 1u) gfx_frect(g, x + w - 5, y + 3, 2, 2, c);
    }
}

/* the scene shows while a step is down, and in PERFORM mode as long as any effect is on */
static int ui_engaged(perf_t *p, const perf_vis_t *v, int mode) { return v->fx >= 0 && (perf_held_count(p) > 0 || mode); }

int perf_ui_draw(perf_ui_t *u, gfx_t *g, perf_t *p, int shift, int target, long clock_frames, int mode) {
    perf_vis_t v;
    perf_vis(p, &v);
    s_boxes = 0;
    char tgt[12];
    float dt = u->last_frames ? (float)(clock_frames - u->last_frames) / (float)MARK_SR : 0.0f;
    if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
    float tsec = (float)clock_frames / (float)MARK_SR;
    int engaged = ui_engaged(p, &v, mode);
    if (!engaged && !shift) return 0;
    gfx_clear(g, GFX_OFF);
    if (!engaged) {
        if (target) snprintf(tgt, sizeof tgt, "PISTA %d", target); else snprintf(tgt, sizeof tgt, "MIX");
        page_grid(g, p, -1, tgt); return 1;
    }
    if (v.target) snprintf(tgt, sizeof tgt, "PISTA %d", v.target); else snprintf(tgt, sizeof tgt, "MIX");
    if (shift && v.held && v.k < 0.15 * (double)MARK_SR) { page_grid(g, p, v.fx, tgt); return 1; }   /* the pressed cell flashes inverted */
    float spd = v.speed;
    if (v.fx == PF_TSTOP) u->rot[0] += dt * clampf(spd, 0, 1) * 7.0f;
    else if (v.fx == PF_VINYL) u->rot[0] += dt * spd * 3.5f;
    else if (v.fx == PF_LFO) u->rot[0] += dt * 0.5f;
    else if (v.fx == PF_REVERSE) u->rot[0] -= dt * 2.4f;
    else if (v.fx == PF_PITCH) u->rot[0] += dt * 0.9f * powf(2.0f, (perf_pval(p, PF_PITCH, 0) + perf_pval(p, PF_PITCH, 1) / 100.0f) / 12.0f);
    else if (v.fx == PF_STUTTER) u->rot[1] += dt * (1.2f + 5.0f * clampf((float)(v.k / (perf_pval(p, PF_STUTTER, 0) * v.beat)), 0.0f, 1.0f));
    else if (v.fx == PF_FREEZE) u->rot[0] += dt * 1.0f;
    else if (v.fx == PF_HALF) { u->rot[0] += dt * spd * 1.6f; u->rot[1] += dt * 1.6f; }
    frame(g);
    title(g, perf_name(v.fx), tgt);
    switch (v.fx) {
    case PF_BEATREP: scene_beatrep(g, p, &v, tsec); break;
    case PF_STUTTER: scene_stutter(g, u, p, &v, tsec); break;
    case PF_PITCH:   scene_pitch(g, u, p, &v); break;
    case PF_CRUSH:   scene_crush(g, p, &v, tsec); break;
    case PF_TSTOP:   scene_tape(g, u, &v); break;
    case PF_LFO:     scene_lfo(g, u, p, &v); break;
    case PF_REVERSE: scene_reverse(g, u, &v); break;
    case PF_VINYL:   scene_vinyl(g, u, p, &v); break;
    case PF_LP: case PF_HP: scene_filter(g, p, &v, v.fx == PF_HP, tsec); break;
    case PF_GATE:    scene_gate(g, p, &v, tsec); break;
    case PF_DELAY:   scene_delay(g, p, &v); break;
    case PF_FREEZE:  scene_freeze(g, u, p, &v); break;
    case PF_RETRIG:  scene_retrig(g, p, &v); break;
    case PF_HALF:    scene_half(g, u, p, &v); break;
    case PF_KILL:    scene_kill(g, p, &v, tsec); break;
    default: break;
    }
    others(g, &v);
    if (v.latched) gfx_text(g, 124 - gfx_text_w(tgt) - 7 - gfx_text_w("ON"), 2, "ON", GFX_OFF);
    panel(g, p, &v);
    return 1;
}

int perf_ui_param(perf_ui_t *u, perf_t *p, int shift, int target, long clock_frames, int mode, char *out, int n) {
    static char hex[GFX_HEX_LEN + 1];
    if (u->shown && u->have && clock_frames - u->last_frames < (long)(MARK_SR / PUI_FPS) && clock_frames >= u->last_frames) {
        /* inside the frame period: repeat the last answer unless the state flipped to "nothing" */
        perf_vis_t v; perf_vis(p, &v);
        if (ui_engaged(p, &v, mode) || shift) {
            gfx_hex(&u->prev, hex);
            return snprintf(out, (size_t)n, "%u|%s", u->seq, hex);
        }
    }
    if (!perf_ui_draw(u, &u->g, p, shift, target, clock_frames, mode)) {
        u->shown = 0; u->have = 0;
        if (n > 0) out[0] = 0;
        return 0;
    }
    if (!u->have || memcmp(u->g.b, u->prev.b, sizeof u->g.b)) { u->seq = (u->seq + 1) % 100000u; u->prev = u->g; }
    u->have = 1; u->shown = 1; u->last_frames = clock_frames;
    gfx_hex(&u->prev, hex);
    return snprintf(out, (size_t)n, "%u|%s", u->seq, hex);
}
