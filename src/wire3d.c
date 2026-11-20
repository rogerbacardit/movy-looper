#include <math.h>
#include "wire3d.h"

#define NEAR_Z 0.15f

void w3_init(w3_t *w, gfx_t *g, float cx, float cy, float focal, float cam) {
    w->g = g; w->cx = cx; w->cy = cy; w->focal = focal; w->cam = cam;
    w->rx = w->ry = w->rz = 0.0f; w->cue_z = 0.0f; w->snap = 1.0f;
    w->x0 = 0; w->y0 = 0; w->x1 = GFX_W - 1; w->y1 = GFX_H - 1; w->c = GFX_ON;
}
void w3_clip(w3_t *w, int x0, int y0, int x1, int y1) { w->x0 = x0; w->y0 = y0; w->x1 = x1; w->y1 = y1; }
void w3_view(w3_t *w, float rx, float ry, float rz) { w->rx = rx; w->ry = ry; w->rz = rz; }

static v3 rot3(v3 p, float rx, float ry, float rz) {
    float c, s, a, b;
    if (ry != 0.0f) { c = cosf(ry); s = sinf(ry); a = c * p.x + s * p.z; b = -s * p.x + c * p.z; p.x = a; p.z = b; }
    if (rx != 0.0f) { c = cosf(rx); s = sinf(rx); a = c * p.y + s * p.z; b = -s * p.y + c * p.z; p.y = a; p.z = b; }
    if (rz != 0.0f) { c = cosf(rz); s = sinf(rz); a = c * p.x - s * p.y; b = s * p.x + c * p.y; p.x = a; p.y = b; }
    return p;
}

v3 w3_pt(const w3_t *w, const w3_xf *l, v3 p) {
    if (l) {
        p.x *= l->s; p.y *= l->s; p.z *= l->s;
        p = rot3(p, l->rx, l->ry, l->rz);
        p.x += l->t.x; p.y += l->t.y; p.z += l->t.z;
    }
    return rot3(p, w->rx, w->ry, w->rz);
}

int w3_project(const w3_t *w, v3 p, float *sx, float *sy) {
    float d = p.z + w->cam;
    if (d < NEAR_Z) return 0;
    float s = w->focal / d;
    *sx = w->cx + p.x * s; *sy = w->cy - p.y * s;
    if (w->snap > 1.0f) {
        *sx = w->cx + roundf((*sx - w->cx) / w->snap) * w->snap;
        *sy = w->cy + roundf((*sy - w->cy) / w->snap) * w->snap;
    }
    return 1;
}

/* Liang-Barsky against the viewport; returns 0 when nothing is left */
static int clip2d(const w3_t *w, float *x0, float *y0, float *x1, float *y1) {
    float dx = *x1 - *x0, dy = *y1 - *y0, t0 = 0.0f, t1 = 1.0f;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { *x0 - (float)w->x0, (float)w->x1 - *x0, *y0 - (float)w->y0, (float)w->y1 - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) { if (q[i] < 0.0f) return 0; continue; }
        float r = q[i] / p[i];
        if (p[i] < 0.0f) { if (r > t1) return 0; if (r > t0) t0 = r; }
        else { if (r < t0) return 0; if (r < t1) t1 = r; }
    }
    float ax = *x0 + t0 * dx, ay = *y0 + t0 * dy, bx = *x0 + t1 * dx, by = *y0 + t1 * dy;
    *x0 = ax; *y0 = ay; *x1 = bx; *y1 = by;
    return 1;
}

static void draw2d(w3_t *w, float x0, float y0, float x1, float y1, int style) {
    if (!clip2d(w, &x0, &y0, &x1, &y1)) return;
    int a = (int)lroundf(x0), b = (int)lroundf(y0), c = (int)lroundf(x1), d = (int)lroundf(y1);
    switch (style) {
    case W3_DOT:   gfx_dline(w->g, a, b, c, d, 1, 2, w->c); break;
    case W3_DASH:  gfx_dline(w->g, a, b, c, d, 3, 2, w->c); break;
    case W3_FAINT: gfx_dline(w->g, a, b, c, d, 1, 4, w->c); break;
    default:       gfx_line(w->g, a, b, c, d, w->c); break;
    }
}

void w3_line2d(w3_t *w, float x0, float y0, float x1, float y1, int style) { draw2d(w, x0, y0, x1, y1, style); }

void w3_seg(w3_t *w, v3 a, v3 b, int style) {
    float na = NEAR_Z - w->cam;                       /* view z of the near plane */
    if (a.z < na && b.z < na) return;
    if (a.z < na) { float t = (na - a.z) / (b.z - a.z); a.x += t * (b.x - a.x); a.y += t * (b.y - a.y); a.z = na; }
    else if (b.z < na) { float t = (na - b.z) / (a.z - b.z); b.x += t * (a.x - b.x); b.y += t * (a.y - b.y); b.z = na; }
    if (style == W3_AUTO) style = (a.z + b.z) * 0.5f > w->cue_z ? W3_DOT : W3_SOLID;
    float x0, y0, x1, y1;
    if (!w3_project(w, a, &x0, &y0) || !w3_project(w, b, &x1, &y1)) return;
    draw2d(w, x0, y0, x1, y1, style);
}

void w3_line(w3_t *w, const w3_xf *l, v3 a, v3 b, int style) { w3_seg(w, w3_pt(w, l, a), w3_pt(w, l, b), style); }

void w3_box(w3_t *w, const w3_xf *l, float hx, float hy, float hz, int style) {
    v3 p[8];
    for (int i = 0; i < 8; i++) p[i] = w3_pt(w, l, (v3){ i & 1 ? hx : -hx, i & 2 ? hy : -hy, i & 4 ? hz : -hz });
    for (int i = 0; i < 8; i++)
        for (int b = 1; b <= 4; b <<= 1)
            if (!(i & b)) w3_seg(w, p[i], p[i | b], style);
}

void w3_circle(w3_t *w, const w3_xf *l, v3 c, v3 ex, v3 ey, float r, int segs, int style) {
    v3 prev = { 0, 0, 0 };
    for (int i = 0; i <= segs; i++) {
        float a = 6.2831853f * (float)i / (float)segs, ca = cosf(a) * r, sa = sinf(a) * r;
        v3 p = w3_pt(w, l, (v3){ c.x + ex.x * ca + ey.x * sa, c.y + ex.y * ca + ey.y * sa, c.z + ex.z * ca + ey.z * sa });
        if (i) {
            if (style == W3_AUTO) { if ((prev.z + p.z) * 0.5f <= w->cue_z || !(i & 1)) w3_seg(w, prev, p, W3_SOLID); }   /* far half: every other segment = dashes */
            else w3_seg(w, prev, p, style);
        }
        prev = p;
    }
}

void w3_poly(w3_t *w, const w3_xf *l, int n, float r, float y, float a0, int style) {
    v3 first = { 0, 0, 0 }, prev = { 0, 0, 0 };
    for (int i = 0; i <= n; i++) {
        float a = a0 + 6.2831853f * (float)(i % n) / (float)n;
        v3 p = w3_pt(w, l, (v3){ cosf(a) * r, y, sinf(a) * r });
        if (i == 0) first = p; else w3_seg(w, prev, i == n ? first : p, style);
        prev = p;
    }
}

void w3_square(w3_t *w, const w3_xf *l, float h, int style) {      /* in the XY plane, facing the viewer */
    v3 p[4];
    for (int i = 0; i < 4; i++) p[i] = w3_pt(w, l, (v3){ i == 1 || i == 2 ? h : -h, i & 2 ? h : -h, 0 });
    for (int i = 0; i < 4; i++) w3_seg(w, p[i], p[(i + 1) & 3], style);
}

void w3_cylinder(w3_t *w, const w3_xf *l, int n, float r, float h, int style) {
    w3_poly(w, l, n, r, h, 0.0f, style);
    w3_poly(w, l, n, r, -h, 0.0f, style);
    for (int i = 0; i < n; i += n > 8 ? n / 4 : 1) {
        float a = 6.2831853f * (float)i / (float)n;
        w3_line(w, l, (v3){ cosf(a) * r, h, sinf(a) * r }, (v3){ cosf(a) * r, -h, sinf(a) * r }, style);
    }
}

void w3_disc(w3_t *w, const w3_xf *l, float r, int segs, int style) {
    w3_circle(w, l, (v3){ 0, 0, 0 }, (v3){ 1, 0, 0 }, (v3){ 0, 0, 1 }, r, segs, style);
}

void w3_sphere(w3_t *w, const w3_xf *l, float r, int nlat, int nlon, int segs, int style) {
    for (int k = 0; k < nlat; k++) {                         /* latitudes, evenly spread, the equator included when odd */
        float ph = ((float)k - (float)(nlat - 1) * 0.5f) * (3.1415927f / (float)(nlat + 1));
        w3_circle(w, l, (v3){ 0, sinf(ph) * r, 0 }, (v3){ 1, 0, 0 }, (v3){ 0, 0, 1 }, cosf(ph) * r, segs, style);
    }
    for (int j = 0; j < nlon; j++) {                         /* meridians */
        float th = 3.1415927f * (float)j / (float)nlon;
        w3_circle(w, l, (v3){ 0, 0, 0 }, (v3){ cosf(th), 0, sinf(th) }, (v3){ 0, 1, 0 }, r, segs, style);
    }
}

void w3_octa(w3_t *w, const w3_xf *l, float r, int style) {
    v3 p[6];
    static const float V[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int i = 0; i < 6; i++) p[i] = w3_pt(w, l, (v3){ V[i][0] * r, V[i][1] * r, V[i][2] * r });
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if ((i ^ 1) != j) w3_seg(w, p[i], p[j], style);
}

void w3_tunnel(w3_t *w, const w3_xf *l, int n, float half, float zspan, float flow, int style) {
    float sp = zspan / (float)n;
    for (int c = 0; c < 4; c++) {                            /* the four rails */
        float sx = c & 1 ? half : -half, sy = c & 2 ? half : -half;
        w3_line(w, l, (v3){ sx, sy, 0 }, (v3){ sx, sy, zspan * 0.55f }, W3_FAINT);
    }
    for (int i = 0; i < n; i++) {
        float z = fmodf((float)i * sp + flow, zspan);
        if (z < 0.0f) z += zspan;
        if (w->focal / (z + w->cam + (l ? l->t.z : 0.0f)) * half < 6.0f) continue;        /* too small to read */
        w3_xf q = l ? *l : W3_XF(0, 0, 0, 0, 0, 0, 1);
        q.t.z += z;
        w3_square(w, &q, half, style);
    }
}

/* synthwave grid. Screen-space perspective: a surface point at lateral x and depth d (d = 1 is the
 * nearest row) lands at sx = cx + x*FX/d, sy = yh + dir*K/d, K the horizon-to-edge distance. */
void w3_grid(w3_t *w, float yh, int dir, float flow, float wave, float wt) {
    float edge = dir > 0 ? (float)w->y1 : (float)w->y0, K = fabsf(edge - yh) * 0.9f;
    const float FX = 34.0f, DEND = 5.5f;
    float last = yh;
    for (int j = 0; j < 8; j++) {                            /* cross lines, geometric spacing, flowing toward the viewer */
        float d = powf(1.6f, (float)j - flow);
        if (d < 0.8f || d > DEND) continue;
        float base = yh + (float)dir * K / d;
        if (fabsf(base - last) < 4.0f && j) continue;        /* no mush near the horizon */
        last = base;
        int style = d > 2.9f ? W3_DOT : W3_SOLID;
        float px = 0, py = 0; int have = 0;
        for (int i = -14; i <= 14; i++) {
            float x = (float)i * 0.36f;
            float sy = base + wave * sinf(x * 2.1f + d * 1.5f + wt) * 2.4f / d;
            float sx = w->cx + x * FX / d;
            if (have) w3_line2d(w, px, py, sx, sy, style);
            px = sx; py = sy; have = 1;
        }
    }
    for (int i = -3; i <= 3; i++) {                          /* receding lines */
        float x = (float)i * 0.95f, px = 0, py = 0; int have = 0;
        for (int s = 0; s <= 14; s++) {
            float d = 0.8f * powf(DEND / 0.8f, (float)s / 14.0f);
            float sy = yh + (float)dir * K / d + wave * sinf(x * 2.1f + d * 1.5f + wt) * 2.4f / d;
            float sx = w->cx + x * FX / d;
            if (have) w3_line2d(w, px, py, sx, sy, d > 2.9f ? W3_DOT : W3_SOLID);
            px = sx; py = sy; have = 1;
        }
    }
    w3_line2d(w, (float)w->x0, yh, (float)w->x1, yh, W3_SOLID);
}
