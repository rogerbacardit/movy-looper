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
    }
    level_bar(g, v->level);
}

/* ---- the PERFORM grid page ---- */
static void page_grid(gfx_t *g, perf_t *p, int held, const char *tgt) {
    title(g, "PERFORM", tgt);
    (void)p;
    for (int i = 0; i < 16; i++) {
        int col = i & 7, row = i >> 3, x = col * 16, y = row ? 35 : 11, w = 16, h = 22;
        char num[4]; snprintf(num, sizeof num, "%d", i + 1);
        if (i < PF_NFX) {
            int on = i == held;
            if (on) gfx_frect(g, x, y, w, h, GFX_ON);
            else {                                                /* top and bottom rules, a dotted post between cells */
                gfx_line(g, x, y, x + w - 1, y, GFX_ON); gfx_line(g, x, y + h - 1, x + w - 1, y + h - 1, GFX_ON);
                if (col) gfx_dline(g, x, y + 1, x, y + h - 2, 1, 1, GFX_ON);
            }
            int c = on ? GFX_OFF : GFX_ON, fam = perf_family(i);
            gfx_text_c(g, x + 8, y + 3, num, c);
            gfx_text_c(g, x + 8, y + 10, perf_short(i), c);
            if (fam == 0) gfx_dline(g, x + 2, y + 18, x + w - 3, y + 18, 1, 1, c);
            else if (fam == 1) gfx_line(g, x + 2, y + 18, x + w - 3, y + 18, c);
            else if (fam == 2) gfx_dline(g, x + 2, y + 18, x + w - 3, y + 18, 3, 1, c);
            else { gfx_dline(g, x + 2, y + 17, x + w - 3, y + 17, 2, 2, c); gfx_dline(g, x + 3, y + 19, x + w - 2, y + 19, 2, 2, c); }
        } else {                                                  /* batch 2: not there yet */
            for (int c = 0; c < 4; c++) {                         /* corner brackets only */
                int cx0 = c & 1 ? x + w - 1 : x, cy0 = c & 2 ? y + h - 1 : y, dx = c & 1 ? -1 : 1, dy = c & 2 ? -1 : 1;
                gfx_line(g, cx0, cy0, cx0 + 3 * dx, cy0, GFX_ON); gfx_line(g, cx0, cy0, cx0, cy0 + 3 * dy, GFX_ON);
            }
            gfx_text_c(g, x + 8, y + 3, num, GFX_ON);
            gfx_text_c(g, x + 8, y + 10, "---", GFX_ON);
        }
    }
    gfx_text_c(g, 64, 58, "HOLD A STEP", GFX_ON);
}

int perf_ui_draw(perf_ui_t *u, gfx_t *g, perf_t *p, int shift, int target, long clock_frames) {
    perf_vis_t v;
    perf_vis(p, &v);
    char tgt[12];
    if (target) snprintf(tgt, sizeof tgt, "PISTA %d", target); else snprintf(tgt, sizeof tgt, "MIX");
    float dt = u->last_frames ? (float)(clock_frames - u->last_frames) / (float)MARK_SR : 0.0f;
    if (dt < 0.0f || dt > 0.5f) dt = 0.0f;
    float tsec = (float)clock_frames / (float)MARK_SR;
    int engaged = v.fx >= 0 && (v.held || v.gate > 0.0f);
    if (!engaged && !shift) return 0;
    gfx_clear(g, GFX_OFF);
    if (!engaged) { page_grid(g, p, -1, tgt); return 1; }
    if (shift && v.held && v.k < 0.15 * (double)MARK_SR) { page_grid(g, p, v.fx, tgt); return 1; }   /* the pressed cell flashes inverted */
    float spd = v.speed;
    if (v.fx == PF_TSTOP || v.fx == PF_TSTART) u->rot[0] += dt * clampf(spd, 0, 1) * 7.0f;
    else if (v.fx == PF_VINYL) u->rot[0] += dt * spd * 3.5f;
    else if (v.fx == PF_REVERSE) u->rot[0] -= dt * 2.4f;
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
    default: break;
    }
    panel(g, p, &v);
    return 1;
}

int perf_ui_param(perf_ui_t *u, perf_t *p, int shift, int target, long clock_frames, char *out, int n) {
    static char hex[GFX_HEX_LEN + 1];
    if (u->shown && u->have && clock_frames - u->last_frames < (long)(MARK_SR / PUI_FPS) && clock_frames >= u->last_frames) {
        /* inside the frame period: repeat the last answer unless the state flipped to "nothing" */
        perf_vis_t v; perf_vis(p, &v);
        if ((v.fx >= 0 && (v.held || v.gate > 0.0f)) || shift) {
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
