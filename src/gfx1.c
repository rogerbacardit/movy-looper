#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "gfx1.h"
#include "gfx1_font.h"

void gfx_clear(gfx_t *g, int c) { memset(g->b, c ? 0xFF : 0, sizeof g->b); }

void gfx_px(gfx_t *g, int x, int y, int c) {
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
    uint8_t *p = &g->b[y * (GFX_W / 8) + (x >> 3)], m = (uint8_t)(0x80 >> (x & 7));
    if (c == GFX_XOR) *p ^= m; else if (c) *p |= m; else *p &= (uint8_t)~m;
}

int gfx_get(const gfx_t *g, int x, int y) {
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return 0;
    return (g->b[y * (GFX_W / 8) + (x >> 3)] >> (7 - (x & 7))) & 1;
}

void gfx_dline(gfx_t *g, int x0, int y0, int x1, int y1, int on, int off, int c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0, per = on + off;
    if (per < 1) per = 1;
    for (;;) {
        if (off <= 0 || (n % per) < on) gfx_px(g, x0, y0, c);
        n++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, int c) { gfx_dline(g, x0, y0, x1, y1, 1, 0, c); }

void gfx_circle(gfx_t *g, int cx, int cy, int r, int c) {
    if (r < 0) return;
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        gfx_px(g, cx + x, cy + y, c); gfx_px(g, cx - x, cy + y, c);
        gfx_px(g, cx + x, cy - y, c); gfx_px(g, cx - x, cy - y, c);
        gfx_px(g, cx + y, cy + x, c); gfx_px(g, cx - y, cy + x, c);
        gfx_px(g, cx + y, cy - x, c); gfx_px(g, cx - y, cy - x, c);
        y++;
        if (d < 0) d += 2 * y + 1; else { x--; d += 2 * (y - x) + 1; }
    }
}

void gfx_darc(gfx_t *g, int cx, int cy, int r, float a0, float a1, float dash, int c) {
    if (r < 1) { gfx_px(g, cx, cy, c); return; }
    float step = 0.6f / (float)r * 57.29578f;          /* ~0.6 px per step, in degrees */
    int px = 1 << 30, py = 1 << 30;
    if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
    for (float a = a0; a <= a1 + 1e-3f; a += step) {
        if (dash > 0.0f && fmodf(a - a0, 2.0f * dash) >= dash) continue;
        float rad = a * 0.017453293f;
        int x = cx + (int)lroundf(cosf(rad) * (float)r), y = cy + (int)lroundf(sinf(rad) * (float)r);
        if (x == px && y == py) continue;
        px = x; py = y;
        gfx_px(g, x, y, c);
    }
}
void gfx_arc(gfx_t *g, int cx, int cy, int r, float a0, float a1, int c) { gfx_darc(g, cx, cy, r, a0, a1, 0.0f, c); }
void gfx_dcircle(gfx_t *g, int cx, int cy, int r, int dash_deg, int c) {
    gfx_darc(g, cx, cy, r, 0.0f, 359.0f, (float)dash_deg, c);
}

void gfx_frect(gfx_t *g, int x, int y, int w, int h, int c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) gfx_px(g, x + i, y + j, c);
}
void gfx_rect(gfx_t *g, int x, int y, int w, int h, int c) {
    if (w < 1 || h < 1) return;
    for (int i = 0; i < w; i++) { gfx_px(g, x + i, y, c); gfx_px(g, x + i, y + h - 1, c); }
    for (int j = 1; j < h - 1; j++) { gfx_px(g, x, y + j, c); gfx_px(g, x + w - 1, y + j, c); }
}
void gfx_cross(gfx_t *g, int x, int y, int r, int c) {
    for (int i = -r; i <= r; i++) { gfx_px(g, x + i, y, c); gfx_px(g, x, y + i, c); }
}

static const gfx_glyph_t *glyph_of(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    const char *p = strchr(GFX_CHARS, ch);
    return p && ch ? &GFX_GLYPHS[p - GFX_CHARS] : &GFX_GLYPHS[0];
}
int gfx_text_w(const char *s) {
    int w = 0;
    for (; *s; s++) w += glyph_of(*s)->adv;
    return w > 0 ? w - 1 : 0;
}
int gfx_text(gfx_t *g, int x, int y, const char *s, int c) {
    int x0 = x;
    for (; *s; s++) {
        const gfx_glyph_t *gl = glyph_of(*s);
        for (int r = 0; r < 5; r++)
            for (int i = 0; i < gl->w; i++)
                if ((gl->row[r] >> i) & 1) gfx_px(g, x + i, y + r, c);
        x += gl->adv;
    }
    return x - x0 > 0 ? x - x0 - 1 : 0;
}
void gfx_text_c(gfx_t *g, int cx, int y, const char *s, int c) { gfx_text(g, cx - gfx_text_w(s) / 2, y, s, c); }

void gfx_hex(const gfx_t *g, char *out) {
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < GFX_BYTES; i++) { out[i * 2] = H[g->b[i] >> 4]; out[i * 2 + 1] = H[g->b[i] & 15]; }
    out[GFX_HEX_LEN] = 0;
}
