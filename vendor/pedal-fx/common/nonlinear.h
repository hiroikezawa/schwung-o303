/* Nonlinear building blocks for schwung-pedal-fx.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Hiro Ikezawa. Original work.
 *
 * Diode networks are solved, not table-looked-up. Every clipper in these
 * pedals reduces to one node:
 *
 *        J ──►──┬────────┬─────────┐
 *               │        │         │
 *               G      Id(v)     (C: trapezoidal companion, folded into G, J)
 *               │        │         │
 *              ─┴─      ─┴─       ─┴─
 *
 *        G v + Id(v) = J,   Id(v) = Is+ (e^{v/a+} - 1) - Is- (e^{-v/a-} - 1)
 *
 * with a = N n Vt for N diodes in series. The dominant diode alone has the
 * closed-form solution v = u - a W((Is/(G a)) e^{u/a}), u = (J + Is)/G,
 * evaluated with the Wright omega function (W(e^x) = omega(x)) using the
 * fast approximation of D'Angelo, Gabrielli & Turchet, "Fast approximation of
 * the Lambert W function for virtual analog modelling", DAFx 2019. One Newton
 * step on the FULL two-diode equation then corrects both the approximation
 * and the neglected opposite diode. Cost: ~4 exp/log per sample.
 */
#ifndef PFX_NONLINEAR_H
#define PFX_NONLINEAR_H

#include <math.h>
#include <stdint.h>

#define PFX_VT 0.02585f /* thermal voltage at 300 K */

static inline float pfx_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* ---- Wright omega ---------------------------------------------------------- */
static inline float pfx_omega3(float x) {
    const float x1 = -3.341459552768620f, x2 = 8.0f;
    const float a = -1.314293149877800e-3f, b = 4.775931364975583e-2f;
    const float c = 3.631952663804445e-1f, d = 6.313183464296682e-1f;
    if (x < x1) return 0.0f;
    if (x < x2) return d + x * (c + x * (b + x * a));
    return x - logf(x);
}
static inline float pfx_omega4(float x) {
    if (x >= 8.0f) {
        /* y = x - ln x, so exp(x - y) = x exactly: the refinement needs
         * no exp in the high-current region every hard clipper lives in. */
        const float y = x - logf(x);
        return y - (y - x) / (y + 1.0f);
    }
    const float y = pfx_omega3(x);
    return y - (y - expf(x - y)) / (y + 1.0f);
}

/* ---- diode pair ------------------------------------------------------------ */
typedef struct {
    float is_p, a_p; /* conducts for v > 0 */
    float is_n, a_n; /* conducts for v < 0 */
} pfx_diodes;

/* Silicon small-signal diode (1N4148 / 1N914 / 1SS133 class):
 * Is = 2.52 nA, n = 1.752 (the common SPICE card). `series` diodes stacked. */
static inline pfx_diodes pfx_diodes_si(int series_p, int series_n, float mismatch) {
    pfx_diodes d;
    d.is_p = 2.52e-9f;
    d.is_n = 2.52e-9f * mismatch; /* >1: the negative side conducts earlier */
    d.a_p = (float)series_p * 1.752f * PFX_VT;
    d.a_n = (float)series_n * 1.752f * PFX_VT;
    return d;
}

/* Everything about the node that changes only when G changes. */
typedef struct {
    float G, inv_G, ln_p, ln_n;
} pfx_node;

static inline void pfx_node_set(pfx_node *n, const pfx_diodes *d, float G) {
    n->G = G;
    n->inv_G = 1.0f / G;
    n->ln_p = logf(d->is_p / (G * d->a_p));
    n->ln_n = logf(d->is_n / (G * d->a_n));
}

/* Hot-path solve: the omega4 closed form on the dominant diode only.
 * Max error 2 mV against the exact two-diode solution (~0.3% of the drop,
 * i.e. a 0.3% different diode), smooth and continuous through J = 0 where
 * the neglected diode's conductance is ~1e-4 of any G used here. */
static inline float pfx_diode_fast(const pfx_diodes *d, const pfx_node *n, float J) {
    if (J >= 0.0f) {
        const float u = (J + d->is_p) * n->inv_G;
        return u - d->a_p * pfx_omega4(n->ln_p + u / d->a_p);
    }
    const float u = (-J + d->is_n) * n->inv_G;
    return -(u - d->a_n * pfx_omega4(n->ln_n + u / d->a_n));
}

/* Reference solve: closed form plus one Newton step on the full two-diode
 * equation (max error ~40 uV). Used by the tests to check pfx_diode_fast. */
static inline float pfx_diode_solve(const pfx_diodes *d, const pfx_node *n, float J) {
    float v;
    if (J >= 0.0f) {
        const float u = (J + d->is_p) * n->inv_G;
        v = u - d->a_p * pfx_omega4(n->ln_p + u / d->a_p);
    } else {
        const float u = (-J + d->is_n) * n->inv_G;
        v = -(u - d->a_n * pfx_omega4(n->ln_n + u / d->a_n));
    }
    /* Newton on the full equation; exponents clamped far above any real
     * operating point (e^60 A would be a dead diode anyway). */
    const float ep = expf(pfx_clampf(v / d->a_p, -60.0f, 60.0f));
    const float en = d->a_p == d->a_n ? 1.0f / ep : expf(pfx_clampf(-v / d->a_n, -60.0f, 60.0f));
    const float f = n->G * v + d->is_p * (ep - 1.0f) - d->is_n * (en - 1.0f) - J;
    const float fp = n->G + d->is_p / d->a_p * ep + d->is_n / d->a_n * en;
    return v - f / fp;
}

/* ---- capacitor across the node (trapezoidal companion) -------------------- *
 *   i_C[n] = Gc v[n] - Ih,   Ih = Gc v[n-1] + i_C[n-1],   Gc = 2 C fs
 * so the node sees an extra conductance Gc and an extra source current Ih. */
typedef struct { float ih; } pfx_cap_s;

static inline float pfx_cap_update(pfx_cap_s *s, float Gc, float v) {
    const float ic = Gc * v - s->ih;
    s->ih = Gc * v + ic;
    return ic;
}

/* ---- static saturators ----------------------------------------------------- */
/* x / (1 + x^4)^(1/4): the algebraic clipper of dm-Rat. */
static inline float pfx_alg4(float x) {
    const float x2 = x * x;
    return x / sqrtf(sqrtf(1.0f + x2 * x2));
}
/* Output-swing limiter: exactly linear up to knee*V, then a rational curve
 * tangent to the line (C1) approaching V. Costs nothing below the knee, one
 * divide above it. vp / vn: positive / negative limits (> 0). */
static inline float pfx_rail2k(float x, float vp, float vn, float knee_p, float knee_n) {
    const float v = x >= 0.0f ? vp : vn, ax = fabsf(x), k = (x >= 0.0f ? knee_p : knee_n) * v;
    if (ax <= k) return x;
    const float t = (ax - k) / (v - k);
    return copysignf(k + (v - k) * t / (1.0f + t), x);
}
static inline float pfx_rail(float x, float vp, float vn, float knee) {
    return pfx_rail2k(x, vp, vn, knee, knee);
}

/* Asymmetric rail limiter, unity slope at 0, hard-ish knee (p = 4).
 * vp / vn are the positive / negative swing limits (both > 0). */
static inline float pfx_rail4(float x, float vp, float vn) {
    return (x >= 0.0f ? vp : vn) * pfx_alg4(x / (x >= 0.0f ? vp : vn));
}
/* Same, soft knee (p = 2): a transistor running out of headroom. */
static inline float pfx_rail2(float x, float vp, float vn) {
    const float v = x >= 0.0f ? vp : vn, u = x / v;
    return x / sqrtf(1.0f + u * u);
}

#endif
