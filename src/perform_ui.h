/* PERFORM graphics: the PERFORM grid page and one animated HUD scene per effect,
 * drawn with gfx1 on the get_param path (never from process()). */
#ifndef PERFORM_UI_H
#define PERFORM_UI_H
#include "gfx1.h"
#include "perform.h"

#define PUI_FPS 25

typedef struct {
    gfx_t g, prev;
    unsigned seq;
    int      have;               /* a picture was published (prev is valid) */
    long     last_frames;        /* audio clock of the last drawn frame */
    float    rot[2];             /* reel / record / spiral angles (radians) */
    unsigned rnd;
    int      shown;              /* the last call published something (not "") */
} perf_ui_t;

/* Draw one frame: effect scene if one is engaged, else the grid page when `shift`.
 * Returns 0 when there is nothing to show (screen_param ""), 1 otherwise.
 * target: 0 = MIX, else the track number. held_step: step button held (-1 none). */
int perf_ui_draw(perf_ui_t *u, gfx_t *g, perf_t *p, int shift, int target, long clock_frames);

/* screen_param: "<seq>|<hex>" or "". Throttled to PUI_FPS; seq bumps only when the
 * picture changed. Returns the length written. */
int perf_ui_param(perf_ui_t *u, perf_t *p, int shift, int target, long clock_frames, char *out, int n);
#endif
