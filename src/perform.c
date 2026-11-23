#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mark_core.h"
#include "perform.h"

#define SR        ((double)MARK_SR)
#define RING_N    (1 << 18)                 /* frames of history (~5.9 s) */
#define RING_MASK (RING_N - 1)
#define SL_N      (1 << 17)                 /* slice / delay buffer frames (~3 s) */
#define GRAN_W    1764.0                    /* granular window (40 ms) */
#define FADE_IN   (1.0f / (0.004f * (float)MARK_SR))
#define FADE_OUT  (1.0f / (0.012f * (float)MARK_SR))
#define FADE_KILL (1.0f / (0.003f * (float)MARK_SR))
#define EDGE      32.0f                     /* repeat edge fade, frames */
#define XFADE     96                        /* jump crossfade (retrigger / half speed), frames */

enum { SB_REP, SB_STU, SB_FRZ, SB_DLY, SB_N };

typedef struct { float a1, a2, a3, kk; } svfc;

typedef struct {
    int   act, held;
    float g;
    double k;
    int64_t p0, L;                          /* beat repeat: phase and slice length; freeze: length */
    int   rep, last_rep, rep_pos, rep_len;
    float rep_gain, rep_rho;
    double d;                               /* tape stop head delay */
    double ph;                              /* granular phase */
    uint64_t rv0;                           /* reverse: press point */
    double hold;                            /* crush sample&hold countdown */
    float hl, hr, lpl, lpr;
    float speed, phase, aux;
    /* filters / kill: TPT state-variable filter */
    float ic1[2][2], ic2[2][2], fcs;
    svfc  c[2];                             /* [0] main (reso) stage, [1] fixed 2nd stage (24 dB slope) */
    float slope;                            /* LP/HP: 0 = 12 dB .. 1 = 24 dB, slewed */
    float fk;                               /* KILL: fade step per frame */
    double lfo;                             /* wow / flutter phase */
    double offp;                            /* retrigger: pitch-drop offset at the last jump */
    int   fn, kmode;
    int   ph2; double k2; float u0;         /* TAPE: 0 stopping, 1 starting back up, 2 up to speed */
    /* gate */
    float gg;
    /* delay */
    double dly; int wi; float env, fl, fr_; int rel; double tail;
    /* retrigger / half speed */
    double dprev; int xf; double tau_prev;
} slot_t;

typedef struct {
    float *ring, *ring1, *ring2;            /* RING_N frames, stereo interleaved */
    float *sb[SB_N];                        /* SL_N frames each */
    int    dly_dirty;
    slot_t s[PF_NFX];
    uint32_t act;                           /* bit per active slot (audio thread) */
    _Atomic uint32_t want;                  /* bit per held/latched effect (control thread) */
    uint64_t wc;                            /* frames written */
    double grid, beat;
    int    bn;                              /* frames since the block began */
    float  lvl;
    uint32_t rng;
} lane_t;

typedef struct { int fx, target, latched, held, tog; long t0; } entry_t;

struct perf {
    lane_t ln[PF_NLANE];
    uint8_t pct[PF_NFX][PF_NKNOB];
    entry_t st[PF_MAXSTACK];
    int    nst;
    /* knob automation (see perform.h) */
    uint8_t au[PF_NLANE][PF_NFX][3][PF_AU_N];   /* [track 1..6] */
    int     aucnt[PF_NLANE][PF_NFX][3];         /* steps stored */
    _Atomic int hold;                           /* track whose REC pad is held in PERFORM, 0 = none */
    _Atomic int touch[PF_NFX][3];               /* a knob was turned (set by the control thread, consumed per block) */
    uint8_t armed[PF_NFX][3];                   /* audio thread: being written until the hold ends */
    int     last_step[PF_NFX][3];               /* audio thread: last step written, -1 none */
};

/* defaults; stepped parameters (DISC) hold idx * 100 / (n - 1) */
static const uint8_t DEF[PF_NFX][3] = {
    { 20, 20, 0 },   { 33, 50, 100 }, { 75, 50, 100 }, { 60, 20, 100 },
    { 40, 0, 0 },    { 78, 0, 70 },   { 100, 100, 0 }, { 40, 40, 0 },
    { 75, 25, 0 },   { 30, 25, 0 },   { 40, 85, 0 },   { 50, 40, 85 },
    { 50, 100, 0 },  { 50, 0, 0 },    { 0, 100, 0 },   { 0, 7, 53 },
    { 40, 70, 50 },
};
static const uint8_t DISC[PF_NFX][3] = {          /* option count of a stepped knob, 0 = continuous */
    { 0 }, { 0, 3, 0 }, { 0 }, { 0 }, { 0 }, { 10, 4, 0 }, { 4, 0, 0 }, { 0 },
    { 0, 0, 2 }, { 0, 0, 2 }, { 6, 0, 0 }, { 5, 0, 0 }, { 5, 0, 0 }, { 3, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 },
    { 6, 0, 0 },
};
static const char *const NAME[PF_NFX] = { "BEAT REPEAT", "STUTTER", "PITCH", "BITCRUSH", "TAPE", "LFO", "REVERSE", "VINYL",
    "FILTER LP", "FILTER HP", "GATE", "DELAY THROW", "ECHO FREEZE", "RETRIGGER", "HALF SPEED", "KILL", "SIDECHAIN" };
static const char *const SHORT[PF_NFX] = { "RPT", "STU", "PCH", "BIT", "TAP", "LFO", "BCK", "VIN",
    "LPF", "HPF", "GAT", "DLY", "FRZ", "RTG", "HLF", "KIL", "DUK" };
static const int NP[PF_NFX] = { 3, 3, 3, 3, 3, 3, 2, 3, 3, 3, 3, 3, 3, 3, 2, 3, 3 };
static const char *const PL[PF_NFX][3] = {          /* <= 4 letters: they must fit beside the knob number tab */
    { "RATE", "DCAY", "DROP" }, { "TIME", "END", "CURV" }, { "SEMI", "FINE", "MIX" }, { "BITS", "DOWN", "MIX" },
    { "TIME", "CURV", "WOW" }, { "RATE", "DEST", "DEPT" }, { "WIND", "MIX", "" }, { "BRAK", "CRKL", "WOW" },
    { "CUT", "RESO", "SLOP" }, { "CUT", "RESO", "SLOP" }, { "RATE", "DEPT", "SWNG" }, { "TIME", "FDBK", "LEVL" },
    { "SIZE", "MIX", "DCAY" }, { "EVRY", "DCAY", "PTCH" }, { "SPED", "MIX", "" }, { "PART", "FADE", "SPLT" },
    { "RATE", "DEPT", "CURV" },
};
static const char *const LFO_TXT[10] = { "1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4.", "1/2", "1BAR", "2BAR", "4BAR" };
static const double LFO_BEATS[10] = { 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0 };
static const char *const LFO_DST[4] = { "FILT", "VOL", "PAN", "PTCH" };
static const char *const RATE_TXT[6] = { "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T" };
static const double RATE_MUL[6] = { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
static const char *const DLY_TXT[5] = { "1/8", "1/8.", "1/4", "1/4.", "1/2" };
static const double DLY_MUL[5] = { 0.5, 0.75, 1.0, 1.5, 2.0 };
static const char *const FRZ_TXT[5] = { "1/16", "1/8", "1/4", "1/2", "1B" };
static const double FRZ_MUL[5] = { 0.0625, 0.125, 0.25, 0.5, 1.0 };
static const char *const RTG_TXT[3] = { "1/4", "1/2", "1 BAR" };
static const double RTG_BEATS[3] = { 1.0, 2.0, 4.0 };
static const char *const HLF_TXT[2] = { "1/2", "1/4" };
static const char *const KIL_TXT[3] = { "ALL", "LOW", "HIGH" };
static const char *const STU_TXT[3] = { "1/16", "1/32", "1/64" };
static const double STU_END[3] = { 0.25, 0.125, 0.0625 };
static const char *const REV_TXT[4] = { "1/4", "1/2", "1BAR", "FREE" };
static const double REV_BEATS[3] = { 1.0, 2.0, 4.0 };

/* fixed chain order; ring1 is written after index 3, ring2 after index 13 */
static const uint8_t CHAIN[PF_NFX] = { PF_BEATREP, PF_STUTTER, PF_RETRIG, PF_REVERSE,
    PF_PITCH, PF_HALF, PF_TSTOP, PF_LFO, PF_VINYL,
    PF_LP, PF_HP, PF_CRUSH, PF_GATE, PF_DUCK, PF_KILL, PF_DELAY, PF_FREEZE };
#define CH_A_END 4
#define CH_B_END 15

static void lane_free(lane_t *L) {
    free(L->ring); free(L->ring1); free(L->ring2);
    for (int i = 0; i < SB_N; i++) free(L->sb[i]);
}

perf_t *perf_create(void) {
    perf_t *p = (perf_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    int ok = 1;
    for (int i = 0; i < PF_NLANE; i++) {
        lane_t *L = &p->ln[i];
        L->ring = (float *)calloc((size_t)RING_N * 2, sizeof(float));
        L->ring1 = (float *)calloc((size_t)RING_N * 2, sizeof(float));
        L->ring2 = (float *)calloc((size_t)RING_N * 2, sizeof(float));
        for (int b = 0; b < SB_N; b++) { L->sb[b] = (float *)calloc((size_t)SL_N * 2, sizeof(float)); if (!L->sb[b]) ok = 0; }
        if (!L->ring || !L->ring1 || !L->ring2) ok = 0;
        atomic_init(&L->want, 0);
        L->beat = SR * 0.5; L->rng = 0x9E3779B9u + (uint32_t)i * 7919u;
        for (int f = 0; f < PF_NFX; f++) L->s[f].speed = 1.0f;
    }
    if (!ok) { perf_destroy(p); return NULL; }
    for (int f = 0; f < PF_NFX; f++) for (int k = 0; k < 3; k++) p->pct[f][k] = DEF[f][k];
    memset(p->au, 255, sizeof p->au);
    memset(p->last_step, 0xff, sizeof p->last_step);
    return p;
}
void perf_destroy(perf_t *p) {
    if (!p) return;
    for (int i = 0; i < PF_NLANE; i++) lane_free(&p->ln[i]);
    free(p);
}

/* ---- control thread: the stack ---- */
static void publish(perf_t *p) {
    uint32_t w[PF_NLANE] = { 0 };
    for (int i = 0; i < p->nst; i++) w[p->st[i].target] |= 1u << p->st[i].fx;
    for (int l = 0; l < PF_NLANE; l++) atomic_store(&p->ln[l].want, w[l]);
}
static void st_remove(perf_t *p, int i) {
    for (int j = i; j + 1 < p->nst; j++) p->st[j] = p->st[j + 1];
    p->nst--;
}
int perf_press(perf_t *p, int fx, int target, long now, int toggle) {
    if (!p || fx < 0 || fx >= PF_NFX) return PF_P_REFUSED;
    if (target < 0 || target >= PF_NLANE) target = 0;
    for (int i = 0; i < p->nst; i++)
        if (p->st[i].fx == fx) {
            if (!p->st[i].latched) return PF_P_REFUSED;       /* already down */
            st_remove(p, i); publish(p);                       /* a tap on an effect that is on switches it off */
            return PF_P_OFF;
        }
    if (p->nst >= PF_MAXSTACK) return PF_P_REFUSED;
    entry_t e = { fx, target, 0, 1, toggle ? 1 : 0, now };
    p->st[p->nst++] = e;
    publish(p);
    return PF_P_ON;
}
int perf_release(perf_t *p, int fx, long now) {
    if (!p) return 0;
    for (int i = p->nst - 1; i >= 0; i--)
        if (p->st[i].fx == fx && p->st[i].held) {
            entry_t *e = &p->st[i];
            e->held = 0;
            long dt = now - e->t0;
            if (e->tog && dt >= 0 && dt < (long)(SR * PF_HOLD_MS / 1000.0)) { e->latched = 1; publish(p); return 1; }   /* a tap: it stays on */
            st_remove(p, i);                                   /* a long press is momentary */
            publish(p);
            return 0;
        }
    return 0;
}
void perf_clear_target(perf_t *p, int target) {
    if (!p) return;
    for (int i = p->nst - 1; i >= 0; i--) if (p->st[i].target == target) st_remove(p, i);
    publish(p);
}
int perf_count(const perf_t *p) { return p ? p->nst : 0; }
int perf_entry(const perf_t *p, int i, int *fx, int *target, int *latched, int *held) {
    if (!p || i < 0 || i >= p->nst) return 0;
    if (fx) *fx = p->st[i].fx;
    if (target) *target = p->st[i].target;
    if (latched) *latched = p->st[i].latched;
    if (held) *held = p->st[i].held;
    return 1;
}
int perf_top_fx(const perf_t *p) { return p && p->nst ? p->st[p->nst - 1].fx : -1; }
int perf_held_count(const perf_t *p) { int n = 0; if (p) for (int i = 0; i < p->nst; i++) n += p->st[i].held; return n; }
unsigned perf_on_mask(const perf_t *p) { unsigned m = 0; if (p) for (int i = 0; i < p->nst; i++) m |= 1u << p->st[i].fx; return m; }
unsigned perf_latch_mask(const perf_t *p) { unsigned m = 0; if (p) for (int i = 0; i < p->nst; i++) if (p->st[i].latched) m |= 1u << p->st[i].fx; return m; }

/* ---- knobs ---- */
int  perf_pct(const perf_t *p, int fx, int k) { return fx >= 0 && fx < PF_NFX && k >= 0 && k < PF_NKNOB ? p->pct[fx][k] : 0; }
void perf_pct_set(perf_t *p, int fx, int k, int v) {
    if (fx >= 0 && fx < PF_NFX && k >= 0 && k < PF_NKNOB) p->pct[fx][k] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}
static int didx(const perf_t *p, int fx, int k) {
    int n = DISC[fx][k], i = (perf_pct(p, fx, k) * (n - 1) + 50) / 100;
    return i < 0 ? 0 : i > n - 1 ? n - 1 : i;
}
void perf_knob(perf_t *p, int fx, int k, int delta) {
    if (fx >= 0 && fx < PF_NFX && k >= 0 && k < 3) atomic_store(&p->touch[fx][k], 1);
    if (fx >= 0 && fx < PF_NFX && k >= 0 && k < 3 && DISC[fx][k]) {
        int n = DISC[fx][k], i = didx(p, fx, k) + (delta > 0 ? 1 : -1);
        i = i < 0 ? 0 : i > n - 1 ? n - 1 : i;
        perf_pct_set(p, fx, k, (i * 100 + (n - 1) / 2) / (n - 1));
        return;
    }
    perf_pct_set(p, fx, k, perf_pct(p, fx, k) + delta * 2);
}

/* ---- automation ---- */
void perf_auto_hold(perf_t *p, int track) {
    if (!p) return;
    if (track < 1 || track >= PF_NLANE) track = 0;
    if (track) for (int f = 0; f < PF_NFX; f++) for (int k = 0; k < 3; k++) atomic_store(&p->touch[f][k], 0);   /* only turns made DURING the hold arm a knob */
    atomic_store(&p->hold, track);
}
int perf_auto_holding(const perf_t *p) { return p ? atomic_load(&((perf_t *)p)->hold) : 0; }
static void au_put(perf_t *p, int t, int fx, int k, int step, uint8_t v) {
    uint8_t *c = &p->au[t][fx][k][step];
    if (*c == 255 && v != 255) p->aucnt[t][fx][k]++;
    else if (*c != 255 && v == 255) p->aucnt[t][fx][k]--;
    *c = v;
}
void perf_auto_clear(perf_t *p, int track, int fx, int knob) {
    if (!p) return;
    for (int t = 1; t < PF_NLANE; t++) {
        if (track >= 1 && track != t) continue;
        for (int f = 0; f < PF_NFX; f++) {
            if (fx >= 0 && fx != f) continue;
            for (int k = 0; k < 3; k++) {
                if (knob >= 0 && knob != k) continue;
                memset(p->au[t][f][k], 255, PF_AU_N); p->aucnt[t][f][k] = 0; p->last_step[f][k] = -1;
            }
        }
    }
}
int perf_auto_has(const perf_t *p, int track, int fx, int knob) {
    if (!p) return 0;
    int n = 0;
    for (int t = 1; t < PF_NLANE; t++) {
        if (track >= 1 && track != t) continue;
        for (int f = 0; f < PF_NFX; f++) {
            if (fx >= 0 && fx != f) continue;
            for (int k = 0; k < 3; k++) if (knob < 0 || knob == k) n += p->aucnt[t][f][k];
        }
    }
    return n;
}
int perf_auto_get(const perf_t *p, int track, int fx, int knob, uint8_t *out) {
    if (!p || track < 1 || track >= PF_NLANE || fx < 0 || fx >= PF_NFX || knob < 0 || knob > 2) return 0;
    memcpy(out, p->au[track][fx][knob], PF_AU_N);
    return p->aucnt[track][fx][knob];
}
void perf_auto_put(perf_t *p, int track, int fx, int knob, const uint8_t *in) {
    if (!p || track < 1 || track >= PF_NLANE || fx < 0 || fx >= PF_NFX || knob < 0 || knob > 2) return;
    for (int i = 0; i < PF_AU_N; i++) au_put(p, track, fx, knob, i, in[i] == 255 || in[i] <= 100 ? in[i] : 255);
}

static int au_step(const double *tpos, const double *tlen, int t) {
    if (t < 1 || t >= PF_NLANE || tlen[t] < 64.0) return -1;
    double ph = tpos[t] / tlen[t]; ph -= floor(ph);
    int st = (int)(ph * PF_AU_N);
    return st < 0 ? 0 : st >= PF_AU_N ? PF_AU_N - 1 : st;
}
/* audio thread, once per block. WRITE: a knob turned while a REC pad is held (hold = that track) is written every
 * block at the step under that track's playhead (gaps since the last block filled) until the hold ends. PLAY: every
 * other knob of an effect that is on copies its stored step into the live value, from the track the effect is bound to. */
static void auto_block(perf_t *p, const double *tpos, const double *tlen) {
    int hold = atomic_load(&p->hold);
    int hs = hold ? au_step(tpos, tlen, hold) : -1;
    int lane_of[PF_NFX];
    for (int f = 0; f < PF_NFX; f++) lane_of[f] = -1;
    for (int l = 0; l < PF_NLANE; l++) {
        uint32_t w = atomic_load(&p->ln[l].want);
        for (int f = 0; f < PF_NFX; f++) if ((w >> f) & 1u) lane_of[f] = l;
    }
    for (int fx = 0; fx < PF_NFX; fx++) {
        for (int k = 0; k < 3; k++) {
            int touched = atomic_exchange(&p->touch[fx][k], 0);
            if (lane_of[fx] < 0) { p->armed[fx][k] = 0; p->last_step[fx][k] = -1; continue; }
            if (hs < 0) { p->armed[fx][k] = 0; p->last_step[fx][k] = -1; }
            else if (touched) p->armed[fx][k] = 1;
            if (p->armed[fx][k]) {
                uint8_t v = p->pct[fx][k] > 100 ? 100 : p->pct[fx][k];
                int ls = p->last_step[fx][k];
                au_put(p, hold, fx, k, hs, v);
                if (ls >= 0) for (int s = ls; s != hs; s = (s + 1) % PF_AU_N) au_put(p, hold, fx, k, s, v);
                p->last_step[fx][k] = hs;
                continue;
            }
            int owner = lane_of[fx];
            if (owner == 0) { for (int t = 1; t < PF_NLANE; t++) if (p->aucnt[t][fx][k]) { owner = t; break; } }
            if (owner < 1 || !p->aucnt[owner][fx][k]) continue;
            int st = au_step(tpos, tlen, owner);
            if (st < 0) continue;
            uint8_t v = p->au[owner][fx][k][st];
            if (v != 255) p->pct[fx][k] = v;
        }
    }
}

const char *perf_name(int fx) { return fx >= 0 && fx < PF_NFX ? NAME[fx] : "---"; }
const char *perf_short(int fx) { return fx >= 0 && fx < PF_NFX ? SHORT[fx] : "---"; }
int perf_nparams(int fx) { return fx >= 0 && fx < PF_NFX ? NP[fx] : 0; }
const char *perf_plabel(int fx, int k) { return fx >= 0 && fx < PF_NFX && k >= 0 && k < 3 ? PL[fx][k] : ""; }
int perf_family(int fx) {
    switch (fx) {
    case PF_BEATREP: return 0;
    case PF_TSTOP: case PF_REVERSE: case PF_VINYL: case PF_HALF: return 1;
    case PF_PITCH: case PF_CRUSH: case PF_LP: case PF_HP: return 2;
    case PF_STUTTER: case PF_GATE: case PF_RETRIG: case PF_LFO: case PF_DUCK: return 3;
    case PF_DELAY: case PF_FREEZE: return 4;
    default: return 5;
    }
}

/* Decoded value of knob k of effect fx:
 *  BEATREP 0 rate index 0..5, 1 gain kept per repeat 1..0.35, 2 pitch drop st/repeat 0..12
 *  STUTTER 0 beats 1..4, 1 end rate index 0..2 (1/16 1/32 1/64), 2 curve 0 linear .. 1 exponential
 *  PITCH 0 semitones -12..12, 1 fine cents -50..50, 2 mix 0..1
 *  CRUSH 0 bits 16..2, 1 hold 1..32, 2 mix 0..1
 *  TAPE 0 beats .25..2, 1 curve 0 linear .. 1 steep, 2 wow 0..1 (on = stop, off = start back up over the same time)
 *  LFO 0 rate index 0..9 (1/16 1/8T 1/8 1/8. 1/4 1/4. 1/2 1 2 4 bars), 1 destination 0..3 (FILT VOL PAN PTCH), 2 depth 0..1 (shape: sine)
 *  REVERSE 0 window index 0..3 (1/4 1/2 1 bar, free), 1 mix 0..1
 *  VINYL 0 floor speed .92..30, 1 crackle 0..1, 2 wow 0..1
 *  LP/HP 0 cutoff Hz 30..18k (exp), 1 reso 0..1, 2 slope index 0..1 (12 / 24 dB)
 *  GATE 0 rate index 0..5, 1 depth 0..1, 2 swing 0..1
 *  DELAY 0 time index 0..4, 1 feedback 0..0.88, 2 echo level 0..1
 *  FREEZE 0 size index 0..4, 1 mix 0..1, 2 decay 0..1 (0 = holds forever)
 *  RETRIG 0 index 0..2, 1 volume decay per retrig 0..1, 2 pitch drop per retrig 0..4 st
 *  HALF 0 index 0..1, 1 mix 0..1
 *  KILL 0 index 0..2 (ALL, LOW, HIGH), 1 fade ms 2..500 (exp), 2 split Hz 60..12k (exp) */
float perf_pval(const perf_t *p, int fx, int k) {
    float v = (float)perf_pct(p, fx, k) / 100.0f;
    switch (fx) {
    case PF_BEATREP: return k == 0 ? (float)(int)(v * 5.999f) : k == 1 ? 1.0f - v * 0.65f : v * 12.0f;
    case PF_STUTTER: return k == 0 ? 1.0f + v * 3.0f : k == 1 ? (float)didx(p, fx, 1) : v;
    case PF_PITCH:   return k == 0 ? (float)(int)lroundf(-12.0f + v * 24.0f) : k == 1 ? (float)(int)lroundf(-50.0f + v * 100.0f) : v;
    case PF_CRUSH:   return k == 0 ? (float)(16 - (int)lroundf(v * 14.0f)) : k == 1 ? 1.0f + (float)(int)lroundf(v * 31.0f) : v;
    case PF_TSTOP:   return k == 0 ? 0.25f + v * 1.75f : v;
    case PF_LFO:     return k < 2 ? (float)didx(p, fx, k) : v;
    case PF_REVERSE: return k == 0 ? (float)didx(p, fx, 0) : v;
    case PF_VINYL:   return k == 0 ? 0.92f - v * 0.62f : v;
    case PF_LP: case PF_HP: return k == 0 ? 30.0f * powf(600.0f, v) : k == 1 ? v : (float)didx(p, fx, 2);
    case PF_GATE: case PF_DUCK: return k == 0 ? (float)didx(p, fx, 0) : v;
    case PF_DELAY:   return k == 0 ? (float)didx(p, fx, 0) : k == 1 ? v * 0.88f : v;
    case PF_FREEZE:  return k == 0 ? (float)didx(p, fx, 0) : v;
    case PF_RETRIG:  return k == 0 ? (float)didx(p, fx, 0) : k == 1 ? v : v * 4.0f;
    case PF_HALF:    return k == 0 ? (float)didx(p, fx, 0) : v;
    case PF_KILL:    return k == 0 ? (float)didx(p, fx, 0) : k == 1 ? 2.0f * powf(250.0f, v) : 60.0f * powf(200.0f, v);
    default: return 0.0f;
    }
}

static void pcent(float v, char *out, int n) { snprintf(out, (size_t)n, "%d%%", (int)lroundf(v * 100.0f)); }
void perf_ptext(const perf_t *p, int fx, int k, char *out, int n) {
    float v = perf_pval(p, fx, k);
    out[0] = 0;
    switch (fx) {
    case PF_BEATREP:
        if (k == 0) snprintf(out, (size_t)n, "%s", RATE_TXT[(int)v]);
        else if (k == 1) snprintf(out, (size_t)n, "%d%%", (int)lroundf((1.0f - v) * 100.0f / 0.65f));
        else snprintf(out, (size_t)n, "-%dST", (int)lroundf(v));
        break;
    case PF_STUTTER:
        if (k == 0) snprintf(out, (size_t)n, "%.1fB", (double)v);
        else if (k == 1) snprintf(out, (size_t)n, "%s", STU_TXT[(int)v]);
        else if (v < 0.02f) snprintf(out, (size_t)n, "LIN"); else if (v > 0.98f) snprintf(out, (size_t)n, "EXP"); else pcent(v, out, n);
        break;
    case PF_PITCH:
        if (k == 0) snprintf(out, (size_t)n, "%+dST", (int)v);
        else if (k == 1) snprintf(out, (size_t)n, "%+dC", (int)v);
        else pcent(v, out, n);
        break;
    case PF_CRUSH:   if (k == 0) snprintf(out, (size_t)n, "%dB", (int)v); else if (k == 1) snprintf(out, (size_t)n, "/%d", (int)v); else pcent(v, out, n); break;
    case PF_LFO:
        if (k == 0) snprintf(out, (size_t)n, "%s", LFO_TXT[(int)v]); else if (k == 1) snprintf(out, (size_t)n, "%s", LFO_DST[(int)v]); else pcent(v, out, n);
        break;
    case PF_TSTOP:
        if (k == 0) snprintf(out, (size_t)n, "%.2gB", (double)v);
        else if (k == 1) { if (v < 0.02f) snprintf(out, (size_t)n, "LIN"); else if (v > 0.98f) snprintf(out, (size_t)n, "EXP"); else pcent(v, out, n); }
        else pcent(v, out, n);
        break;
    case PF_REVERSE: if (k == 0) snprintf(out, (size_t)n, "%s", REV_TXT[(int)v]); else pcent(v, out, n); break;
    case PF_VINYL:   pcent(v, out, n); break;
    case PF_LP: case PF_HP:
        if (k == 0) { if (v >= 1000.0f) snprintf(out, (size_t)n, "%.1fK", (double)(v / 1000.0f)); else snprintf(out, (size_t)n, "%dHZ", (int)lroundf(v)); }
        else if (k == 1) pcent(v, out, n);
        else snprintf(out, (size_t)n, "%dDB", (int)v ? 24 : 12);
        break;
    case PF_GATE: case PF_DUCK:
        if (k == 0) snprintf(out, (size_t)n, "%s", RATE_TXT[(int)v]); else pcent(v, out, n);
        break;
    case PF_DELAY:
        if (k == 0) snprintf(out, (size_t)n, "%s", DLY_TXT[(int)v]);
        else if (k == 1) snprintf(out, (size_t)n, "%d%%", (int)lroundf(v / 0.88f * 100.0f));
        else pcent(v, out, n);
        break;
    case PF_FREEZE:  if (k == 0) snprintf(out, (size_t)n, "%s", FRZ_TXT[(int)v]); else pcent(v, out, n); break;
    case PF_RETRIG:
        if (k == 0) snprintf(out, (size_t)n, "%s", RTG_TXT[(int)v]);
        else if (k == 1) pcent(v, out, n);
        else snprintf(out, (size_t)n, "-%.1fST", (double)v);
        break;
    case PF_HALF:    if (k == 0) snprintf(out, (size_t)n, "%s", HLF_TXT[(int)v]); else pcent(v, out, n); break;
    case PF_KILL:
        if (k == 0) snprintf(out, (size_t)n, "%s", KIL_TXT[(int)v]);
        else if (k == 1) snprintf(out, (size_t)n, "%dMS", (int)lroundf(v));
        else if ((int)perf_pval(p, fx, 0) == 0) snprintf(out, (size_t)n, "-");
        else if (v >= 1000.0f) snprintf(out, (size_t)n, "%.1fK", (double)(v / 1000.0f)); else snprintf(out, (size_t)n, "%dHZ", (int)lroundf(v));
        break;
    default: break;
    }
}

int perf_lane_active(const perf_t *p, int lane) { return p && lane >= 0 && lane < PF_NLANE ? (int)p->ln[lane].act : 0; }

void perf_vis(const perf_t *p, perf_vis_t *v) {
    memset(v, 0, sizeof *v);
    v->fx = -1; v->speed = 1.0f;
    if (!p) return;
    if (p->nst) {
        const entry_t *e = &p->st[p->nst - 1];
        const lane_t *L = &p->ln[e->target];
        const slot_t *s = &L->s[e->fx];
        v->fx = e->fx; v->target = e->target; v->held = e->held; v->latched = e->latched;
        v->gate = s->g; v->k = s->k; v->beat = L->beat; v->rep = s->rep; v->speed = s->speed;
        v->phase = s->phase; v->aux = s->aux; v->level = L->lvl;
        for (int i = p->nst - 2; i >= 0; i--) v->other[v->nother++] = p->st[i].fx;
    }
}

/* ---- reading the rings ---- */
static inline void ring_at(const float *ring, double pos, float *l, float *r) {
    int64_t i = (int64_t)floor(pos);
    float fr = (float)(pos - (double)i);
    uint32_t a = (uint32_t)i & RING_MASK, b = (uint32_t)(i + 1) & RING_MASK;
    *l = ring[a * 2] + (ring[b * 2] - ring[a * 2]) * fr;
    *r = ring[a * 2 + 1] + (ring[b * 2 + 1] - ring[a * 2 + 1]) * fr;
}
static inline void slice_at(const float *sl, double pos, int64_t len, float *l, float *r) {
    if (pos < 0.0) pos = 0.0;
    if (pos > (double)(len - 1)) pos = (double)(len - 1);
    int64_t i = (int64_t)pos;
    int64_t j = i + 1 < len ? i + 1 : i;
    float fr = (float)(pos - (double)i);
    *l = sl[i * 2] + (sl[j * 2] - sl[i * 2]) * fr;
    *r = sl[i * 2 + 1] + (sl[j * 2 + 1] - sl[i * 2 + 1]) * fr;
}
static void copy_slice(const lane_t *L, const float *ring, float *dst, int64_t from_back, int64_t len) {     /* ring frames [wc-from_back, +len) */
    if (len > SL_N) len = SL_N;
    for (int64_t i = 0; i < len; i++) {
        uint32_t a = (uint32_t)((int64_t)L->wc - from_back + i) & RING_MASK;
        dst[i * 2] = ring[a * 2]; dst[i * 2 + 1] = ring[a * 2 + 1];
    }
}
static float noise(lane_t *L) {
    L->rng ^= L->rng << 13; L->rng ^= L->rng >> 17; L->rng ^= L->rng << 5;
    return (float)(int32_t)L->rng / 2147483648.0f;
}

/* TPT state-variable filter coefficients */
static void svf_set(slot_t *s, int ci, float fc, float k) {
    float g = tanf((float)M_PI * fc / (float)MARK_SR);
    svfc *c = &s->c[ci];
    c->kk = k; c->a1 = 1.0f / (1.0f + g * (g + k)); c->a2 = g * c->a1; c->a3 = g * c->a2;
}
static inline void svf_run(slot_t *s, int ci, int ch, int st, float x, float *lp, float *hp) {
    const svfc *c = &s->c[ci];
    float v3 = x - s->ic2[ch][st];
    float v1 = c->a1 * s->ic1[ch][st] + c->a2 * v3;
    float v2 = s->ic2[ch][st] + c->a2 * s->ic1[ch][st] + c->a3 * v3;
    s->ic1[ch][st] = 2.0f * v1 - s->ic1[ch][st];
    s->ic2[ch][st] = 2.0f * v2 - s->ic2[ch][st];
    *lp = v2; *hp = x - c->kk * v1 - v2;
}

static void engage(perf_t *p, lane_t *L, int fx) {
    slot_t *s = &L->s[fx];
    if (fx == PF_DELAY && s->act) { s->held = 1; return; }            /* the tail is still ringing: just feed it again */
    float g0 = s->act ? s->g : 0.0f;
    memset(s, 0, sizeof *s);
    s->act = 1; s->held = 1; s->g = g0; s->last_rep = -1; s->rep_gain = 1.0f; s->rep_rho = 1.0f;
    s->speed = 1.0f; s->rv0 = L->wc; s->gg = 1.0f; s->kmode = -1;
    L->act |= 1u << fx;
    if (fx == PF_BEATREP) {
        int idx = (int)perf_pval(p, fx, 0);
        int64_t Ln = (int64_t)(L->beat * RATE_MUL[idx] + 0.5);
        if (Ln < 256) Ln = 256;
        if (Ln > SL_N) Ln = SL_N;
        s->L = Ln;
        s->p0 = (int64_t)fmod(L->grid < 0.0 ? 0.0 : L->grid, (double)Ln);
        copy_slice(L, L->ring, L->sb[SB_REP], s->p0 + Ln, Ln);          /* the slice that ended on the last grid line */
    } else if (fx == PF_STUTTER) {
        int64_t Ln = (int64_t)L->beat;
        if (Ln < 256) Ln = 256;
        if (Ln > SL_N) Ln = SL_N;
        s->L = Ln;
        copy_slice(L, L->ring, L->sb[SB_STU], Ln, Ln);                  /* the last quarter note */
    } else if (fx == PF_FREEZE) {
        int64_t Ln = (int64_t)(L->beat * FRZ_MUL[(int)perf_pval(p, fx, 0)] + 0.5);
        if (Ln < 256) Ln = 256;
        if (Ln > SL_N) Ln = SL_N;
        s->L = Ln; s->env = 1.0f;
        copy_slice(L, L->ring2, L->sb[SB_FRZ], Ln, Ln);                 /* what reached the end of the chain */
    } else if (fx == PF_DELAY) {
        if (L->dly_dirty) { memset(L->sb[SB_DLY], 0, (size_t)SL_N * 2 * sizeof(float)); L->dly_dirty = 0; }
        s->dly = -1.0;
    } else if (fx == PF_LP || fx == PF_HP) {
        s->fcs = perf_pval(p, fx, 0); s->fn = 0; s->slope = perf_pval(p, fx, 2);
    } else if (fx == PF_KILL) {
        s->fcs = perf_pval(p, fx, 2);
    }
}

void perf_block_begin(perf_t *p, double grid_pos, double beat_frames, const double *tpos, const double *tlen) {
    if (!p) return;
    auto_block(p, tpos, tlen);
    for (int l = 0; l < PF_NLANE; l++) {
        lane_t *L = &p->ln[l];
        L->grid = grid_pos; L->bn = 0;
        if (beat_frames > 1000.0) L->beat = beat_frames;
        uint32_t w = atomic_load(&L->want);
        for (int fx = 0; fx < PF_NFX; fx++) {
            slot_t *s = &L->s[fx];
            if ((w >> fx) & 1u) { if (!s->held || !s->act) engage(p, L, fx); }
            else if (s->held) s->held = 0;
            if (fx == PF_KILL && s->act) s->fk = (float)(1.0 / ((double)perf_pval(p, fx, 1) * 0.001 * SR));
        }
    }
}

/* granular pitch shifter: two taps on a ring, delay swept by (1 - ratio) per frame */
static void gran(const lane_t *L, slot_t *s, const float *ring, double ratio, float *wl, float *wr) {
    s->ph += (1.0 - ratio) / GRAN_W;
    s->ph -= floor(s->ph);
    float sl = 0.0f, sr = 0.0f;
    for (int t = 0; t < 2; t++) {
        double ph = s->ph + 0.5 * t;
        if (ph >= 1.0) ph -= 1.0;
        float w = (float)sin(M_PI * ph); w *= w;
        float a, b;
        ring_at(ring, (double)L->wc - (ph * GRAN_W + 2.0), &a, &b);
        sl += a * w; sr += b * w;
    }
    *wl = sl; *wr = sr;
}

/* read `ring` at an integer delay with a short crossfade when the delay jumps */
/* the same, plus a continuous extra delay `off` (frames, >= 0) that makes the read slower than real time */
static void jump_read_off(const lane_t *L, slot_t *s, const float *ring, double d, double off, float *wl, float *wr) {
    int64_t di = (int64_t)(d + 0.5);
    if (di > RING_N - 1024 - 65536) di = RING_N - 1024 - 65536;
    if (di < 0) di = 0;
    if (s->dprev < 0.0) s->dprev = (double)di;
    if ((double)di != s->dprev && s->xf <= 0) { s->xf = XFADE; s->d = s->offp; }
    float l, r;
    ring_at(ring, (double)L->wc - ((double)di + off), &l, &r);
    if (s->xf > 0) {
        float c, e;
        ring_at(ring, (double)L->wc - (s->dprev + s->d), &c, &e);
        float m = (float)s->xf / (float)XFADE;
        l += (c - l) * m; r += (e - r) * m;
        if (--s->xf <= 0) s->dprev = (double)di;
    } else s->dprev = (double)di;
    s->offp = off;
    *wl = l; *wr = r;
}
static void render(perf_t *p, lane_t *L, slot_t *s, int fx, float il, float ir, float *wl, float *wr) {
    double k = s->k;
    switch (fx) {
    case PF_BEATREP: {
        int64_t t = s->p0 + (int64_t)k, Ln = s->L;
        int rep = (int)(t / Ln);
        double pos = (double)(t % Ln);
        if (rep != s->last_rep) {
            s->last_rep = rep; s->rep = rep;
            s->rep_gain = powf(perf_pval(p, PF_BEATREP, 1), (float)(rep > 40 ? 40 : rep));
            s->rep_rho = exp2f(-perf_pval(p, PF_BEATREP, 2) * (float)(rep > 40 ? 40 : rep) / 12.0f);
            if (s->rep_rho < 0.05f) s->rep_rho = 0.05f;
        }
        float a, b;
        slice_at(L->sb[SB_REP], pos * s->rep_rho, Ln, &a, &b);
        float e = (float)(pos < (double)(Ln - 1) - pos ? pos : (double)(Ln - 1) - pos) / EDGE;
        e = e > 1.0f ? 1.0f : e;
        float g = s->rep_gain * e;
        *wl = a * g; *wr = b * g;
        s->speed = s->rep_rho;
        s->phase = (float)(pos / (double)Ln);
        break;
    }
    case PF_STUTTER: {
        double total = perf_pval(p, PF_STUTTER, 0) * L->beat;
        if (s->rep_pos >= s->rep_len) {
            double u = k / total; if (u > 1.0) u = 1.0;
            double er = STU_END[(int)perf_pval(p, PF_STUTTER, 1)], cv = (double)perf_pval(p, PF_STUTTER, 2);
            double f = (1.0 - cv) * (1.0 + (er - 1.0) * u) + cv * pow(er, u);          /* linear -> exponential shrink of the repeat */
            s->rep_len = (int)((double)s->L * f);
            if (s->rep_len < 64) s->rep_len = 64;
            s->rep_pos = 0; s->rep++;
        }
        float a, b;
        slice_at(L->sb[SB_STU], (double)(s->L - s->rep_len + s->rep_pos), s->L, &a, &b);
        float e1 = (float)s->rep_pos / EDGE, e2 = (float)(s->rep_len - 1 - s->rep_pos) / EDGE;
        float e = e1 < e2 ? e1 : e2; e = e > 1.0f ? 1.0f : e;
        *wl = a * e; *wr = b * e;
        s->rep_pos++;
        s->speed = (float)s->L / (float)s->rep_len;
        s->phase = (float)s->rep_pos / (float)s->rep_len;
        break;
    }
    case PF_RETRIG: {                 /* the first quarter of the grid unit plays again and again, jumping back to the unit start */
        int idx = (int)perf_pval(p, fx, 0);
        double T = L->beat * RTG_BEATS[idx], R = T / 4.0;
        double t = L->grid + (double)L->bn; if (t < 0.0) t = 0.0;
        double tau = fmod(t, T);
        double jj = floor(tau / R);
        float semi = perf_pval(p, fx, 2), dec = perf_pval(p, fx, 1);
        double rho = exp2(-(double)semi * jj / 12.0);                    /* each retrig plays a little lower */
        double off = (1.0 - rho) * (tau - jj * R);
        jump_read_off(L, s, L->ring, R * jj, off, wl, wr);
        float tg = 1.0f;
        for (int i = 0; i < (int)jj; i++) tg *= 1.0f - 0.3f * dec;        /* and a little quieter */
        s->gg += (tg - s->gg) * 0.02f;
        *wl *= s->gg; *wr *= s->gg;
        s->rep = (int)jj; s->phase = (float)((tau - jj * R) / R);
        break;
    }
    case PF_PITCH: {
        double ratio = pow(2.0, ((double)perf_pval(p, PF_PITCH, 0) + (double)perf_pval(p, PF_PITCH, 1) / 100.0) / 12.0);
        float mix = perf_pval(p, PF_PITCH, 2), a, b;
        s->speed = (float)ratio;
        gran(L, s, L->ring1, ratio, &a, &b);
        *wl = il + (a - il) * mix; *wr = ir + (b - ir) * mix;
        break;
    }
    case PF_HALF: {                   /* tape-slow read of the history; the lag returns to 0 every cycle, so it stays in time */
        int q = (int)perf_pval(p, fx, 0);
        double r = q ? 0.25 : 0.5, C = L->beat * (q ? 4.0 : 2.0);
        double t = L->grid + (double)L->bn; if (t < 0.0) t = 0.0;
        double tau = fmod(t, C);
        double d = 2.0 + (1.0 - r) * tau;
        if (tau < s->tau_prev) s->xf = XFADE;           /* wrapped: fade from the (still growing) old lag back to live */
        s->tau_prev = tau;
        float a, b;
        ring_at(L->ring1, (double)L->wc - (d > RING_N - 1024 ? RING_N - 1024 : d), &a, &b);
        if (s->xf > 0) {
            float c, e;
            double dold = 2.0 + (1.0 - r) * (tau + C);
            ring_at(L->ring1, (double)L->wc - (dold > RING_N - 1024 ? RING_N - 1024 : dold), &c, &e);
            float m = (float)s->xf / (float)XFADE; s->xf--;
            a += (c - a) * m; b += (e - b) * m;
        }
        float mix = perf_pval(p, fx, 1);
        *wl = il + (a - il) * mix; *wr = ir + (b - ir) * mix;
        s->speed = (float)r; s->phase = (float)(tau / C);
        break;
    }
    case PF_CRUSH: {
        float bits = perf_pval(p, PF_CRUSH, 0), ds = perf_pval(p, PF_CRUSH, 1), mix = perf_pval(p, PF_CRUSH, 2);
        float q = exp2f(16.0f - bits);
        if (s->hold <= 0.0) {
            s->hold += (double)ds;
            s->hl = floorf(il / q + 0.5f) * q; s->hr = floorf(ir / q + 0.5f) * q;
        }
        s->hold -= 1.0;
        *wl = il + (s->hl - il) * mix; *wr = ir + (s->hr - ir) * mix;
        break;
    }
    case PF_TSTOP: {                  /* TAPE: on = the tape stops; off = it spools back up to speed in the same time */
        double T = (double)perf_pval(p, PF_TSTOP, 0) * L->beat;
        float curve = perf_pval(p, PF_TSTOP, 1), wow = perf_pval(p, PF_TSTOP, 2);
        if (s->ph2 == 0) {
            double u = k / T; if (u > 1.0) u = 1.0;
            double sp = pow(1.0 - u, 1.0 + 2.0 * (double)curve);                     /* curve: linear .. steep */
            if (sp > 0.0) {                                                          /* wow: speed dips, never above 1 */
                s->lfo += 2.0 * M_PI * 5.0 / SR;
                sp *= 1.0 - (double)wow * 0.08 * (0.5 + 0.5 * sin(s->lfo));
            }
            s->d += 1.0 - sp;
            if (s->d > RING_N / 2) s->d = RING_N / 2;
            s->speed = (float)sp;
            if (sp <= 0.0) { *wl = *wr = 0.0f; s->hl = s->hr = 0.0f; break; }
            float a, b;
            ring_at(L->ring1, (double)L->wc - s->d, &a, &b);
            float gs = sqrtf((float)sp);
            *wl = a * gs; *wr = b * gs;
            s->hl = *wl; s->hr = *wr;
        } else {                                                                     /* start: from the speed it had, up to 1 */
            double u = s->k2 / T; s->k2 += 1.0;
            if (u >= 1.0) { u = 1.0; s->ph2 = 2; }
            double uu = (double)s->u0 + (1.0 - (double)s->u0) * pow(u, 1.0 + 2.0 * (double)curve);
            s->lfo += 2.0 * M_PI * 5.0 / SR;
            double ratio = uu * (1.0 - (double)wow * 0.08 * (0.5 + 0.5 * sin(s->lfo)));
            if (ratio < 0.03) ratio = 0.03;
            s->speed = (float)uu;
            float a, b;
            gran(L, s, L->ring1, ratio, &a, &b);
            a *= (float)uu; b *= (float)uu;                                          /* amplitude ramps with the speed */
            if (s->xf > 0) {                                                         /* hand over from the stopping head without a step */
                float m = (float)s->xf / (float)XFADE; s->xf--;
                a += (s->hl - a) * m; b += (s->hr - b) * m;
            }
            *wl = a; *wr = b;
        }
        break;
    }
    case PF_LFO: {                    /* tempo-synced sine on the loop grid: FILT (LP sweep) / VOL / PAN / PTCH */
        double P = L->beat * LFO_BEATS[(int)perf_pval(p, fx, 0)];
        double t = L->grid + (double)L->bn; if (t < 0.0) t = 0.0;
        double ph = fmod(t, P) / P;
        int dst = (int)perf_pval(p, fx, 1);
        float depth = perf_pval(p, fx, 2) * (k < 0.03 * SR ? (float)(k / (0.03 * SR)) : 1.0f);   /* depth eases in over 30 ms */
        float m = sinf(2.0f * (float)M_PI * (float)ph), c = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)ph);
        s->phase = (float)ph; s->speed = m; s->rep = dst;
        if (dst == 0) {
            if ((s->fn++ & 15) == 0) {                                               /* open at phase 0, closed at the half cycle */
                float fc = 18000.0f * powf(90.0f / 18000.0f, depth * c);
                svf_set(s, 0, fc, 1.0f / (0.8f + 2.2f * depth));
                svf_set(s, 1, fc, 1.4142f);
                s->aux = logf(fc / 30.0f) / logf(600.0f);
            }
            float lp, hp, lq, hq, l2, h2, l3, h3;
            svf_run(s, 0, 0, 0, il, &lp, &hp); svf_run(s, 0, 1, 0, ir, &lq, &hq);
            svf_run(s, 1, 0, 1, lp, &l2, &h2); svf_run(s, 1, 1, 1, lq, &l3, &h3);
            *wl = l2; *wr = l3;
        } else if (dst == 1) {
            float g = 1.0f - depth * c;
            *wl = il * g; *wr = ir * g; s->aux = g;
        } else if (dst == 2) {
            float ang = (depth * m + 1.0f) * (float)M_PI * 0.25f;
            *wl = il * cosf(ang) * 1.41421356f; *wr = ir * sinf(ang) * 1.41421356f; s->aux = 0.5f + 0.5f * depth * m;
        } else {
            double ratio = pow(2.0, (double)(depth * m) * 7.0 / 12.0);
            gran(L, s, L->ring1, ratio, wl, wr);
            s->aux = 0.5f + 0.5f * depth * m;
        }
        break;
    }
    case PF_REVERSE: {
        int64_t M = RING_N / 2 - 256;
        int wi = (int)perf_pval(p, fx, 0);
        if (wi < 3) { int64_t W = (int64_t)(L->beat * REV_BEATS[wi]); if (W < 512) W = 512; if (W < M) M = W; }
        float mix = perf_pval(p, fx, 1);
        int64_t j = (int64_t)k % M;
        uint32_t a = (uint32_t)((int64_t)s->rv0 - j) & RING_MASK;
        float e1 = (float)j / EDGE, e2 = (float)(M - 1 - j) / EDGE;
        float e = e1 < e2 ? e1 : e2; e = e > 1.0f ? 1.0f : e;
        *wl = il + (L->ring[a * 2] * e - il) * mix; *wr = ir + (L->ring[a * 2 + 1] * e - ir) * mix;
        s->speed = -1.0f;
        break;
    }
    case PF_VINYL: {
        float floor_s = perf_pval(p, PF_VINYL, 0), cr = perf_pval(p, PF_VINYL, 1);
        double ratio = 1.0 - (1.0 - (double)floor_s) * (1.0 - exp(-k / (1.2 * SR)));
        s->lfo += 2.0 * M_PI * 0.9 / SR;
        ratio *= 1.0 + (double)perf_pval(p, PF_VINYL, 2) * 0.05 * sin(s->lfo);          /* wow */
        s->speed = (float)ratio;
        float a, b;
        gran(L, s, L->ring1, ratio, &a, &b);
        s->lpl += (a - s->lpl) * 0.45f; s->lpr += (b - s->lpr) * 0.45f;     /* dull the top, like worn vinyl */
        float hiss = noise(L) * 90.0f * cr, pop = 0.0f;
        if (cr > 0.0f && (noise(L) * 0.5f + 0.5f) < cr * 0.0006f) pop = noise(L) * 6000.0f * cr;
        *wl = s->lpl + hiss + pop; *wr = s->lpr + hiss + pop * 0.6f;
        break;
    }
    case PF_LP: case PF_HP: {
        if ((s->fn++ & 15) == 0) {                 /* the cutoff follows the knob smoothly; coefficients every 16 frames */
            float tgt = perf_pval(p, fx, 0), q = 0.7f + perf_pval(p, fx, 1) * perf_pval(p, fx, 1) * 11.0f;
            s->fcs += (tgt - s->fcs) * 0.18f;
            float fc = s->fcs > 18000.0f ? 18000.0f : s->fcs < 20.0f ? 20.0f : s->fcs;
            svf_set(s, 0, fc, 1.0f / q);
            svf_set(s, 1, fc, 1.4142f);            /* 2nd pole pair of the 24 dB slope: plain Butterworth, no extra resonance */
            s->lpl = 1.0f / (1.0f + 0.06f * (q - 0.7f));          /* level compensation */
            s->aux = logf(fc / 30.0f) / logf(600.0f);
        }
        s->slope += (perf_pval(p, fx, 2) - s->slope) * 0.002f;      /* 12 <-> 24 dB without a click: the 2nd stage always runs */
        float lp, hp, lq, hq, l2, h2, l3, h3;
        svf_run(s, 0, 0, 0, il, &lp, &hp); svf_run(s, 0, 1, 0, ir, &lq, &hq);
        float o1l = fx == PF_LP ? lp : hp, o1r = fx == PF_LP ? lq : hq;
        svf_run(s, 1, 0, 1, o1l, &l2, &h2); svf_run(s, 1, 1, 1, o1r, &l3, &h3);
        float o2l = fx == PF_LP ? l2 : h2, o2r = fx == PF_LP ? l3 : h3;
        *wl = (o1l + (o2l - o1l) * s->slope) * s->lpl; *wr = (o1r + (o2r - o1r) * s->slope) * s->lpl;
        break;
    }
    case PF_GATE: {                   /* beat-synced to the loop grid: open for the first half of each cell, swing delays the odd ones */
        int idx = (int)perf_pval(p, fx, 0);
        double Ln = L->beat * RATE_MUL[idx];
        double t = L->grid + (double)L->bn; if (t < 0.0) t = 0.0;
        double c = floor(t / Ln), u = t / Ln - c;
        float depth = perf_pval(p, fx, 1), swing = perf_pval(p, fx, 2);
        if (((int64_t)c) & 1) { u -= (double)swing * 0.4; if (u < 0.0) u += 1.0; }
        float tgt = u < 0.5 ? 1.0f : 1.0f - depth;
        s->gg += (tgt - s->gg) * 0.01f;
        *wl = il * s->gg; *wr = ir * s->gg;
        s->rep = (int)((int64_t)c & 0x7fff); s->phase = (float)u; s->speed = s->gg;
        break;
    }
    case PF_DUCK: {                   /* sidechain pump: ducks on every grid cell, then swells back (curve = how late) */
        int idx = (int)perf_pval(p, fx, 0);
        double Ln = L->beat * RATE_MUL[idx];
        double t = L->grid + (double)L->bn; if (t < 0.0) t = 0.0;
        double u = t / Ln; u -= floor(u);
        float depth = perf_pval(p, fx, 1), curve = perf_pval(p, fx, 2);
        float tgt = 1.0f - depth * (1.0f - powf((float)u, 0.5f + curve * 2.5f));
        s->gg += (tgt - s->gg) * 0.02f;
        *wl = il * s->gg; *wr = ir * s->gg;
        s->phase = (float)u; s->speed = s->gg;
        break;
    }
    case PF_KILL: {
        int m = (int)perf_pval(p, fx, 0);
        if (m != s->kmode) s->kmode = m;
        if (m == 0) { *wl = *wr = 0.0f; s->speed = 0.0f; break; }
        if ((s->fn++ & 15) == 0) {                 /* the split follows the knob smoothly */
            s->fcs += (perf_pval(p, fx, 2) - s->fcs) * 0.18f;
            svf_set(s, 0, s->fcs > 18000.0f ? 18000.0f : s->fcs < 20.0f ? 20.0f : s->fcs, 1.4142f);
        }
        float lp, hp, lq, hq, l2, h2;
        svf_run(s, 0, 0, 0, il, &lp, &hp); svf_run(s, 0, 1, 0, ir, &lq, &hq);            /* two stages: 24 dB/oct */
        if (m == 1) { svf_run(s, 0, 0, 1, hp, &l2, &h2); *wl = h2; svf_run(s, 0, 1, 1, hq, &l2, &h2); *wr = h2; }   /* LOW kill = the lows are gone */
        else { svf_run(s, 0, 0, 1, lp, &l2, &h2); *wl = l2; svf_run(s, 0, 1, 1, lq, &l2, &h2); *wr = l2; }          /* HIGH kill = the highs are gone */
        s->speed = 0.5f;
        break;
    }
    case PF_DELAY: {                  /* input only while held; the echoes ring out after the release */
        float *buf = L->sb[SB_DLY];
        int idx = (int)perf_pval(p, fx, 0);
        double tgt = L->beat * DLY_MUL[idx];
        if (tgt > (double)(SL_N - 8)) tgt = (double)(SL_N - 8);
        if (tgt < 64.0) tgt = 64.0;
        if (s->dly < 0.0) s->dly = tgt;
        s->dly += (tgt - s->dly) * 0.0004;
        double pos = (double)s->wi - s->dly;
        if (pos < 0.0) pos += (double)SL_N;
        int64_t i0 = (int64_t)pos; float fr = (float)(pos - (double)i0);
        int64_t i1 = (i0 + 1) & (SL_N - 1); i0 &= (SL_N - 1);
        float el = buf[i0 * 2] + (buf[i1 * 2] - buf[i0 * 2]) * fr, er = buf[i0 * 2 + 1] + (buf[i1 * 2 + 1] - buf[i0 * 2 + 1]) * fr;
        float fb = perf_pval(p, fx, 1);
        s->fl += (el - s->fl) * 0.55f; s->fr_ += (er - s->fr_) * 0.55f;      /* each repeat a little darker */
        int w0 = s->wi;
        buf[s->wi * 2] = il * s->g * 0.8f + s->fl * fb;
        buf[s->wi * 2 + 1] = ir * s->g * 0.8f + s->fr_ * fb;
        s->wi = (s->wi + 1) & (SL_N - 1);
        L->dly_dirty = 1;
        float wm = fabsf(buf[w0 * 2]) > fabsf(buf[w0 * 2 + 1]) ? fabsf(buf[w0 * 2]) : fabsf(buf[w0 * 2 + 1]);   /* what was just written */
        float m = fabsf(el) > fabsf(er) ? fabsf(el) : fabsf(er);
        if (wm > m) m = wm;
        s->env = m > s->env ? m : s->env * 0.99997f;
        if (s->held) s->rel = 0;
        else if (!s->rel) {                           /* released: the tail lasts for as many passes as it takes to fall under ~2 LSB */
            double passes = fb < 0.02f ? 1.0 : ceil(log(2.0 / (s->env < 4.0f ? 4.0 : (double)s->env)) / log((double)fb * 0.9)) + 1.0;
            if (passes < 1.0) passes = 1.0;
            if (passes > 60.0) passes = 60.0;
            s->tail = passes * s->dly + 2000.0; s->rel = 1;
        } else s->tail -= 1.0;
        float lvl = perf_pval(p, fx, 2);
        *wl = il + el * lvl; *wr = ir + er * lvl;
        s->aux = s->env / 16000.0f > 1.0f ? 1.0f : s->env / 16000.0f;
        s->phase = (float)(s->dly / (double)SL_N);
        break;
    }
    case PF_FREEZE: {
        const float *sl = L->sb[SB_FRZ];
        int64_t F = s->L / 4 < 441 ? s->L / 4 : 441, Lb = s->L - F;
        if (Lb < 64) { Lb = s->L; F = 0; }
        int64_t j = (int64_t)k % Lb;
        float a = sl[j * 2], b = sl[j * 2 + 1];
        if (j < F) {
            float w = (float)j / (float)F;
            a = a * w + sl[(Lb + j) * 2] * (1.0f - w); b = b * w + sl[(Lb + j) * 2 + 1] * (1.0f - w);
        }
        float mix = perf_pval(p, fx, 1), dc = perf_pval(p, fx, 2);
        s->env *= 1.0f - dc * dc * 4e-5f;                            /* decay: the held loop fades away slowly (0 = forever) */
        a *= s->env; b *= s->env;
        *wl = il + (a - il) * mix; *wr = ir + (b - ir) * mix;
        s->phase = (float)j / (float)Lb; s->speed = 1.0f;
        break;
    }
    default: *wl = il; *wr = ir; break;
    }
}

static void stage(perf_t *p, lane_t *L, int fx, float *x0, float *x1) {
    slot_t *s = &L->s[fx];
    int on = s->held;
    if (fx == PF_TSTOP) {                                      /* TAPE: let go = start back up, then end */
        if (!s->held && s->ph2 == 0) { s->ph2 = 1; s->k2 = 0.0; s->u0 = s->speed < 0.0f ? 0.0f : s->speed > 1.0f ? 1.0f : s->speed; s->xf = XFADE; }
        on = s->held || s->ph2 == 1;
    }
    float tgt = on ? 1.0f : 0.0f;
    float fi = fx == PF_KILL ? s->fk : FADE_IN, fo = fx == PF_KILL ? s->fk : FADE_OUT;
    if (s->g < tgt) { s->g += fi; if (s->g > tgt) s->g = tgt; }
    else if (s->g > tgt) { s->g -= fo; if (s->g < tgt) s->g = tgt; }
    float wl, wr;
    render(p, L, s, fx, *x0, *x1, &wl, &wr);
    if (fx == PF_DELAY) { *x0 = wl; *x1 = wr; }               /* the echoes are not gated: only the input is */
    else { *x0 += (wl - *x0) * s->g; *x1 += (wr - *x1) * s->g; }
    s->k += 1.0;
    if (!on && s->g <= 0.0f && (fx != PF_DELAY || s->tail <= 0.0)) {
        s->act = 0; s->g = 0.0f; s->speed = 1.0f; s->phase = 0.0f; s->aux = 0.0f; s->env = 0.0f; s->rel = 0; s->tail = 0.0;
        L->act &= ~(1u << fx);
    }
}

void perf_process(perf_t *p, int lane, float *l, float *r) {
    lane_t *L = &p->ln[lane >= 0 && lane < PF_NLANE ? lane : 0];
    uint32_t w = (uint32_t)L->wc & RING_MASK;
    L->ring[w * 2] = *l; L->ring[w * 2 + 1] = *r;
    float m = fabsf(*l) > fabsf(*r) ? fabsf(*l) : fabsf(*r);
    L->lvl = m / 32768.0f > L->lvl ? m / 32768.0f : L->lvl * 0.9997f;
    float a = *l, b = *r;
    if (L->act) for (int i = 0; i < CH_A_END; i++) if ((L->act >> CHAIN[i]) & 1u) stage(p, L, CHAIN[i], &a, &b);
    L->ring1[w * 2] = a; L->ring1[w * 2 + 1] = b;
    if (L->act) for (int i = CH_A_END; i < CH_B_END; i++) if ((L->act >> CHAIN[i]) & 1u) stage(p, L, CHAIN[i], &a, &b);
    L->ring2[w * 2] = a; L->ring2[w * 2 + 1] = b;
    if (L->act) for (int i = CH_B_END; i < PF_NFX; i++) if ((L->act >> CHAIN[i]) & 1u) stage(p, L, CHAIN[i], &a, &b);
    *l = a; *r = b;
    L->bn++; L->wc++;
}
