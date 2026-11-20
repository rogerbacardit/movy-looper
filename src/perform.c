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
    float ic1[2][2], ic2[2][2], fcs, a1, a2, a3, kk;
    int   fn, kmode;
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

typedef struct { int fx, target, latched, held; } entry_t;

struct perf {
    lane_t ln[PF_NLANE];
    uint8_t pct[PF_NFX][PF_NKNOB];
    entry_t st[PF_MAXSTACK];
    int    nst;
    int    tap_fx; long tap_t;
};

/* defaults; stepped parameters (DISC) hold idx * 100 / (n - 1) */
static const uint8_t DEF[PF_NFX][3] = {
    { 20, 20, 0 }, { 33, 0, 0 }, { 75, 0, 0 }, { 60, 20, 0 },
    { 40, 0, 0 }, { 40, 0, 0 }, { 0, 0, 0 }, { 40, 40, 0 },
    { 75, 25, 0 }, { 30, 25, 0 }, { 40, 85, 0 }, { 50, 40, 0 },
    { 50, 0, 0 }, { 50, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
};
static const uint8_t DISC[PF_NFX][3] = {          /* option count of a stepped knob, 0 = continuous */
    { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 }, { 0 },
    { 0 }, { 0 }, { 6, 0, 0 }, { 5, 0, 0 }, { 5, 0, 0 }, { 3, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 },
};
static const char *const NAME[PF_NFX] = { "BEAT REPEAT", "STUTTER", "PITCH", "BITCRUSH", "TAPE STOP", "TAPE START", "REVERSE", "VINYL",
    "FILTER LP", "FILTER HP", "GATE", "DELAY THROW", "ECHO FREEZE", "RETRIGGER", "HALF SPEED", "KILL" };
static const char *const SHORT[PF_NFX] = { "RPT", "STU", "PCH", "BIT", "STP", "STA", "BCK", "VIN",
    "LPF", "HPF", "GAT", "DLY", "FRZ", "RTG", "HLF", "KIL" };
static const int NP[PF_NFX] = { 3, 1, 1, 2, 1, 1, 0, 2, 2, 2, 3, 2, 1, 1, 1, 1 };
static const char *const PL[PF_NFX][3] = {
    { "RATE", "DECAY", "DROP" }, { "TIME", "", "" }, { "SEMI", "", "" }, { "BITS", "DOWN", "" },
    { "TIME", "", "" }, { "TIME", "", "" }, { "", "", "" }, { "BRAKE", "CRKL", "" },
    { "CUT", "RESO", "" }, { "CUT", "RESO", "" }, { "RATE", "DEPTH", "SWING" }, { "TIME", "FDBK", "" },
    { "SIZE", "", "" }, { "EVERY", "", "" }, { "SPEED", "", "" }, { "PART", "", "" },
};
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

/* fixed chain order; ring1 is written after index 3, ring2 after index 13 */
static const uint8_t CHAIN[PF_NFX] = { PF_BEATREP, PF_STUTTER, PF_RETRIG, PF_REVERSE,
    PF_PITCH, PF_HALF, PF_TSTOP, PF_TSTART, PF_VINYL,
    PF_LP, PF_HP, PF_CRUSH, PF_GATE, PF_KILL, PF_DELAY, PF_FREEZE };
#define CH_A_END 4
#define CH_B_END 14

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
    p->tap_fx = -1;
    for (int f = 0; f < PF_NFX; f++) for (int k = 0; k < 3; k++) p->pct[f][k] = DEF[f][k];
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
int perf_press(perf_t *p, int fx, int target, long now) {
    if (!p || fx < 0 || fx >= PF_NFX) return PF_P_REFUSED;
    if (target < 0 || target >= PF_NLANE) target = 0;
    for (int i = 0; i < p->nst; i++)
        if (p->st[i].fx == fx && p->st[i].latched) {          /* another tap of a latched step releases it */
            st_remove(p, i); p->tap_fx = -1; publish(p);
            return PF_P_UNLATCHED;
        }
    if (p->nst >= PF_MAXSTACK) return PF_P_REFUSED;
    int latch = p->tap_fx == fx && now - p->tap_t >= 0 && now - p->tap_t <= (long)(SR * PF_LATCH_MS / 1000.0);
    entry_t e = { fx, target, latch, 1 };
    p->st[p->nst++] = e;
    if (latch) p->tap_fx = -1; else { p->tap_fx = fx; p->tap_t = now; }
    publish(p);
    return latch ? PF_P_LATCHED : PF_P_ON;
}
void perf_release(perf_t *p, int fx) {
    if (!p) return;
    for (int i = p->nst - 1; i >= 0; i--)
        if (p->st[i].fx == fx && p->st[i].held) {
            p->st[i].held = 0;
            if (!p->st[i].latched) st_remove(p, i);
            publish(p);
            return;
        }
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
    if (fx >= 0 && fx < PF_NFX && k >= 0 && k < 3 && DISC[fx][k]) {
        int n = DISC[fx][k], i = didx(p, fx, k) + (delta > 0 ? 1 : -1);
        i = i < 0 ? 0 : i > n - 1 ? n - 1 : i;
        perf_pct_set(p, fx, k, (i * 100 + (n - 1) / 2) / (n - 1));
        return;
    }
    perf_pct_set(p, fx, k, perf_pct(p, fx, k) + delta * 2);
}

const char *perf_name(int fx) { return fx >= 0 && fx < PF_NFX ? NAME[fx] : "---"; }
const char *perf_short(int fx) { return fx >= 0 && fx < PF_NFX ? SHORT[fx] : "---"; }
int perf_nparams(int fx) { return fx >= 0 && fx < PF_NFX ? NP[fx] : 0; }
const char *perf_plabel(int fx, int k) { return fx >= 0 && fx < PF_NFX && k >= 0 && k < 3 ? PL[fx][k] : ""; }
int perf_family(int fx) {
    switch (fx) {
    case PF_BEATREP: return 0;
    case PF_TSTOP: case PF_TSTART: case PF_REVERSE: case PF_VINYL: case PF_HALF: return 1;
    case PF_PITCH: case PF_CRUSH: case PF_LP: case PF_HP: return 2;
    case PF_STUTTER: case PF_GATE: case PF_RETRIG: return 3;
    case PF_DELAY: case PF_FREEZE: return 4;
    default: return 5;
    }
}

/* Decoded value of knob k of effect fx:
 *  BEATREP 0 rate index 0..5, 1 gain kept per repeat 1..0.35, 2 pitch drop st/repeat 0..12
 *  STUTTER 0 beats 1..4          PITCH 0 semitones -12..12   CRUSH 0 bits 16..2, 1 hold 1..32
 *  TSTOP/TSTART 0 beats .25..2   VINYL 0 floor speed .92..30, 1 crackle 0..1
 *  LP/HP 0 cutoff Hz 30..18k (exp), 1 reso 0..1
 *  GATE 0 rate index 0..5, 1 depth 0..1, 2 swing 0..1
 *  DELAY 0 time index 0..4, 1 feedback 0..0.88   FREEZE 0 size index 0..4
 *  RETRIG 0 index 0..2   HALF 0 index 0..1   KILL 0 index 0..2 (ALL, LOW, HIGH) */
float perf_pval(const perf_t *p, int fx, int k) {
    float v = (float)perf_pct(p, fx, k) / 100.0f;
    switch (fx) {
    case PF_BEATREP: return k == 0 ? (float)(int)(v * 5.999f) : k == 1 ? 1.0f - v * 0.65f : v * 12.0f;
    case PF_STUTTER: return 1.0f + v * 3.0f;
    case PF_PITCH:   return (float)(int)lroundf(-12.0f + v * 24.0f);
    case PF_CRUSH:   return k == 0 ? (float)(16 - (int)lroundf(v * 14.0f)) : 1.0f + (float)(int)lroundf(v * 31.0f);
    case PF_TSTOP: case PF_TSTART: return 0.25f + v * 1.75f;
    case PF_VINYL:   return k == 0 ? 0.92f - v * 0.62f : v;
    case PF_LP: case PF_HP: return k == 0 ? 30.0f * powf(600.0f, v) : v;
    case PF_GATE:    return k == 0 ? (float)didx(p, fx, 0) : v;
    case PF_DELAY:   return k == 0 ? (float)didx(p, fx, 0) : v * 0.88f;
    case PF_FREEZE: case PF_RETRIG: case PF_HALF: case PF_KILL: return (float)didx(p, fx, 0);
    default: return 0.0f;
    }
}

void perf_ptext(const perf_t *p, int fx, int k, char *out, int n) {
    float v = perf_pval(p, fx, k);
    out[0] = 0;
    switch (fx) {
    case PF_BEATREP:
        if (k == 0) snprintf(out, (size_t)n, "%s", RATE_TXT[(int)v]);
        else if (k == 1) snprintf(out, (size_t)n, "%d%%", (int)lroundf((1.0f - v) * 100.0f / 0.65f));
        else snprintf(out, (size_t)n, "-%dST", (int)lroundf(v));
        break;
    case PF_STUTTER: snprintf(out, (size_t)n, "%.1fB", (double)v); break;
    case PF_PITCH:   snprintf(out, (size_t)n, "%+dST", (int)v); break;
    case PF_CRUSH:   if (k == 0) snprintf(out, (size_t)n, "%dB", (int)v); else snprintf(out, (size_t)n, "/%d", (int)v); break;
    case PF_TSTOP: case PF_TSTART: snprintf(out, (size_t)n, "%.2gB", (double)v); break;
    case PF_VINYL:   snprintf(out, (size_t)n, "%d%%", (int)lroundf(v * 100.0f)); break;
    case PF_LP: case PF_HP:
        if (k == 0) { if (v >= 1000.0f) snprintf(out, (size_t)n, "%.1fK", (double)(v / 1000.0f)); else snprintf(out, (size_t)n, "%dHZ", (int)lroundf(v)); }
        else snprintf(out, (size_t)n, "%d%%", (int)lroundf(v * 100.0f));
        break;
    case PF_GATE:
        if (k == 0) snprintf(out, (size_t)n, "%s", RATE_TXT[(int)v]); else snprintf(out, (size_t)n, "%d%%", (int)lroundf(v * 100.0f));
        break;
    case PF_DELAY:
        if (k == 0) snprintf(out, (size_t)n, "%s", DLY_TXT[(int)v]); else snprintf(out, (size_t)n, "%d%%", (int)lroundf(v / 0.88f * 100.0f));
        break;
    case PF_FREEZE:  snprintf(out, (size_t)n, "%s", FRZ_TXT[(int)v]); break;
    case PF_RETRIG:  snprintf(out, (size_t)n, "%s", RTG_TXT[(int)v]); break;
    case PF_HALF:    snprintf(out, (size_t)n, "%s", HLF_TXT[(int)v]); break;
    case PF_KILL:    snprintf(out, (size_t)n, "%s", KIL_TXT[(int)v]); break;
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
static void svf_set(slot_t *s, float fc, float k) {
    float g = tanf((float)M_PI * fc / (float)MARK_SR);
    s->kk = k; s->a1 = 1.0f / (1.0f + g * (g + k)); s->a2 = g * s->a1; s->a3 = g * s->a2;
}
static inline void svf_run(slot_t *s, int ch, int st, float x, float *lp, float *hp) {
    float v3 = x - s->ic2[ch][st];
    float v1 = s->a1 * s->ic1[ch][st] + s->a2 * v3;
    float v2 = s->ic2[ch][st] + s->a2 * s->ic1[ch][st] + s->a3 * v3;
    s->ic1[ch][st] = 2.0f * v1 - s->ic1[ch][st];
    s->ic2[ch][st] = 2.0f * v2 - s->ic2[ch][st];
    *lp = v2; *hp = x - s->kk * v1 - v2;
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
        s->L = Ln;
        copy_slice(L, L->ring2, L->sb[SB_FRZ], Ln, Ln);                 /* what reached the end of the chain */
    } else if (fx == PF_DELAY) {
        if (L->dly_dirty) { memset(L->sb[SB_DLY], 0, (size_t)SL_N * 2 * sizeof(float)); L->dly_dirty = 0; }
        s->dly = -1.0;
    } else if (fx == PF_LP || fx == PF_HP) {
        s->fcs = perf_pval(p, fx, 0); s->fn = 0;
    }
}

void perf_block_begin(perf_t *p, double grid_pos, double beat_frames) {
    if (!p) return;
    for (int l = 0; l < PF_NLANE; l++) {
        lane_t *L = &p->ln[l];
        L->grid = grid_pos; L->bn = 0;
        if (beat_frames > 1000.0) L->beat = beat_frames;
        uint32_t w = atomic_load(&L->want);
        for (int fx = 0; fx < PF_NFX; fx++) {
            slot_t *s = &L->s[fx];
            if ((w >> fx) & 1u) { if (!s->held || !s->act) engage(p, L, fx); }
            else if (s->held) s->held = 0;
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
static void jump_read(const lane_t *L, slot_t *s, const float *ring, double d, float *wl, float *wr) {
    int64_t di = (int64_t)(d + 0.5);
    if (di > RING_N - 1024) di = RING_N - 1024;
    if (di < 0) di = 0;
    if (s->dprev < 0.0) s->dprev = (double)di;
    if ((double)di != s->dprev && s->xf <= 0) s->xf = XFADE;
    uint32_t a = (uint32_t)((int64_t)L->wc - di) & RING_MASK;
    float l = ring[a * 2], r = ring[a * 2 + 1];
    if (s->xf > 0) {
        uint32_t b = (uint32_t)((int64_t)L->wc - (int64_t)s->dprev) & RING_MASK;
        float m = (float)s->xf / (float)XFADE;
        l += (ring[b * 2] - l) * m; r += (ring[b * 2 + 1] - r) * m;
        if (--s->xf <= 0) s->dprev = (double)di;
    } else s->dprev = (double)di;
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
            s->rep_len = (int)((double)s->L * pow(0.125, u));
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
        jump_read(L, s, L->ring, R * jj, wl, wr);
        s->rep = (int)jj; s->phase = (float)((tau - jj * R) / R);
        break;
    }
    case PF_PITCH: {
        double ratio = pow(2.0, perf_pval(p, PF_PITCH, 0) / 12.0);
        s->speed = (float)ratio;
        gran(L, s, L->ring1, ratio, wl, wr);
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
        *wl = a; *wr = b;
        s->speed = (float)r; s->phase = (float)(tau / C);
        break;
    }
    case PF_CRUSH: {
        float bits = perf_pval(p, PF_CRUSH, 0), ds = perf_pval(p, PF_CRUSH, 1);
        float q = exp2f(16.0f - bits);
        if (s->hold <= 0.0) {
            s->hold += (double)ds;
            s->hl = floorf(il / q + 0.5f) * q; s->hr = floorf(ir / q + 0.5f) * q;
        }
        s->hold -= 1.0;
        *wl = s->hl; *wr = s->hr;
        break;
    }
    case PF_TSTOP: {
        double T = (double)perf_pval(p, PF_TSTOP, 0) * L->beat;
        double sp = 1.0 - k / T; if (sp < 0.0) sp = 0.0;
        s->d += 1.0 - sp;
        if (s->d > RING_N / 2) s->d = RING_N / 2;
        s->speed = (float)sp;
        if (sp <= 0.0) { *wl = *wr = 0.0f; break; }
        float a, b;
        ring_at(L->ring1, (double)L->wc - s->d, &a, &b);
        float gs = sqrtf((float)sp);
        *wl = a * gs; *wr = b * gs;
        break;
    }
    case PF_TSTART: {
        double T = (double)perf_pval(p, PF_TSTART, 0) * L->beat;
        double u = k / T; if (u > 1.0) u = 1.0;
        double ratio = u < 0.03 ? 0.03 : u;
        s->speed = (float)u;
        float a, b;
        gran(L, s, L->ring1, ratio, &a, &b);
        float gs = (float)u;                       /* amplitude ramps 0..1 with the speed */
        *wl = a * gs; *wr = b * gs;
        break;
    }
    case PF_REVERSE: {
        const int64_t M = RING_N / 2 - 256;
        int64_t j = (int64_t)k % M;
        uint32_t a = (uint32_t)((int64_t)s->rv0 - j) & RING_MASK;
        float e1 = (float)j / EDGE, e2 = (float)(M - 1 - j) / EDGE;
        float e = e1 < e2 ? e1 : e2; e = e > 1.0f ? 1.0f : e;
        *wl = L->ring[a * 2] * e; *wr = L->ring[a * 2 + 1] * e;
        s->speed = -1.0f;
        break;
    }
    case PF_VINYL: {
        float floor_s = perf_pval(p, PF_VINYL, 0), cr = perf_pval(p, PF_VINYL, 1);
        double ratio = 1.0 - (1.0 - (double)floor_s) * (1.0 - exp(-k / (1.2 * SR)));
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
            svf_set(s, fc, 1.0f / q);
            s->lpl = 1.0f / (1.0f + 0.06f * (q - 0.7f));          /* level compensation */
            s->aux = logf(fc / 30.0f) / logf(600.0f);
        }
        float lp, hp, lq, hq;
        svf_run(s, 0, 0, il, &lp, &hp); svf_run(s, 1, 0, ir, &lq, &hq);
        *wl = (fx == PF_LP ? lp : hp) * s->lpl; *wr = (fx == PF_LP ? lq : hq) * s->lpl;
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
    case PF_KILL: {
        int m = (int)perf_pval(p, fx, 0);
        if (m != s->kmode) {
            s->kmode = m;
            if (m == 1) svf_set(s, 400.0f, 1.4142f); else if (m == 2) svf_set(s, 2000.0f, 1.4142f);
        }
        if (m == 0) { *wl = *wr = 0.0f; s->speed = 0.0f; break; }
        float lp, hp, lq, hq, l2, h2;
        svf_run(s, 0, 0, il, &lp, &hp); svf_run(s, 1, 0, ir, &lq, &hq);            /* two stages: 24 dB/oct */
        if (m == 1) { svf_run(s, 0, 1, hp, &l2, &h2); *wl = h2; svf_run(s, 1, 1, hq, &l2, &h2); *wr = h2; }   /* LOW kill = the lows are gone */
        else { svf_run(s, 0, 1, lp, &l2, &h2); *wl = l2; svf_run(s, 1, 1, lq, &l2, &h2); *wr = l2; }          /* HIGH kill = the highs are gone */
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
        *wl = il + el * 0.85f; *wr = ir + er * 0.85f;
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
        *wl = a; *wr = b;
        s->phase = (float)j / (float)Lb; s->speed = 1.0f;
        break;
    }
    default: *wl = il; *wr = ir; break;
    }
}

static void stage(perf_t *p, lane_t *L, int fx, float *x0, float *x1) {
    slot_t *s = &L->s[fx];
    float tgt = s->held ? 1.0f : 0.0f;
    float fi = fx == PF_KILL ? FADE_KILL : FADE_IN, fo = fx == PF_KILL ? FADE_KILL : FADE_OUT;
    if (s->g < tgt) { s->g += fi; if (s->g > tgt) s->g = tgt; }
    else if (s->g > tgt) { s->g -= fo; if (s->g < tgt) s->g = tgt; }
    float wl, wr;
    render(p, L, s, fx, *x0, *x1, &wl, &wr);
    if (fx == PF_DELAY) { *x0 = wl; *x1 = wr; }               /* the echoes are not gated: only the input is */
    else { *x0 += (wl - *x0) * s->g; *x1 += (wr - *x1) * s->g; }
    s->k += 1.0;
    if (!s->held && s->g <= 0.0f && (fx != PF_DELAY || s->tail <= 0.0)) {
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
