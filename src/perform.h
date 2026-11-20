/* PERFORM: momentary performance effects for Looper 606 (batch 1: steps 1-8).
 *
 * The effects are STREAM processors with history: perf_process() is fed the
 * target's audio (one selected track after its read, or the whole looper mix) one
 * frame at a time, keeps the last ~6 s in a ring, and replaces the sample with
 * dry -> wet crossfaded by a gate (4 ms in, 12 ms out). Nothing here ever writes
 * to a loop buffer or touches a playhead, so releasing a step returns the exact
 * signal that was running underneath, in sync.
 *
 * Audio thread: no allocation, no locks, no printf. press/release come from the
 * control thread through one atomic (perf_set_want); the engage itself (slice
 * capture) happens inside perf_block_begin() on the audio thread. */
#ifndef PERFORM_H
#define PERFORM_H
#include <stdint.h>

enum { PF_BEATREP = 0, PF_STUTTER, PF_PITCH, PF_CRUSH, PF_TSTOP, PF_TSTART, PF_REVERSE, PF_VINYL, PF_NFX };
#define PF_NKNOB 8

typedef struct perf perf_t;

typedef struct {                 /* what the screen needs; read off the audio thread, torn reads are harmless */
    int   fx;                    /* -1 = idle */
    int   held;                  /* the step is still down */
    float gate;                  /* 0..1 wet amount */
    double k;                    /* frames since engage */
    double beat;                 /* frames per beat at engage */
    int   rep;                   /* beat repeat / stutter: repeat counter */
    float speed;                 /* tape/vinyl/start/stop: current speed or ratio (reverse = -1) */
    float phase;                 /* beat repeat / stutter: 0..1 position inside the current repeat */
    float level;                 /* 0..1 envelope of the effect input */
} perf_vis_t;

perf_t *perf_create(void);
void    perf_destroy(perf_t *p);

/* control thread */
void perf_set_want(perf_t *p, int fx);               /* fx 0..PF_NFX-1 = hold it, -1 = release */
int  perf_want(const perf_t *p);
void perf_knob(perf_t *p, int fx, int knob, int delta);   /* knob 0..7, relative clicks (2 % each) */
int  perf_pct(const perf_t *p, int fx, int knob);          /* the stored 0..100 */
void perf_pct_set(perf_t *p, int fx, int knob, int v);

/* audio thread */
void perf_block_begin(perf_t *p, double grid_pos, double beat_frames);
void perf_process(perf_t *p, float *l, float *r);    /* in place, int16-scaled floats */
int  perf_busy(const perf_t *p);                     /* an effect is engaged or fading out */

/* any thread */
void perf_vis(const perf_t *p, perf_vis_t *v);

/* names and parameter text, for the screen / knob readouts */
const char *perf_name(int fx);                       /* "BEAT RPT" */
const char *perf_short(int fx);                      /* "RPT" (grid cell) */
int         perf_nparams(int fx);
const char *perf_plabel(int fx, int knob);           /* "RATE" */
void        perf_ptext(const perf_t *p, int fx, int knob, char *out, int n);   /* "1/8" */
float       perf_pval(const perf_t *p, int fx, int knob);                      /* decoded value (see perform.c) */
int         perf_family(int fx);                     /* 0 repeat, 1 tape/time, 2 pitch/tone, 3 rhythm */
#endif
