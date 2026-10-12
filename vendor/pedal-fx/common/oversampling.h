/* 2x / 4x oversampling with polyphase IIR half-band filters.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Hiro Ikezawa. Original work; coefficients from
 * tools/design_halfband.py (elliptic polyphase half-band design after
 * Valenzuela & Constantinides, 1983).
 *
 * Each half-band is two chains of first-order allpass sections running at
 * the LOW rate, so a 2x up + down pair costs 2 x 8 allpass sections per base
 * sample: about 32 multiplies. Phase is not linear, which does not matter
 * inside a distortion pedal (the analog circuit is not linear-phase either).
 *
 *   stage 1 (base <-> 2x):  8 coefs, transition 0.04 -> flat to 20.3 kHz,
 *                           >= 99 dB image / alias rejection
 *   stage 2 (2x  <-> 4x):   4 coefs, transition 0.20 -> flat to 26 kHz,
 *                           >= 100 dB rejection
 *
 * PFX_OS selects 1, 2 or 4 at compile time, per pedal.
 */
#ifndef PFX_OVERSAMPLING_H
#define PFX_OVERSAMPLING_H

#include <math.h>

#ifndef PFX_OS
#define PFX_OS 2
#endif
#if PFX_OS != 1 && PFX_OS != 2 && PFX_OS != 4
#error "PFX_OS must be 1, 2 or 4"
#endif

static const float pfx_hb1[8] = {
    0.040633460924f, 0.150505129023f, 0.300757055992f, 0.460774504961f,
    0.609524314896f, 0.738503841119f, 0.849223810392f, 0.949742783705f};
static const float pfx_hb2[4] = {
    0.049551035313f, 0.193570326347f, 0.426736688756f, 0.767070072813f};

typedef struct { float x[8], y[8]; } pfx_hb_s;

/* One input sample -> two output samples. */
static inline void pfx_hb_up(const float *c, int n, pfx_hb_s *s, float in, float *o0, float *o1) {
    float even = in, odd = in;
    for (int i = 0; i < n; i += 2) {
        const float t0 = (even - s->y[i]) * c[i] + s->x[i];
        const float t1 = (odd - s->y[i + 1]) * c[i + 1] + s->x[i + 1];
        s->x[i] = even; s->x[i + 1] = odd;
        s->y[i] = t0; s->y[i + 1] = t1;
        even = t0; odd = t1;
    }
    *o0 = even;
    *o1 = odd;
}

/* Two input samples -> one output sample. */
static inline float pfx_hb_down(const float *c, int n, pfx_hb_s *s, float in0, float in1) {
    float even = in1, odd = in0;
    for (int i = 0; i < n; i += 2) {
        const float t0 = (even - s->y[i]) * c[i] + s->x[i];
        const float t1 = (odd - s->y[i + 1]) * c[i + 1] + s->x[i + 1];
        s->x[i] = even; s->x[i + 1] = odd;
        s->y[i] = t0; s->y[i + 1] = t1;
        even = t0; odd = t1;
    }
    return 0.5f * (even + odd);
}

typedef struct {
    pfx_hb_s up1, dn1;
#if PFX_OS == 4
    pfx_hb_s up2, dn2; /* one state each: stage 2 sees one 2x stream */
#endif
} pfx_os_s;

static inline void pfx_os_up(pfx_os_s *s, float in, float *out /* [PFX_OS] */) {
#if PFX_OS == 1
    (void)s;
    out[0] = in;
#elif PFX_OS == 2
    pfx_hb_up(pfx_hb1, 8, &s->up1, in, &out[0], &out[1]);
#else
    float a, b;
    pfx_hb_up(pfx_hb1, 8, &s->up1, in, &a, &b);
    pfx_hb_up(pfx_hb2, 4, &s->up2, a, &out[0], &out[1]);
    pfx_hb_up(pfx_hb2, 4, &s->up2, b, &out[2], &out[3]);
#endif
}

static inline float pfx_os_down(pfx_os_s *s, const float *in /* [PFX_OS] */) {
#if PFX_OS == 1
    (void)s;
    return in[0];
#elif PFX_OS == 2
    return pfx_hb_down(pfx_hb1, 8, &s->dn1, in[0], in[1]);
#else
    const float a = pfx_hb_down(pfx_hb2, 4, &s->dn2, in[0], in[1]);
    const float b = pfx_hb_down(pfx_hb2, 4, &s->dn2, in[2], in[3]);
    return pfx_hb_down(pfx_hb1, 8, &s->dn1, a, b);
#endif
}

static inline void pfx_hb_flush(pfx_hb_s *s) {
    for (int i = 0; i < 8; ++i) {
        if (fabsf(s->x[i]) < 1e-20f) s->x[i] = 0.0f;
        if (fabsf(s->y[i]) < 1e-20f) s->y[i] = 0.0f;
    }
}
static inline void pfx_os_flush(pfx_os_s *s) {
    pfx_hb_flush(&s->up1); pfx_hb_flush(&s->dn1);
#if PFX_OS == 4
    pfx_hb_flush(&s->up2); pfx_hb_flush(&s->dn2);
#endif
}

#endif
