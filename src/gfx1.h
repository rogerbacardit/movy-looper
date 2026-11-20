/* gfx1: a tiny 128x64 1-bit canvas for the Move's OLED (Looper 606 PERFORM).
 * Layout = Movy's screen_param: row-major, 16 bytes a row, MSB = leftmost pixel.
 * Pure C, no allocation; only ever called from get_param (never process()). */
#ifndef GFX1_H
#define GFX1_H
#include <stdint.h>

#define GFX_W 128
#define GFX_H 64
#define GFX_BYTES (GFX_W * GFX_H / 8)
#define GFX_HEX_LEN (GFX_BYTES * 2)

enum { GFX_OFF = 0, GFX_ON = 1, GFX_XOR = 2 };

typedef struct { uint8_t b[GFX_BYTES]; } gfx_t;

void gfx_clear(gfx_t *g, int c);                       /* c = 0 / 1 */
void gfx_px(gfx_t *g, int x, int y, int c);
int  gfx_get(const gfx_t *g, int x, int y);
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, int c);                 /* Bresenham */
void gfx_dline(gfx_t *g, int x0, int y0, int x1, int y1, int on, int off, int c);
void gfx_circle(gfx_t *g, int cx, int cy, int r, int c);                        /* midpoint */
void gfx_dcircle(gfx_t *g, int cx, int cy, int r, int dash_deg, int c);         /* dashes + gaps of dash_deg degrees, rot = a0 */
void gfx_arc(gfx_t *g, int cx, int cy, int r, float a0, float a1, int c);       /* degrees, 0 = right, 90 = down */
void gfx_darc(gfx_t *g, int cx, int cy, int r, float a0, float a1, float dash_deg, int c);
void gfx_rect(gfx_t *g, int x, int y, int w, int h, int c);                     /* outline */
void gfx_frect(gfx_t *g, int x, int y, int w, int h, int c);                    /* filled */
void gfx_cross(gfx_t *g, int x, int y, int r, int c);                           /* small + */
int  gfx_text_w(const char *s);
int  gfx_text(gfx_t *g, int x, int y, const char *s, int c);                    /* 4x5 font, returns width */
void gfx_text_c(gfx_t *g, int cx, int y, const char *s, int c);                 /* centred on cx */
void gfx_hex(const gfx_t *g, char *out);                                        /* GFX_HEX_LEN digits + NUL */
#endif
