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
#define SL_N      (1 << 17)                 /* slice buffer frames (~3 s) */
#define GRAN_W    1764.0                    /* granular window (40 ms) */
#define FADE_IN   (1.0f / (0.004f * (float)MARK_SR))
#define FADE_OUT  (1.0f / (0.012f * (float)MARK_SR))
#define EDGE      32.0f                     /* repeat edge fade, frames */

struct perf {
    float *ring;                            /* 2 * RING_N */
    float *slice;                           /* 2 * SL_N */
    uint64_t wc;                            /* frames written */
    _Atomic int want;                       /* -1 = none, else the held effect */
    int   fx, held;
    float g;
    double k, beat, grid;
    uint8_t pct[PF_NFX][PF_NKNOB];
    /* shared per-effect state */
    int64_t p0, L;                          /* beat repeat: phase and slice length */
    int   rep, last_rep, rep_pos, rep_len;
    float rep_gain, rep_rho;
    double d;                               /* tape stop head delay */
    double ph;                              /* granular phase */
    uint64_t rv0;                           /* reverse: press point */
    double hold;                            /* crush sample&hold countdown */
    float hl, hr, lpl, lpr;
    uint32_t rng;
    float speed, lvl, phase;
};

static const uint8_t DEF[PF_NFX][3] = {
    { 20, 20, 0 }, { 33, 0, 0 }, { 75, 0, 0 }, { 60, 20, 0 },
    { 40, 0, 0 }, { 40, 0, 0 }, { 0, 0, 0 }, { 40, 40, 0 },
};
static const char *const NAME[PF_NFX] = { "BEAT REPEAT", "STUTTER", "PITCH", "BITCRUSH", "TAPE STOP", "TAPE START", "REVERSE", "VINYL" };
static const char *const SHORT[PF_NFX] = { "RPT", "STU", "PCH", "BIT", "STP", "STA", "REV", "VNL" };
static const int NP[PF_NFX] = { 3, 1, 1, 2, 1, 1, 0, 2 };
static const char *const PL[PF_NFX][3] = {
    { "RATE", "DECAY", "DROP" }, { "TIME", "", "" }, { "SEMI", "", "" }, { "BITS", "DOWN", "" },
    { "TIME", "", "" }, { "TIME", "", "" }, { "", "", "" }, { "BRAKE", "CRKL", "" },
};
static const char *const RATE_TXT[6] = { "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T" };
static const double RATE_MUL[6] = { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };

perf_t *perf_create(void) {
    perf_t *p = (perf_t *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->ring = (float *)calloc((size_t)RING_N * 2, sizeof(float));
    p->slice = (float *)calloc((size_t)SL_N * 2, sizeof(float));
    if (!p->ring || !p->slice) { perf_destroy(p); return NULL; }
    atomic_init(&p->want, -1);
    p->fx = -1; p->beat = SR * 0.5; p->rng = 0x9E3779B9u; p->speed = 1.0f;
    for (int f = 0; f < PF_NFX; f++) for (int k = 0; k < 3; k++) p->pct[f][k] = DEF[f][k];
    return p;
}
void perf_destroy(perf_t *p) { if (!p) return; free(p->ring); free(p->slice); free(p); }

void perf_set_want(perf_t *p, int fx) { if (p) atomic_store(&p->want, fx >= 0 && fx < PF_NFX ? fx : -1); }
int  perf_want(const perf_t *p) { return p ? atomic_load((_Atomic int *)&p->want) : -1; }
int  perf_pct(const perf_t *p, int fx, int k) { return fx >= 0 && fx < PF_NFX && k >= 0 && k < PF_NKNOB ? p->pct[fx][k] : 0; }
void perf_pct_set(perf_t *p, int fx, int k, int v) {
    if (fx >= 0 && fx < PF_NFX && k >= 0 && k < PF_NKNOB) p->pct[fx][k] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
}
void perf_knob(perf_t *p, int fx, int k, int delta) { perf_pct_set(p, fx, k, perf_pct(p, fx, k) + delta * 2); }

const char *perf_name(int fx) { return fx >= 0 && fx < PF_NFX ? NAME[fx] : "---"; }
const char *perf_short(int fx) { return fx >= 0 && fx < PF_NFX ? SHORT[fx] : "---"; }
int perf_nparams(int fx) { return fx >= 0 && fx < PF_NFX ? NP[fx] : 0; }
const char *perf_plabel(int fx, int k) { return fx >= 0 && fx < PF_NFX && k >= 0 && k < 3 ? PL[fx][k] : ""; }
int perf_family(int fx) {
    return fx == PF_BEATREP ? 0 : fx == PF_STUTTER ? 3 : fx == PF_PITCH || fx == PF_CRUSH ? 2 : 1;
}

/* Decoded value of knob k of effect fx:
 *  BEATREP 0 rate index 0..5, 1 gain kept per repeat 1..0.35, 2 pitch drop st/repeat 0..12
 *  STUTTER 0 beats 1..4          PITCH 0 semitones -12..12   CRUSH 0 bits 16..2, 1 hold 1..32
 *  TSTOP/TSTART 0 beats .25..2   VINYL 0 floor speed .92..30, 1 crackle 0..1 */
float perf_pval(const perf_t *p, int fx, int k) {
    float v = (float)perf_pct(p, fx, k) / 100.0f;
    switch (fx) {
    case PF_BEATREP: return k == 0 ? (float)(int)(v * 5.999f) : k == 1 ? 1.0f - v * 0.65f : v * 12.0f;
    case PF_STUTTER: return 1.0f + v * 3.0f;
    case PF_PITCH:   return (float)(int)lroundf(-12.0f + v * 24.0f);
    case PF_CRUSH:   return k == 0 ? (float)(16 - (int)lroundf(v * 14.0f)) : 1.0f + (float)(int)lroundf(v * 31.0f);
    case PF_TSTOP: case PF_TSTART: return 0.25f + v * 1.75f;
    case PF_VINYL:   return k == 0 ? 0.92f - v * 0.62f : v;
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
    case PF_VINYL:   if (k == 0) snprintf(out, (size_t)n, "%d%%", (int)lroundf(v * 100.0f)); else snprintf(out, (size_t)n, "%d%%", (int)lroundf(v * 100.0f)); break;
    default: break;
    }
}

int perf_busy(const perf_t *p) { return p && p->fx >= 0; }

void perf_vis(const perf_t *p, perf_vis_t *v) {
    v->fx = p->fx; v->held = p->held; v->gate = p->g; v->k = p->k; v->beat = p->beat;
    v->rep = p->rep; v->speed = p->speed; v->phase = p->phase; v->level = p->lvl;
}

/* ---- reading the ring ---- */
static inline void ring_at(const perf_t *p, double pos, float *l, float *r) {
    int64_t i = (int64_t)floor(pos);
    float fr = (float)(pos - (double)i);
    uint32_t a = (uint32_t)i & RING_MASK, b = (uint32_t)(i + 1) & RING_MASK;
    *l = p->ring[a * 2] + (p->ring[b * 2] - p->ring[a * 2]) * fr;
    *r = p->ring[a * 2 + 1] + (p->ring[b * 2 + 1] - p->ring[a * 2 + 1]) * fr;
}
static inline void slice_at(const perf_t *p, double pos, int64_t len, float *l, float *r) {
    if (pos < 0.0) pos = 0.0;
    if (pos > (double)(len - 1)) pos = (double)(len - 1);
    int64_t i = (int64_t)pos;
    int64_t j = i + 1 < len ? i + 1 : i;
    float fr = (float)(pos - (double)i);
    *l = p->slice[i * 2] + (p->slice[j * 2] - p->slice[i * 2]) * fr;
    *r = p->slice[i * 2 + 1] + (p->slice[j * 2 + 1] - p->slice[i * 2 + 1]) * fr;
}

static void copy_slice(perf_t *p, int64_t from_back, int64_t len) {     /* ring frames [wc-from_back, +len) */
    if (len > SL_N) len = SL_N;
    for (int64_t i = 0; i < len; i++) {
        uint32_t a = (uint32_t)((int64_t)p->wc - from_back + i) & RING_MASK;
        p->slice[i * 2] = p->ring[a * 2]; p->slice[i * 2 + 1] = p->ring[a * 2 + 1];
    }
}

static float noise(perf_t *p) {
    p->rng ^= p->rng << 13; p->rng ^= p->rng >> 17; p->rng ^= p->rng << 5;
    return (float)(int32_t)p->rng / 2147483648.0f;
}

static void engage(perf_t *p, int fx) {
    p->fx = fx; p->held = 1; p->k = 0.0; p->rep = 0; p->last_rep = -1; p->rep_pos = 0; p->rep_len = 0;
    p->rep_gain = 1.0f; p->rep_rho = 1.0f; p->d = 0.0; p->ph = 0.0; p->speed = 1.0f;
    p->hold = 0.0; p->lpl = p->lpr = 0.0f; p->rv0 = p->wc;
    if (fx == PF_BEATREP) {
        int idx = (int)perf_pval(p, fx, 0);
        int64_t L = (int64_t)(p->beat * RATE_MUL[idx] + 0.5);
        if (L < 256) L = 256;
        if (L > SL_N) L = SL_N;
        p->L = L;
        p->p0 = (int64_t)fmod(p->grid < 0.0 ? 0.0 : p->grid, (double)L);
        copy_slice(p, p->p0 + L, L);              /* the slice that ended on the last grid line */
    } else if (fx == PF_STUTTER) {
        int64_t L = (int64_t)p->beat;
        if (L < 256) L = 256;
        if (L > SL_N) L = SL_N;
        p->L = L;
        copy_slice(p, L, L);                       /* the last quarter note */
    }
}

void perf_block_begin(perf_t *p, double grid_pos, double beat_frames) {
    if (!p) return;
    int w = atomic_load(&p->want);
    p->grid = grid_pos;
    if (beat_frames > 1000.0) p->beat = beat_frames;
    if (w >= 0) { if (!p->held || p->fx != w) engage(p, w); }
    else p->held = 0;
}

/* granular pitch shifter: two taps on the ring, delay swept by (1 - ratio) per frame */
static void gran(perf_t *p, double ratio, float *wl, float *wr) {
    p->ph += (1.0 - ratio) / GRAN_W;
    p->ph -= floor(p->ph);
    float sl = 0.0f, sr = 0.0f;
    for (int t = 0; t < 2; t++) {
        double ph = p->ph + 0.5 * t;
        if (ph >= 1.0) ph -= 1.0;
        float w = (float)sin(M_PI * ph); w *= w;
        float a, b;
        ring_at(p, (double)p->wc - (ph * GRAN_W + 2.0), &a, &b);
        sl += a * w; sr += b * w;
    }
    *wl = sl; *wr = sr;
}

static void render(perf_t *p, float il, float ir, float *wl, float *wr) {
    double k = p->k;
    switch (p->fx) {
    case PF_BEATREP: {
        int64_t t = p->p0 + (int64_t)k, L = p->L;
        int rep = (int)(t / L);
        double pos = (double)(t % L);
        if (rep != p->last_rep) {
            p->last_rep = rep; p->rep = rep;
            p->rep_gain = powf(perf_pval(p, PF_BEATREP, 1), (float)(rep > 40 ? 40 : rep));
            p->rep_rho = exp2f(-perf_pval(p, PF_BEATREP, 2) * (float)(rep > 40 ? 40 : rep) / 12.0f);
            if (p->rep_rho < 0.05f) p->rep_rho = 0.05f;
        }
        float a, b;
        slice_at(p, pos * p->rep_rho, L, &a, &b);
        float e = (float)(pos < (double)(L - 1) - pos ? pos : (double)(L - 1) - pos) / EDGE;
        e = e > 1.0f ? 1.0f : e;
        float g = p->rep_gain * e;
        *wl = a * g; *wr = b * g;
        p->speed = p->rep_rho;
        p->phase = (float)(pos / (double)L);
        break;
    }
    case PF_STUTTER: {
        double total = perf_pval(p, PF_STUTTER, 0) * p->beat;
        if (p->rep_pos >= p->rep_len) {
            double u = k / total; if (u > 1.0) u = 1.0;
            p->rep_len = (int)((double)p->L * pow(0.125, u));
            if (p->rep_len < 64) p->rep_len = 64;
            p->rep_pos = 0; p->rep++;
        }
        float a, b;
        slice_at(p, (double)(p->L - p->rep_len + p->rep_pos), p->L, &a, &b);
        float e1 = (float)p->rep_pos / EDGE, e2 = (float)(p->rep_len - 1 - p->rep_pos) / EDGE;
        float e = e1 < e2 ? e1 : e2; e = e > 1.0f ? 1.0f : e;
        *wl = a * e; *wr = b * e;
        p->rep_pos++;
        p->speed = (float)p->L / (float)p->rep_len;
        p->phase = (float)p->rep_pos / (float)p->rep_len;
        break;
    }
    case PF_PITCH: {
        double ratio = pow(2.0, perf_pval(p, PF_PITCH, 0) / 12.0);
        p->speed = (float)ratio;
        gran(p, ratio, wl, wr);
        break;
    }
    case PF_CRUSH: {
        float bits = perf_pval(p, PF_CRUSH, 0), ds = perf_pval(p, PF_CRUSH, 1);
        float q = exp2f(16.0f - bits);
        if (p->hold <= 0.0) {
            p->hold += (double)ds;
            p->hl = floorf(il / q + 0.5f) * q; p->hr = floorf(ir / q + 0.5f) * q;
        }
        p->hold -= 1.0;
        *wl = p->hl; *wr = p->hr;
        break;
    }
    case PF_TSTOP: {
        double T = (double)perf_pval(p, PF_TSTOP, 0) * p->beat;
        double s = 1.0 - k / T; if (s < 0.0) s = 0.0;
        p->d += 1.0 - s;
        if (p->d > RING_N / 2) p->d = RING_N / 2;
        p->speed = (float)s;
        if (s <= 0.0) { *wl = *wr = 0.0f; break; }
        float a, b;
        ring_at(p, (double)p->wc - p->d, &a, &b);
        float gs = sqrtf((float)s);
        *wl = a * gs; *wr = b * gs;
        break;
    }
    case PF_TSTART: {
        double T = (double)perf_pval(p, PF_TSTART, 0) * p->beat;
        double u = k / T; if (u > 1.0) u = 1.0;
        double ratio = u < 0.03 ? 0.03 : u;
        p->speed = (float)u;
        float a, b;
        gran(p, ratio, &a, &b);
        float gs = (float)u;                       /* amplitude ramps 0..1 with the speed */
        *wl = a * gs; *wr = b * gs;
        break;
    }
    case PF_REVERSE: {
        const int64_t M = RING_N / 2 - 256;
        int64_t j = (int64_t)k % M;
        uint32_t a = (uint32_t)((int64_t)p->rv0 - j) & RING_MASK;
        float e1 = (float)j / EDGE, e2 = (float)(M - 1 - j) / EDGE;
        float e = e1 < e2 ? e1 : e2; e = e > 1.0f ? 1.0f : e;
        *wl = p->ring[a * 2] * e; *wr = p->ring[a * 2 + 1] * e;
        p->speed = -1.0f;
        break;
    }
    case PF_VINYL: {
        float floor_s = perf_pval(p, PF_VINYL, 0), cr = perf_pval(p, PF_VINYL, 1);
        double ratio = 1.0 - (1.0 - (double)floor_s) * (1.0 - exp(-k / (1.2 * SR)));
        p->speed = (float)ratio;
        float a, b;
        gran(p, ratio, &a, &b);
        p->lpl += (a - p->lpl) * 0.45f; p->lpr += (b - p->lpr) * 0.45f;     /* dull the top, like worn vinyl */
        float hiss = noise(p) * 90.0f * cr, pop = 0.0f;
        if (cr > 0.0f && (noise(p) * 0.5f + 0.5f) < cr * 0.0006f) pop = noise(p) * 6000.0f * cr;
        *wl = p->lpl + hiss + pop; *wr = p->lpr + hiss + pop * 0.6f;
        break;
    }
    default: *wl = il; *wr = ir; break;
    }
}

void perf_process(perf_t *p, float *l, float *r) {
    uint32_t w = (uint32_t)p->wc & RING_MASK;
    p->ring[w * 2] = *l; p->ring[w * 2 + 1] = *r;
    float m = fabsf(*l) > fabsf(*r) ? fabsf(*l) : fabsf(*r);
    p->lvl = m / 32768.0f > p->lvl ? m / 32768.0f : p->lvl * 0.9997f;
    if (p->fx >= 0) {
        float tgt = p->held ? 1.0f : 0.0f;
        if (p->g < tgt) { p->g += FADE_IN; if (p->g > tgt) p->g = tgt; }
        else if (p->g > tgt) { p->g -= FADE_OUT; if (p->g < tgt) p->g = tgt; }
        float wl, wr;
        render(p, *l, *r, &wl, &wr);
        *l += (wl - *l) * p->g; *r += (wr - *r) * p->g;
        p->k += 1.0;
        if (!p->held && p->g <= 0.0f) { p->fx = -1; p->g = 0.0f; p->speed = 1.0f; }
    }
    p->wc++;
}
