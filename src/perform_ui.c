#include <math.h>
#include <stdio.h>
#include <string.h>
#include "mark_core.h"
#include "perform_ui.h"

#define CX 44
#define CY 36
#define PI_F 3.14159265f

static void polar(int cx, int cy, float r, float a, int *x, int *y) {
    *x = cx + (int)lroundf(cosf(a) * r); *y = cy + (int)lroundf(sinf(a) * r);
}
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
static void box(gfx_t *g, int y, const char *lab, const char *val) {
    gfx_rect(g, 92, y, 33, 14, GFX_ON);
    gfx_text(g, 95, y + 2, lab, GFX_ON);
    gfx_text(g, 122 - gfx_text_w(val), y + 8, val, GFX_ON);
}
static void level_bar(gfx_t *g, float lvl) {
    gfx_rect(g, 92, 56, 33, 4, GFX_ON);
    gfx_frect(g, 93, 57, (int)lroundf(clampf(lvl * 2.0f, 0.0f, 1.0f) * 31.0f), 2, GFX_ON);
}
/* tick ring + level gauge: the common radar furniture */
static void radar_base(gfx_t *g, float lvl, int r) {
    for (int i = 0; i < 24; i++) {
        int x0, y0, x1, y1;
        float a = (float)i * PI_F / 12.0f, l = i % 6 == 0 ? 4.0f : 2.0f;
        polar(CX, CY, (float)r, a, &x0, &y0); polar(CX, CY, (float)r + l, a, &x1, &y1);
        gfx_line(g, x0, y0, x1, y1, GFX_ON);
    }
    float sw = clampf(lvl * 2.5f, 0.0f, 1.0f) * 90.0f;
    gfx_arc(g, CX, CY, 25, 270.0f - sw, 270.0f + sw, GFX_ON);
    gfx_dline(g, 8, CY, CX - r - 6, CY, 1, 2, GFX_ON);
    gfx_dline(g, CX + r + 6, CY, 81, CY, 1, 2, GFX_ON);
}

/* ---- scenes ---- */
static void scene_beatrep(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    static const int NR[6] = { 2, 3, 4, 5, 3, 4 };
    int idx = (int)perf_pval(p, PF_BEATREP, 0), n = NR[idx];
    float decay = perf_pval(p, PF_BEATREP, 1);
    float gain = powf(decay, (float)(v->rep > 40 ? 40 : v->rep));
    float ping = 3.0f + v->phase * 15.0f;
    for (int i = 0; i < n; i++) {
        int r = 3 + (i + 1) * 15 / n;
        if ((float)r <= ping) gfx_circle(g, CX, CY, r, GFX_ON); else gfx_dcircle(g, CX, CY, r, 14, GFX_ON);
    }
    gfx_circle(g, CX, CY, (int)lroundf(ping), GFX_ON);
    int rd = 1 + (int)lroundf(4.0f * gain * (1.0f - v->phase));
    for (int r = 0; r <= rd; r++) gfx_circle(g, CX, CY, r, GFX_ON);
    radar_base(g, v->level, 20);
    int cnt = v->rep + 1 > 8 ? 8 : v->rep + 1;                   /* repeat counter */
    for (int i = 0; i < 8; i++) {
        if (i < cnt) gfx_frect(g, 14 + i * 8, 58, 5, 3, GFX_ON); else gfx_rect(g, 14 + i * 8, 58, 5, 3, GFX_ON);
    }
}

static void scene_stutter(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    float beats = perf_pval(p, PF_STUTTER, 0);
    float u = clampf((float)(v->k / (beats * v->beat)), 0.0f, 1.0f);
    float q = 0.9f - 0.3f * u;
    int act = v->rep % 10;
    gfx_dline(g, 4, CY, 85, CY, 1, 2, GFX_ON);
    for (int j = 0; j < 10; j++) {
        float d = 38.0f * powf(q, (float)j);
        if (d < 3.0f && j != act) continue;
        int h = (int)lroundf(10.0f * powf(0.9f, (float)j)) + 2;
        int on = j == act;
        if (on) h += (int)lroundf(4.0f * (1.0f - v->phase));
        for (int s = -1; s <= 1; s += 2) {
            int x = CX + s * (int)lroundf(d);
            gfx_line(g, x, CY - h, x, CY + h, GFX_ON);
            if (on) gfx_line(g, x + s, CY - h, x + s, CY + h, GFX_ON);
        }
    }
    gfx_arc(g, CX, CY, 24, 150.0f, 210.0f, GFX_ON);
    gfx_arc(g, CX, CY, 24, -30.0f, 30.0f, GFX_ON);
    gfx_arc(g, CX, CY, 25, 270.0f - clampf(v->level * 2.5f, 0, 1) * 60.0f, 270.0f + clampf(v->level * 2.5f, 0, 1) * 60.0f, GFX_ON);
    gfx_dcircle(g, CX, CY, 8, 25, GFX_ON);
    gfx_frect(g, CX - 1, CY - 1, 3, 3, GFX_ON);
    for (int i = 0; i < 10; i++)                                 /* progress ticks */
        if (u * 10.0f > (float)i) gfx_frect(g, 14 + i * 6, 58, 4, 3, GFX_ON); else gfx_rect(g, 14 + i * 6, 58, 4, 3, GFX_ON);
}

static void scene_pitch(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    int st = (int)perf_pval(p, PF_PITCH, 0), cx = CX, cy = 54;
    const float R = 36.0f;
    for (int i = -12; i <= 12; i++) {
        float a = PI_F + (float)(i + 12) / 24.0f * PI_F;
        int x0, y0, x1, y1;
        float l = i == 0 ? 8.0f : i % 6 == 0 ? 6.0f : 3.0f;
        polar(cx, cy, R, a, &x0, &y0); polar(cx, cy, R - l, a, &x1, &y1);
        gfx_line(g, x0, y0, x1, y1, GFX_ON);
    }
    gfx_arc(g, cx, cy, (int)R, 180.0f, 360.0f, GFX_ON);
    gfx_darc(g, cx, cy, 22, 180.0f, 360.0f, 6.0f, GFX_ON);
    gfx_darc(g, cx, cy, 14, 180.0f, 360.0f, 4.0f, GFX_ON);
    float a = PI_F + (float)(st + 12) / 24.0f * PI_F;
    int x, y; polar(cx, cy, R - 10.0f + clampf(v->level * 4.0f, 0, 1) * 6.0f, a, &x, &y);
    int tx, ty; polar(cx, cy, 20.0f, PI_F * 1.5f, &tx, &ty);
    gfx_dline(g, cx, cy, tx, ty - 14, 1, 2, GFX_ON);               /* the 0 reference */
    gfx_line(g, cx, cy, x, y, GFX_ON);
    int x2, y2; polar(cx, cy, 6.0f, a + 0.25f, &x2, &y2); gfx_line(g, cx, cy, x2, y2, GFX_ON);
    polar(cx, cy, 6.0f, a - 0.25f, &x2, &y2); gfx_line(g, cx, cy, x2, y2, GFX_ON);
    gfx_circle(g, cx, cy, 3, GFX_ON);
    gfx_text(g, 9, 56, "-12", GFX_ON);
    gfx_text(g, 80 - gfx_text_w("+12"), 56, "+12", GFX_ON);
    gfx_text_c(g, cx, 11, "0", GFX_ON);
}

static void scene_crush(gfx_t *g, perf_t *p, const perf_vis_t *v, float tsec) {
    float bits = perf_pval(p, PF_CRUSH, 0), ds = perf_pval(p, PF_CRUSH, 1);
    int b = (int)lroundf(1.0f + (16.0f - bits) * 0.45f + (ds - 1.0f) * 0.14f);
    b = b < 1 ? 1 : b > 8 ? 8 : b;
    if (b == 1) { gfx_circle(g, CX, CY, 22, GFX_ON); gfx_circle(g, CX, CY, 20, GFX_ON); }
    else {
        int bw = b - 1;
        for (int by = -24; by < 24; by += b)
            for (int bx = -24; bx < 24; bx += b) {
                float d = sqrtf((float)((bx + b / 2) * (bx + b / 2) + (by + b / 2) * (by + b / 2)));
                if (d >= 15.0f && d <= 22.0f) gfx_frect(g, CX + bx, CY + by, b >= 4 ? bw : b, b >= 4 ? bw : b, GFX_ON);
            }
    }
    int lv = 1 << (int)bits; if (lv > 24) lv = 24;
    int hold = (int)lroundf(ds / 4.0f); hold = hold < 1 ? 1 : hold > 8 ? 8 : hold;
    float amp = 4.0f + 5.0f * clampf(v->level * 3.0f, 0, 1);
    int py = CY;
    for (int x = -16; x <= 16; x++) {
        int xs = ((x + 16) / hold) * hold;
        float s = sinf((float)xs / 32.0f * 2.0f * PI_F * 1.5f + tsec * 4.0f);
        float qv = lv > 1 ? roundf((s * 0.5f + 0.5f) * (float)(lv - 1)) / (float)(lv - 1) * 2.0f - 1.0f : 0.0f;
        int y = CY - (int)lroundf(qv * amp);
        if (x > -16) gfx_line(g, CX + x - 1, py, CX + x, y, GFX_ON);
        py = y;
    }
    gfx_dline(g, 8, CY, 18, CY, 1, 2, GFX_ON); gfx_dline(g, 70, CY, 81, CY, 1, 2, GFX_ON);
    float sw = clampf(v->level * 2.5f, 0, 1) * 60.0f;
    gfx_arc(g, CX, CY, 25, 270.0f - sw, 270.0f + sw, GFX_ON);
}

static void scene_tape(gfx_t *g, perf_ui_t *u, const perf_vis_t *v) {
    static const int RX[2] = { 24, 64 };
    float pack0 = 0.5f + 0.5f * (v->speed < 0 ? 0.0f : v->speed);
    for (int r = 0; r < 2; r++) {
        int cx = RX[r], cy = 34;
        gfx_circle(g, cx, cy, 15, GFX_ON);
        gfx_dcircle(g, cx, cy, (int)lroundf(8.0f + 4.0f * (r ? pack0 : 1.0f - pack0)), 24, GFX_ON);
        gfx_circle(g, cx, cy, 3, GFX_ON);
        for (int k = 0; k < 3; k++) {
            int x0, y0, x1, y1;
            float a = u->rot[0] + (float)k * 2.0943951f;
            polar(cx, cy, 4.0f, a, &x0, &y0); polar(cx, cy, 13.0f, a, &x1, &y1);
            gfx_line(g, x0, y0, x1, y1, GFX_ON);
            polar(cx, cy, 13.0f, a + 0.28f, &x0, &y0); gfx_line(g, x1, y1, x0, y0, GFX_ON);
        }
    }
    gfx_line(g, 24, 49, 34, 57, GFX_ON); gfx_line(g, 34, 57, 54, 57, GFX_ON); gfx_line(g, 54, 57, 64, 49, GFX_ON);
    gfx_circle(g, 34, 57, 2, GFX_ON); gfx_circle(g, 54, 57, 2, GFX_ON);
    gfx_dline(g, 24, 49, 64, 49, 1, 3, GFX_ON);
    gfx_dline(g, 44, 18, 44, 30, 1, 2, GFX_ON);
    int w = (int)lroundf(clampf(v->speed, 0, 1) * 30.0f);          /* speed bar */
    gfx_rect(g, 29, 14, 30, 3, GFX_ON); gfx_frect(g, 29, 14, w, 3, GFX_ON);
    float sw = clampf(v->level * 2.5f, 0, 1) * 25.0f;
    gfx_dline(g, 44 - (int)sw, 12, 44 + (int)sw, 12, 1, 1, GFX_ON);
}

static void scene_reverse(gfx_t *g, perf_ui_t *u, const perf_vis_t *v) {
    float rot = u->rot[0];
    int px = 0, py = 0;
    for (float th = 0.2f; th <= 4.0f * PI_F; th += 0.12f) {
        int x, y; polar(CX, CY, 2.0f + 17.0f * th / (4.0f * PI_F), th + rot, &x, &y);
        if (th > 0.2f) gfx_line(g, px, py, x, y, GFX_ON);
        px = x; py = y;
    }
    for (int k = 0; k < 3; k++) {                                /* arrowheads, pointing inward */
        float th = 1.2f + (float)k * 3.3f + 0.4f;
        int x, y, x1, y1; polar(CX, CY, 2.0f + 17.0f * th / (4.0f * PI_F), th + rot, &x, &y);
        polar(CX, CY, 2.0f + 17.0f * (th - 0.35f) / (4.0f * PI_F), th - 0.35f + rot, &x1, &y1);
        float ang = atan2f((float)(y1 - y), (float)(x1 - x)), c = 4.0f;
        int xa, ya; xa = x + (int)lroundf(cosf(ang + 0.6f) * c); ya = y + (int)lroundf(sinf(ang + 0.6f) * c); gfx_line(g, x, y, xa, ya, GFX_ON);
        xa = x + (int)lroundf(cosf(ang - 0.6f) * c); ya = y + (int)lroundf(sinf(ang - 0.6f) * c); gfx_line(g, x, y, xa, ya, GFX_ON);
    }
    radar_base(g, v->level, 20);
}

static void scene_vinyl(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    float cr = perf_pval(p, PF_VINYL, 1);
    gfx_circle(g, CX, CY, 22, GFX_ON);
    for (int r = 9; r <= 19; r += 3) { if (r % 2) gfx_dcircle(g, CX, CY, r, 16, GFX_ON); else gfx_circle(g, CX, CY, r, GFX_ON); }
    gfx_circle(g, CX, CY, 7, GFX_ON); gfx_circle(g, CX, CY, 1, GFX_ON);
    int x0, y0, x1, y1;
    polar(CX, CY, 2.0f, u->rot[0], &x0, &y0); polar(CX, CY, 6.0f, u->rot[0], &x1, &y1); gfx_line(g, x0, y0, x1, y1, GFX_ON);
    polar(CX, CY, 9.0f, u->rot[0] + PI_F, &x0, &y0); polar(CX, CY, 19.0f, u->rot[0] + PI_F, &x1, &y1); gfx_dline(g, x0, y0, x1, y1, 1, 2, GFX_ON);
    gfx_circle(g, 77, 17, 3, GFX_ON);                            /* tone arm */
    gfx_line(g, 77, 17, 60, 46, GFX_ON); gfx_line(g, 60, 46, 56, 45, GFX_ON); gfx_line(g, 60, 46, 62, 49, GFX_ON);
    int nd = (int)lroundf(cr * 8.0f);                            /* crackle */
    for (int i = 0; i < nd; i++) {
        int r = 9 + (int)(rnd(u) % 11); float a = (float)(rnd(u) % 628) / 100.0f;
        polar(CX, CY, (float)r, a, &x0, &y0); gfx_px(g, x0, y0, GFX_XOR); gfx_px(g, x0 + 1, y0, GFX_XOR);
    }
    float sw = clampf(v->level * 2.5f, 0, 1) * 60.0f;
    gfx_arc(g, CX, CY, 25, 270.0f - sw, 270.0f + sw, GFX_ON);
}

/* ---- batch 2 scenes ---- */
static void scene_filter(gfx_t *g, perf_t *p, const perf_vis_t *v, int hp, float tsec) {
    float c = clampf(v->aux, 0.0f, 1.0f), q = perf_pval(p, v->fx, 1);
    int y = CY + 19 - (int)lroundf(c * 38.0f);
    int cy = CY;
    gfx_circle(g, CX, cy, 17, GFX_ON); gfx_dcircle(g, CX, cy, 10, 20, GFX_ON); gfx_circle(g, CX, cy, 2, GFX_ON);
    gfx_dline(g, CX, 12, CX, 61, 1, 3, GFX_ON);                  /* crosshair */
    gfx_dline(g, 6, cy, 82, cy, 1, 3, GFX_ON);
    int y0 = hp ? y + 3 : 12, y1 = hp ? 60 : y - 3;              /* the side that is being cut is hatched */
    for (int yy = y0; yy <= y1; yy += 3) gfx_dline(g, 9 + ((yy / 3) & 1) * 2, yy, 80, yy, 1, 3, GFX_ON);
    gfx_line(g, 6, y, 82, y, GFX_ON);                            /* the cut line */
    gfx_line(g, 6, y - 1, 82, y - 1, GFX_ON);
    int bump = 2 + (int)lroundf(q * 9.0f * (0.85f + 0.15f * sinf(tsec * 9.0f)));   /* resonance peak on the line */
    int dir = hp ? 1 : -1;
    gfx_line(g, CX - 12, y, CX, y + dir * bump, GFX_ON); gfx_line(g, CX, y + dir * bump, CX + 12, y, GFX_ON);
    gfx_cross(g, CX, y, 3, GFX_OFF); gfx_cross(g, CX, y, 2, GFX_ON);
    gfx_arc(g, CX, cy, 26, 215.0f, 325.0f, GFX_ON);              /* frequency arc */
    for (int i = 0; i <= 8; i++) {
        int x0, y0b, x1, y1b; float a = (215.0f + (float)i * 13.75f) * PI_F / 180.0f;
        polar(CX, cy, 26.0f, a, &x0, &y0b); polar(CX, cy, 29.0f, a, &x1, &y1b); gfx_line(g, x0, y0b, x1, y1b, GFX_ON);
    }
    float a = (215.0f + c * 110.0f) * PI_F / 180.0f; int mx, my;
    polar(CX, cy, 26.0f, a, &mx, &my); gfx_frect(g, mx - 1, my - 1, 3, 3, GFX_ON); gfx_circle(g, mx, my, 3, GFX_ON);
    gfx_text(g, 7, 56, hp ? "HP" : "LP", GFX_ON);
    float sw = clampf(v->level * 2.5f, 0, 1) * 8.0f;             /* level: ticks on the right edge */
    gfx_dline(g, 83, 60, 83, 60 - (int)sw * 3, 1, 1, GFX_ON);
}

static int gate_cells(int idx) { static const int N[6] = { 4, 8, 16, 32, 12, 24 }; return N[idx]; }
static void scene_gate(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    int idx = (int)perf_pval(p, PF_GATE, 0), n = gate_cells(idx);
    float depth = perf_pval(p, PF_GATE, 1), swing = perf_pval(p, PF_GATE, 2);
    int cols = n <= 8 ? 4 : n == 12 ? 6 : 8, rows = (n + cols - 1) / cols;
    if (n == 4) cols = 4;
    int aw = 76, ah = 42, cw = aw / cols, ch = ah / rows;
    int cur = v->rep % n;
    for (int i = 0; i < n; i++) {
        int cx = 6 + (i % cols) * cw, cy = 13 + (i / cols) * ch, w = cw - 1, h = ch - 1;
        gfx_rect(g, cx, cy, w, h, GFX_ON);
        float o = (i & 1) ? swing * 0.4f : 0.0f;                  /* the open half of this cell */
        int ox = cx + 1 + (int)lroundf(o * (float)(w - 2)), ow = (int)lroundf(0.5f * (float)(w - 2));
        if (ox + ow > cx + w - 1) ow = cx + w - 1 - ox;
        int on = i == cur;
        gfx_frect(g, ox, cy + 1, ow, h - 2, GFX_ON);
        if (depth < 0.95f && w > 5) gfx_dline(g, ox + ow, cy + h / 2, cx + w - 2, cy + h / 2, 1, depth < 0.5f ? 1 : 3, GFX_ON);   /* what leaks through the closed half */
        if (on) {                                                   /* playhead cell: bracketed and its phase tick */
            gfx_rect(g, cx - 1, cy - 1, w + 2, h + 2, GFX_ON);
            int tx = cx + 1 + (int)lroundf(v->phase * (float)(w - 3));
            gfx_line(g, tx, cy - 2, tx, cy + h + 1, GFX_XOR);
        }
    }
    gfx_dline(g, 6, 57, 6 + (int)lroundf(clampf(v->speed, 0, 1) * 70.0f), 57, 1, 1, GFX_ON);   /* gate gain */
    gfx_dline(g, 6, 59, 6 + (int)lroundf(clampf(v->level * 2.5f, 0, 1) * 70.0f), 59, 1, 1, GFX_ON);
}

static void scene_delay(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    float fb = perf_pval(p, PF_DELAY, 1);
    double Td = (double)v->phase * 131072.0; if (Td < 2000.0) Td = 2000.0;
    float u = (float)fmod(v->k, Td) / (float)Td;
    for (int j = 0; j < 6; j++) {
        float gain = powf(0.3f + 0.7f * fb / 0.88f, (float)j);
        int r = 3 + (int)lroundf(((float)j + u) * 5.0f);
        if (r > 25 || gain < 0.1f) continue;
        if (gain > 0.5f) { gfx_circle(g, CX, CY, r, GFX_ON); if (j == 0) gfx_circle(g, CX, CY, r + 1, GFX_ON); }
        else if (gain > 0.25f) gfx_dcircle(g, CX, CY, r, 18, GFX_ON);
        else gfx_dcircle(g, CX, CY, r, 7, GFX_ON);
    }
    int core = 2 + (int)lroundf(clampf(v->level * 3.0f, 0, 1) * 3.0f);          /* the thrown sound at the centre */
    for (int r = 0; r <= core; r++) gfx_circle(g, CX, CY, r, GFX_ON);
    gfx_cross(g, CX, CY, 28, GFX_OFF);
    gfx_dline(g, 8, CY, 14, CY, 1, 1, GFX_ON); gfx_dline(g, 74, CY, 81, CY, 1, 1, GFX_ON);
    gfx_dline(g, CX, 12, CX, 16, 1, 1, GFX_ON); gfx_dline(g, CX, 56, CX, 60, 1, 1, GFX_ON);
    gfx_text(g, 12, 56, v->held ? "THROW" : "", GFX_ON);
}

static void scene_freeze(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    int idx = (int)perf_pval(p, PF_FREEZE, 0);
    float grow = clampf((float)(v->k / (0.9 * (double)MARK_SR)), 0.0f, 1.0f);
    grow = 1.0f - (1.0f - grow) * (1.0f - grow);
    float R = 4.0f + 17.0f * grow, rot = u->rot[0] * 0.15f;
    int nb = 1 + (idx + 1) / 2;                                   /* branches per arm: more for a bigger slice */
    for (int a = 0; a < 6; a++) {
        float ang = rot + (float)a * PI_F / 3.0f;
        int x0, y0, x1, y1;
        polar(CX, CY, 2.0f, ang, &x0, &y0); polar(CX, CY, R, ang, &x1, &y1);
        gfx_line(g, x0, y0, x1, y1, GFX_ON);
        for (int b = 1; b <= nb; b++) {
            float rb = R * (float)b / (float)(nb + 1) + 2.0f, tw = (R - rb) * 0.55f + 1.0f;
            int bx, by, tx, ty;
            polar(CX, CY, rb, ang, &bx, &by);
            polar(bx, by, tw, ang + 0.9f, &tx, &ty); gfx_line(g, bx, by, tx, ty, GFX_ON);
            polar(bx, by, tw, ang - 0.9f, &tx, &ty); gfx_line(g, bx, by, tx, ty, GFX_ON);
        }
        int dx, dy; polar(CX, CY, R + 2.0f, ang, &dx, &dy); gfx_cross(g, dx, dy, 1, GFX_ON);
    }
    float h = 5.0f + 3.0f * grow;                                  /* the hexagon core */
    for (int a = 0; a < 6; a++) {
        int x0, y0, x1, y1;
        polar(CX, CY, h, rot + (float)a * PI_F / 3.0f + PI_F / 6.0f, &x0, &y0); polar(CX, CY, h, rot + (float)(a + 1) * PI_F / 3.0f + PI_F / 6.0f, &x1, &y1);
        gfx_line(g, x0, y0, x1, y1, GFX_ON);
    }
    gfx_dcircle(g, CX, CY, 23, 12, GFX_ON);
    float sw = clampf(v->level * 2.5f, 0, 1) * 40.0f;
    gfx_arc(g, CX, CY, 25, 270.0f - sw, 270.0f + sw, GFX_ON);
}

static void scene_retrig(gfx_t *g, perf_t *p, const perf_vis_t *v) {
    int idx = (int)perf_pval(p, PF_RETRIG, 0), jj = v->rep & 3;
    (void)idx;
    gfx_circle(g, CX, CY, 22, GFX_ON); gfx_circle(g, CX, CY, 17, GFX_ON);
    for (int i = 0; i < 16; i++) {                                  /* 16 ticks, a heavier one every quarter */
        int x0, y0, x1, y1; float a = ((float)i * 22.5f - 90.0f) * PI_F / 180.0f;
        polar(CX, CY, 22.0f, a, &x0, &y0); polar(CX, CY, i % 4 == 0 ? 27.0f : 25.0f, a, &x1, &y1); gfx_line(g, x0, y0, x1, y1, GFX_ON);
    }
    float live = ((float)jj + v->phase) * 90.0f - 90.0f, rd = v->phase * 90.0f - 90.0f;   /* live head, and where we are reading */
    gfx_darc(g, CX, CY, 19, -90.0f, rd, 0.0f, GFX_ON);
    int lx, ly, rx, ry; polar(CX, CY, 19.5f, live * PI_F / 180.0f, &lx, &ly); polar(CX, CY, 19.5f, rd * PI_F / 180.0f, &rx, &ry);
    gfx_dline(g, lx, ly, rx, ry, 1, 2, GFX_ON);
    for (int r = 0; r <= 2; r++) gfx_circle(g, rx, ry, r, GFX_ON);   /* read head */
    gfx_circle(g, lx, ly, 3, GFX_ON);                                /* live head */
    for (int k = 0; k < 4; k++) {                                    /* rewind chevrons <<, they run backwards round the ring */
        float a = (-(float)k * 90.0f - (float)jj * 22.5f - v->phase * 22.5f - 45.0f) * PI_F / 180.0f;
        int x, y; polar(CX, CY, 11.0f, a, &x, &y);
        float tx = sinf(a), ty = -cosf(a), nx = cosf(a), ny = sinf(a);   /* counter-clockwise tangent and the outward normal */
        int ax = x + (int)lroundf(tx * 4.0f), ay = y + (int)lroundf(ty * 4.0f);
        for (int sgn = -1; sgn <= 1; sgn += 2)
            gfx_line(g, ax, ay, x - (int)lroundf(tx * 3.0f) + (int)lroundf(nx * 3.0f * (float)sgn), y - (int)lroundf(ty * 3.0f) + (int)lroundf(ny * 3.0f * (float)sgn), GFX_ON);
    }
    gfx_frect(g, CX - 1, CY - 1, 3, 3, GFX_ON);
    for (int i = 0; i < 4; i++) { if (i <= jj) gfx_frect(g, 30 + i * 6, 58, 4, 3, GFX_ON); else gfx_rect(g, 30 + i * 6, 58, 4, 3, GFX_ON); }
}

static void scene_half(gfx_t *g, perf_ui_t *u, perf_t *p, const perf_vis_t *v) {
    int q = (int)perf_pval(p, PF_HALF, 0);
    (void)v;
    gfx_dcircle(g, CX, CY, 22, 16, GFX_ON);                         /* slow orbit and the fast ghost orbit */
    gfx_dcircle(g, CX, CY, 11, 40, GFX_ON);
    gfx_circle(g, CX, CY, 3, GFX_ON); gfx_circle(g, CX, CY, 1, GFX_ON);
    float a = u->rot[0];
    for (int t = 0; t < 7; t++) {                                    /* the body with a fading trail */
        int x, y; polar(CX, CY, 22.0f, a - (float)t * 0.11f, &x, &y);
        if (t == 0) { for (int r = 0; r <= 3; r++) gfx_circle(g, x, y, r, GFX_ON); }
        else if (t < 4 || (t & 1)) gfx_px(g, x, y, GFX_ON);
    }
    int gx, gy; polar(CX, CY, 11.0f, u->rot[1], &gx, &gy); gfx_circle(g, gx, gy, 1, GFX_ON);   /* normal-speed reference */
    for (int i = 0; i < 12; i++) {                                   /* clock ticks */
        int x0, y0, x1, y1; float t = (float)i * PI_F / 6.0f;
        polar(CX, CY, 26.0f, t, &x0, &y0); polar(CX, CY, i % 3 == 0 ? 29.0f : 27.0f, t, &x1, &y1); gfx_line(g, x0, y0, x1, y1, GFX_ON);
    }
    gfx_text(g, 9, 56, q ? "X1/4" : "X1/2", GFX_ON);
    float sw = clampf(v->level * 2.5f, 0, 1) * 50.0f;
    gfx_arc(g, CX, CY, 16, 270.0f - sw, 270.0f + sw, GFX_ON);
}

static void scene_kill(gfx_t *g, perf_t *p, const perf_vis_t *v, float tsec) {
    int m = (int)perf_pval(p, PF_KILL, 0), blink = ((int)(tsec * 5.0f)) & 1;
    for (int i = 0; i < 16; i += 2) {                               /* hazard stripes */
        gfx_line(g, 6 + i * 5, 12, 16 + i * 5, 16, GFX_ON); gfx_line(g, 6 + i * 5, 59, 16 + i * 5, 55, GFX_ON);
    }
    if (m == 0) {
        gfx_rect(g, 13, 18, 62, 34, GFX_ON);
        for (int d = -1; d <= 1; d++) { gfx_line(g, 18 + d, 21, 70 + d, 49, GFX_ON); gfx_line(g, 70 + d, 21, 18 + d, 49, GFX_ON); }
        if (blink) gfx_rect(g, 11, 16, 66, 38, GFX_ON);
    } else {                                                         /* the spectrum, with the killed side crossed out */
        for (int i = 0; i < 12; i++) {
            int h = 6 + (int)((i * 7 + (int)(tsec * 6.0f) * 5 + (i & 1) * 9) % 20) + (int)lroundf(v->level * 8.0f);
            int x = 14 + i * 5, killed = m == 1 ? i < 4 : i >= 8;
            if (!killed) gfx_frect(g, x, 52 - h, 4, h, GFX_ON);
            else { gfx_dline(g, x, 52, x, 52 - h, 1, 3, GFX_ON); gfx_dline(g, x + 3, 52, x + 3, 52 - h, 1, 3, GFX_ON); }
        }
        int x0 = m == 1 ? 12 : 54, x1 = x0 + 21;
        gfx_rect(g, x0, 18, 22, 35, GFX_ON);
        gfx_line(g, x0, 18, x1, 52, GFX_ON); gfx_line(g, x1, 18, x0, 52, GFX_ON);
        if (blink) gfx_rect(g, x0 - 2, 16, 26, 39, GFX_ON);
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
    int fx = v->fx, n = perf_nparams(fx), y = 12, shown = 0;
    char t[16];
    panel_sep(g);
    for (int k = 0; k < n && shown < 3; k++, shown++, y += 15) { perf_ptext(p, fx, k, t, sizeof t); box(g, y, perf_plabel(fx, k), t); }
    if (shown < 3) {
        if (fx == PF_REVERSE) { box(g, y, "DIR", "<<"); }
        else if (fx == PF_PITCH) { snprintf(t, sizeof t, "X%.2f", (double)v->speed); box(g, y, "RATIO", t); }
        else if (fx == PF_STUTTER) { snprintf(t, sizeof t, "X%.1f", (double)v->speed); box(g, y, "SPEED", t); }
        else if (fx == PF_TSTOP || fx == PF_TSTART || fx == PF_VINYL) { snprintf(t, sizeof t, "%d%%", (int)lroundf(v->speed * 100.0f)); box(g, y, "SPEED", t); }
        else if (fx == PF_CRUSH) { snprintf(t, sizeof t, "%d", 1 << ((int)perf_pval(p, fx, 0) > 5 ? 5 : (int)perf_pval(p, fx, 0))); box(g, y, "STEPS", t); }
        else if (fx == PF_DELAY) { snprintf(t, sizeof t, "%d%%", (int)lroundf(v->aux * 100.0f)); box(g, y, "ECHO", t); }
        else if (fx == PF_RETRIG) { snprintf(t, sizeof t, "%d/4", (v->rep & 3) + 1); box(g, y, "STEP", t); }
        else if (fx == PF_HALF) { snprintf(t, sizeof t, "%d%%", (int)lroundf(v->speed * 100.0f)); box(g, y, "RATIO", t); }
        else if (fx == PF_GATE && n < 3) { }
        else if (fx == PF_FREEZE) { snprintf(t, sizeof t, "%d%%", (int)lroundf(clampf((float)(v->k / (0.9 * (double)MARK_SR)), 0, 1) * 100.0f)); box(g, y, "GROW", t); }
        else if (fx == PF_LP || fx == PF_HP) { snprintf(t, sizeof t, "%d", 12); box(g, y, "SLOPE", "12DB"); }
        else if (fx == PF_KILL) { box(g, y, "MODE", v->held ? "ON" : "OFF"); }
    }
    if (v->latched) gfx_text_c(g, 108, 50, "LATCH", GFX_ON);
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

static int ui_engaged(perf_t *p, const perf_vis_t *v) { return v->fx >= 0 && perf_held_count(p) > 0; }

int perf_ui_draw(perf_ui_t *u, gfx_t *g, perf_t *p, int shift, int target, long clock_frames) {
    perf_vis_t v;
    perf_vis(p, &v);
    char tgt[12];
    float dt = u->last_frames ? (float)(clock_frames - u->last_frames) / (float)MARK_SR : 0.0f;
    if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
    float tsec = (float)clock_frames / (float)MARK_SR;
    int engaged = ui_engaged(p, &v);
    if (!engaged && !shift) return 0;
    gfx_clear(g, GFX_OFF);
    if (!engaged) {
        if (target) snprintf(tgt, sizeof tgt, "PISTA %d", target); else snprintf(tgt, sizeof tgt, "MIX");
        page_grid(g, p, -1, tgt); return 1;
    }
    if (v.target) snprintf(tgt, sizeof tgt, "PISTA %d", v.target); else snprintf(tgt, sizeof tgt, "MIX");
    if (shift && v.held && v.k < 0.15 * (double)MARK_SR) { page_grid(g, p, v.fx, tgt); return 1; }   /* the pressed cell flashes inverted */
    float spd = v.speed;
    if (v.fx == PF_TSTOP || v.fx == PF_TSTART) u->rot[0] += dt * clampf(spd, 0, 1) * 7.0f;
    else if (v.fx == PF_VINYL) u->rot[0] += dt * spd * 3.5f;
    else if (v.fx == PF_REVERSE) u->rot[0] -= dt * 2.4f;
    else if (v.fx == PF_FREEZE) u->rot[0] += dt * 1.0f;
    else if (v.fx == PF_HALF) { u->rot[0] += dt * spd * 3.0f; u->rot[1] += dt * 3.0f; }
    frame(g);
    title(g, perf_name(v.fx), tgt);
    switch (v.fx) {
    case PF_BEATREP: scene_beatrep(g, p, &v); break;
    case PF_STUTTER: scene_stutter(g, p, &v); break;
    case PF_PITCH:   scene_pitch(g, p, &v); break;
    case PF_CRUSH:   scene_crush(g, p, &v, tsec); break;
    case PF_TSTOP: case PF_TSTART: scene_tape(g, u, &v); break;
    case PF_REVERSE: scene_reverse(g, u, &v); break;
    case PF_VINYL:   scene_vinyl(g, u, p, &v); break;
    case PF_LP: case PF_HP: scene_filter(g, p, &v, v.fx == PF_HP, tsec); break;
    case PF_GATE:    scene_gate(g, p, &v); break;
    case PF_DELAY:   scene_delay(g, p, &v); break;
    case PF_FREEZE:  scene_freeze(g, u, p, &v); break;
    case PF_RETRIG:  scene_retrig(g, p, &v); break;
    case PF_HALF:    scene_half(g, u, p, &v); break;
    case PF_KILL:    scene_kill(g, p, &v, tsec); break;
    default: break;
    }
    others(g, &v);
    panel(g, p, &v);
    return 1;
}

int perf_ui_param(perf_ui_t *u, perf_t *p, int shift, int target, long clock_frames, char *out, int n) {
    static char hex[GFX_HEX_LEN + 1];
    if (u->shown && u->have && clock_frames - u->last_frames < (long)(MARK_SR / PUI_FPS) && clock_frames >= u->last_frames) {
        /* inside the frame period: repeat the last answer unless the state flipped to "nothing" */
        perf_vis_t v; perf_vis(p, &v);
        if (ui_engaged(p, &v) || shift) {
            gfx_hex(&u->prev, hex);
            return snprintf(out, (size_t)n, "%u|%s", u->seq, hex);
        }
    }
    if (!perf_ui_draw(u, &u->g, p, shift, target, clock_frames)) {
        u->shown = 0; u->have = 0;
        if (n > 0) out[0] = 0;
        return 0;
    }
    if (!u->have || memcmp(u->g.b, u->prev.b, sizeof u->g.b)) { u->seq = (u->seq + 1) % 100000u; u->prev = u->g; }
    u->have = 1; u->shown = 1; u->last_frames = clock_frames;
    gfx_hex(&u->prev, hex);
    return snprintf(out, (size_t)n, "%u|%s", u->seq, hex);
}
