/* wire3d: a tiny 3D wireframe engine for the 128x64 1-bit OLED (Looper 606 PERFORM).
 * Float math, perspective projection, clipping to a viewport, and "far edges dotted" as the
 * depth cue. Draws with gfx1 only; only ever called from get_param (never process()).
 * View space: x right, y up, z away from the viewer; a point projects to
 *   sx = cx + x * focal / (z + cam),  sy = cy - y * focal / (z + cam). */
#ifndef WIRE3D_H
#define WIRE3D_H
#include "gfx1.h"

typedef struct { float x, y, z; } v3;

enum { W3_SOLID = 0,    /* solid line */
       W3_AUTO,         /* solid when near, dotted when beyond cue_z: the depth cue */
       W3_DOT,          /* 1 on / 2 off */
       W3_DASH,         /* 3 on / 2 off */
       W3_FAINT };      /* 1 on / 4 off */

typedef struct {
    gfx_t *g;
    float cx, cy, focal, cam;          /* projection */
    float rx, ry, rz;                  /* world rotation: Y, then X (tilt), then Z */
    float cue_z;                       /* beyond this view z, W3_AUTO draws dotted */
    float snap;                        /* >1: snap projected points to a coarse px grid (bitcrush) */
    int   x0, y0, x1, y1;              /* clip rect, inclusive */
    int   c;                           /* GFX_ON */
} w3_t;

/* local transform applied before the world rotation: scale, rotate (Y, X, Z), then translate */
typedef struct { v3 t; float rx, ry, rz, s; } w3_xf;
#define W3_XF(tx, ty, tz, rx, ry, rz, s) ((w3_xf){ { (tx), (ty), (tz) }, (rx), (ry), (rz), (s) })

void w3_init(w3_t *w, gfx_t *g, float cx, float cy, float focal, float cam);
void w3_clip(w3_t *w, int x0, int y0, int x1, int y1);
void w3_view(w3_t *w, float rx, float ry, float rz);

v3   w3_pt(const w3_t *w, const w3_xf *l, v3 p);                       /* local -> view space */
int  w3_project(const w3_t *w, v3 view, float *sx, float *sy);          /* 0 when behind the camera */
void w3_seg(w3_t *w, v3 a, v3 b, int style);                            /* view-space segment: near clip, depth cue, clip, draw */
void w3_line(w3_t *w, const w3_xf *l, v3 a, v3 b, int style);           /* local-space line */
void w3_line2d(w3_t *w, float x0, float y0, float x1, float y1, int style);   /* clipped screen line */

/* primitives; all centred on the local origin (move them with the xf) */
void w3_box(w3_t *w, const w3_xf *l, float hx, float hy, float hz, int style);
void w3_poly(w3_t *w, const w3_xf *l, int n, float r, float y, float a0, int style);   /* n-gon in the XZ plane at height y */
void w3_square(w3_t *w, const w3_xf *l, float h, int style);                          /* square in the XY plane */
void w3_circle(w3_t *w, const w3_xf *l, v3 c, v3 ex, v3 ey, float r, int segs, int style);
void w3_cylinder(w3_t *w, const w3_xf *l, int n, float r, float h, int style);        /* two n-gon rings + verticals */
void w3_disc(w3_t *w, const w3_xf *l, float r, int segs, int style);                  /* ring in the XZ plane */
void w3_sphere(w3_t *w, const w3_xf *l, float r, int nlat, int nlon, int segs, int style);   /* latitude / longitude ellipses */
void w3_octa(w3_t *w, const w3_xf *l, float r, int style);
void w3_tunnel(w3_t *w, const w3_xf *l, int n, float half, float zspan, float flow, int style);   /* n squares flowing along z, + corner rails */
/* synthwave grid: floor (dir +1, below the horizon) or ceiling (dir -1), horizon at screen row yh */
void w3_grid(w3_t *w, float yh, int dir, float flow, float wave, float wt);
#endif
