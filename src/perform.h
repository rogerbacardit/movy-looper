/* PERFORM: momentary / latched performance effects for Looper 606 (steps 1-16).
 *
 * The effects are STREAM processors with history. Every target (MIX = lane 0, a track
 * = lane 1..6) has a lane: three rings with the last ~5.9 s of its audio (ring0 = the
 * lane's input, ring1 = after the time/repeat stage, ring2 = after the kill stage) and
 * one slot per effect. perf_process() runs the active slots of the lane in a FIXED
 * chain order, each one replacing the sample with dry -> wet crossfaded by its own gate
 * (4 ms in, 12 ms out; KILL 3 ms both ways). Nothing here ever writes to a loop buffer
 * or touches a playhead, so with no active effect the sample is returned untouched and
 * a release restores the exact signal that was running underneath, in sync. The one
 * allowed difference after a release is the DELAY THROW tail.
 *
 * Chain order: time/repeat (BEAT REPEAT, STUTTER, RETRIGGER, REVERSE) -> pitch/tape
 * (PITCH, HALF SPEED, TAPE, LFO, VINYL) -> filters (LP, HP) -> BITCRUSH ->
 * GATE -> KILL -> DELAY / FREEZE. Up to PF_MAXSTACK effects at once (held or latched).
 *
 * Audio thread: no allocation, no locks, no printf. The control thread keeps the stack
 * (perf_press / perf_release) and publishes one atomic bit mask per lane; the engage
 * itself (slice capture) happens inside perf_block_begin() on the audio thread. */
#ifndef PERFORM_H
#define PERFORM_H
#include <stdint.h>

enum { PF_BEATREP = 0, PF_STUTTER, PF_PITCH, PF_CRUSH, PF_TSTOP, PF_LFO, PF_REVERSE, PF_VINYL,
       PF_LP, PF_HP, PF_GATE, PF_DELAY, PF_FREEZE, PF_RETRIG, PF_HALF, PF_KILL,
       PF_DUCK,                 /* bank 2, step 1 (17th effect) */
       PF_NFX };
#define PF_BANK 16              /* effects per page of step buttons; < and > flip pages (max 2 pages: masks are 32 bit) */
#define PF_NBANK ((PF_NFX + PF_BANK - 1) / PF_BANK)
#define PF_NKNOB 8
#define PF_MAXSTACK 4
#define PF_NLANE 7              /* MIX + 6 tracks (compile-time MARK_TRACKS must be <= 6) */
#define PF_HOLD_MS 300          /* a step held longer than this is momentary; a shorter tap toggles */

typedef struct perf perf_t;

typedef struct {                 /* what the screen needs; read off the audio thread, torn reads are harmless */
    int   fx;                    /* most recent effect, -1 = none */
    int   target;                /* its target: 0 = MIX, else the track */
    int   held;                  /* its step is still down */
    int   latched;
    float gate;                  /* 0..1 wet amount */
    double k;                    /* frames since engage */
    double beat;                 /* frames per beat */
    int   rep;                   /* repeat / cell counter */
    float speed;                 /* tape/vinyl/half: current speed or ratio (reverse = -1), gate: current gain */
    float phase;                 /* 0..1 position inside the current repeat / cell */
    float aux;                   /* filter: 0..1 smoothed cutoff position; delay: echo level 0..1 */
    float level;                 /* 0..1 envelope of the lane input */
    int   nother;                /* the other stacked effects (press order), for the small indicators */
    int   other[PF_MAXSTACK - 1];
} perf_vis_t;

perf_t *perf_create(void);
void    perf_destroy(perf_t *p);

/* control thread: the stack */
/* toggle = 1 (PERFORM mode): a tap switches the effect on and it stays; held longer than PF_HOLD_MS it is momentary.
 * toggle = 0 (SHIFT held, mode off): always momentary. Pressing a step that is already on switches it off. */
enum { PF_P_REFUSED = 0, PF_P_ON, PF_P_OFF };
int  perf_press(perf_t *p, int fx, int target, long now_frames, int toggle);   /* step down */
int  perf_release(perf_t *p, int fx, long now_frames);            /* step up; 1 = the effect stays on (a tap) */
void perf_clear_target(perf_t *p, int target);                    /* switch off everything bound to this target (RESET of a track) */
int  perf_count(const perf_t *p);
int  perf_entry(const perf_t *p, int i, int *fx, int *target, int *latched, int *held);   /* i = 0 oldest; latched = toggled on */
int  perf_top_fx(const perf_t *p);                                /* most recent, -1 none */
int  perf_held_count(const perf_t *p);
unsigned perf_on_mask(const perf_t *p);                           /* effects in the stack */
unsigned perf_latch_mask(const perf_t *p);

/* control thread: knobs */
void perf_knob(perf_t *p, int fx, int knob, int delta);   /* knob 0..7, relative clicks (2 % each; stepped params move one option) */
int  perf_pct(const perf_t *p, int fx, int knob);          /* the stored 0..100 */
void perf_pct_set(perf_t *p, int fx, int knob, int v);

/* AUTOMATION of the knobs (3 per effect): PF_AU_N steps over the base loop, 255 = empty.
 * While recording (a track in REC/DUB, or AUTO-REC on) a knob that is being turned writes the
 * step under the playhead; a knob that is not being turned plays the stored curve back. Only
 * effects that are on are touched. */
#define PF_AU_N 64
void perf_auto_rec(perf_t *p, int on);                     /* AUTO-REC mode (control thread) */
int  perf_auto_rec_get(const perf_t *p);
void perf_auto_clear(perf_t *p, int fx, int knob);         /* knob < 0: all knobs of the effect */
void perf_auto_clear_all(perf_t *p);
int  perf_auto_has(const perf_t *p, int fx, int knob);     /* number of steps stored (0 = none) */
int  perf_auto_step(const perf_t *p);                      /* current step 0..PF_AU_N-1, -1 = no loop */

/* audio thread */
/* loop_len = frames of the base loop region (0 = no loop running: no automation), recording = a track is in REC/DUB */
void perf_block_begin(perf_t *p, double grid_pos, double beat_frames, double loop_len, int recording);
void perf_process(perf_t *p, int lane, float *l, float *r);   /* in place, int16-scaled floats */

/* any thread */
void perf_vis(const perf_t *p, perf_vis_t *v);
int  perf_lane_active(const perf_t *p, int lane);                  /* slots running (incl. fading / delay tail) */

/* names and parameter text, for the screen / knob readouts */
const char *perf_name(int fx);                       /* "BEAT REPEAT" */
const char *perf_short(int fx);                      /* "RPT" (grid cell, <= 14 px wide in the 4x5 font) */
int         perf_nparams(int fx);
const char *perf_plabel(int fx, int knob);           /* "RATE" */
void        perf_ptext(const perf_t *p, int fx, int knob, char *out, int n);   /* "1/8" */
float       perf_pval(const perf_t *p, int fx, int knob);                      /* decoded value (see perform.c) */
/* 0 repeat (blue), 1 tape/time (orange), 2 pitch/tone/filter (violet), 3 rhythm (cyan), 4 space (teal), 5 kill (yellow) */
int         perf_family(int fx);
#endif
