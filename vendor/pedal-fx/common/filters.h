/* Linear filters for schwung-pedal-fx.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Hiro Ikezawa. Original work.
 *
 * Coefficients (…_c) are shared by both channels; states (…_s) are per
 * channel. Designers run at the control rate in double precision; the
 * per-sample kernels are float. Analog prototypes are turned into digital
 * filters with the bilinear transform, optionally pre-warped at one
 * frequency, which is how every circuit-derived stage in this project is
 * built: write H(s) from the schematic's component values, then pfx_blt*().
 */
#ifndef PFX_FILTERS_H
#define PFX_FILTERS_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- first order --------------------------------------------------------- */
typedef struct { float b0, b1, a1; } pfx_fo_c;
typedef struct { float z; } pfx_fo_s;

static inline float pfx_fo(const pfx_fo_c *c, pfx_fo_s *s, float x) {
    const float y = c->b0 * x + s->z;
    s->z = c->b1 * x - c->a1 * y;
    return y;
}

/* K for the bilinear transform: 2*fs, or pre-warped so that the analog and
 * digital responses agree exactly at f_warp (0 = no pre-warp). */
static inline double pfx_blt_k(double fs, double f_warp) {
    if (f_warp <= 0.0) return 2.0 * fs;
    const double w = 2.0 * M_PI * f_warp;
    return w / tan(w / (2.0 * fs));
}

/* H(s) = (B1 s + B0) / (A1 s + A0) */
static inline void pfx_blt1(pfx_fo_c *c, double B1, double B0, double A1, double A0, double K) {
    const double a0 = A1 * K + A0;
    c->b0 = (float)((B1 * K + B0) / a0);
    c->b1 = (float)((B0 - B1 * K) / a0);
    c->a1 = (float)((A0 - A1 * K) / a0);
}

static inline void pfx_fo_lp(pfx_fo_c *c, double fc, double fs) {
    const double w = 2.0 * M_PI * fc;
    pfx_blt1(c, 0.0, w, 1.0, w, pfx_blt_k(fs, fc));
}
static inline void pfx_fo_hp(pfx_fo_c *c, double fc, double fs) {
    const double w = 2.0 * M_PI * fc;
    pfx_blt1(c, 1.0, 0.0, 1.0, w, pfx_blt_k(fs, fc));
}

/* ---- biquad (transposed direct form II) ---------------------------------- */
typedef struct { float b0, b1, b2, a1, a2; } pfx_bq_c;
typedef struct { float z1, z2; } pfx_bq_s;

static inline float pfx_bq(const pfx_bq_c *c, pfx_bq_s *s, float x) {
    const float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}

static inline void pfx_bq_identity(pfx_bq_c *c) {
    c->b0 = 1.0f; c->b1 = c->b2 = c->a1 = c->a2 = 0.0f;
}

/* H(s) = (B2 s^2 + B1 s + B0) / (A2 s^2 + A1 s + A0) */
static inline void pfx_blt2(pfx_bq_c *c, double B2, double B1, double B0,
                            double A2, double A1, double A0, double K) {
    const double K2 = K * K;
    const double a0 = A2 * K2 + A1 * K + A0;
    c->b0 = (float)((B2 * K2 + B1 * K + B0) / a0);
    c->b1 = (float)(2.0 * (B0 - B2 * K2) / a0);
    c->b2 = (float)((B2 * K2 - B1 * K + B0) / a0);
    c->a1 = (float)(2.0 * (A0 - A2 * K2) / a0);
    c->a2 = (float)((A2 * K2 - A1 * K + A0) / a0);
}

/* Analog prototypes, w0 = 2 pi f0, pre-warped at f0. */
static inline void pfx_bq_lp(pfx_bq_c *c, double f0, double q, double fs) {
    const double w = 2.0 * M_PI * f0;
    pfx_blt2(c, 0, 0, w * w, 1, w / q, w * w, pfx_blt_k(fs, f0));
}
static inline void pfx_bq_hp(pfx_bq_c *c, double f0, double q, double fs) {
    const double w = 2.0 * M_PI * f0;
    pfx_blt2(c, 1, 0, 0, 1, w / q, w * w, pfx_blt_k(fs, f0));
}
static inline void pfx_bq_bp(pfx_bq_c *c, double f0, double q, double fs) { /* unity peak */
    const double w = 2.0 * M_PI * f0;
    pfx_blt2(c, 0, w / q, 0, 1, w / q, w * w, pfx_blt_k(fs, f0));
}
/* Peaking bell, gain in dB. Constant-Q form: the boost and the cut of the
 * same magnitude are exact inverses. */
static inline void pfx_bq_peak(pfx_bq_c *c, double f0, double q, double db, double fs) {
    const double w = 2.0 * M_PI * f0, A = pow(10.0, db / 40.0);
    pfx_blt2(c, 1, w * A / q, w * w, 1, w / (A * q), w * w, pfx_blt_k(fs, f0));
}
/* Shelves: RBJ analog prototypes with slope parameter via q (0.707 = the
 * steepest monotonic shelf, lower = gentler). */
static inline void pfx_bq_lowshelf(pfx_bq_c *c, double f0, double q, double db, double fs) {
    const double w = 2.0 * M_PI * f0, A = pow(10.0, db / 40.0), sA = sqrt(A);
    pfx_blt2(c, A, A * sA * w / q, A * A * w * w, A, sA * w / q, w * w, pfx_blt_k(fs, f0));
    /* H(s) = A (s^2 + sqrt(A) w/q s + A w^2) / (A s^2 + sqrt(A) w/q s + w^2) */
}
static inline void pfx_bq_highshelf(pfx_bq_c *c, double f0, double q, double db, double fs) {
    const double w = 2.0 * M_PI * f0, A = pow(10.0, db / 40.0), sA = sqrt(A);
    pfx_blt2(c, A * A, A * sA * w / q, A * w * w, 1, sA * w / q, A * w * w, pfx_blt_k(fs, f0));
    /* H(s) = A (A s^2 + sqrt(A) w/q s + w^2) / (s^2 + sqrt(A) w/q s + A w^2) */
}

/* ---- magnitude helpers for tests / calibration ---------------------------- */
static inline double pfx_bq_mag(const pfx_bq_c *c, double f, double fs) {
    const double w = 2.0 * M_PI * f / fs, cw = cos(w), sw = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
    const double nr = c->b0 + c->b1 * cw + c->b2 * c2, ni = -(c->b1 * sw + c->b2 * s2);
    const double dr = 1 + c->a1 * cw + c->a2 * c2, di = -(c->a1 * sw + c->a2 * s2);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}
static inline double pfx_fo_mag(const pfx_fo_c *c, double f, double fs) {
    const double w = 2.0 * M_PI * f / fs, cw = cos(w), sw = sin(w);
    const double nr = c->b0 + c->b1 * cw, ni = -c->b1 * sw, dr = 1 + c->a1 * cw, di = -c->a1 * sw;
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* ---- DC blocker: first-order high-pass, fixed ------------------------------ */
typedef struct { float x1, y1; } pfx_dc_s;
static inline float pfx_dc(pfx_dc_s *s, float x, float r) {
    const float y = x - s->x1 + r * s->y1;
    s->x1 = x;
    s->y1 = y;
    return y;
}

/* True when both channels carry the same samples, so a pedal whose two
 * channel states are also identical may run one channel and copy it. */
static inline int pfx_is_mono(const float *l, const float *r, int n) {
    for (int i = 0; i < n; ++i)
        if (l[i] != r[i]) return 0;
    return 1;
}

/* Flush a filter state value that has decayed into the denormal range. */
static inline float pfx_flush(float v) { return fabsf(v) < 1e-20f ? 0.0f : v; }

#endif
