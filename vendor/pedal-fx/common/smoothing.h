/* Parameter smoothing for schwung-pedal-fx.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Hiro Ikezawa. Original work.
 *
 * Every knob is smoothed at the CONTROL rate (one step per PFX_CTRL frames).
 * Audio-rate gains are then ramped linearly across each control block, so a
 * level change is click-free while a filter coefficient is recomputed only
 * once per control block, and only while its knob is still moving.
 */
#ifndef PFX_SMOOTHING_H
#define PFX_SMOOTHING_H

#include <math.h>

#define PFX_FS 44100.0f
#define PFX_CTRL 16 /* frames per control block: 0.36 ms */

typedef struct {
    float cur, target, coef;
} pfx_smooth;

/* tau_ms is the one-pole time constant; the value is within 1% of a step
 * after 4.6 tau. */
static inline void pfx_sm_init(pfx_smooth *s, float value, float tau_ms) {
    s->cur = s->target = value;
    s->coef = 1.0f - expf(-(float)PFX_CTRL / (tau_ms * 0.001f * PFX_FS));
}

static inline void pfx_sm_set(pfx_smooth *s, float target) { s->target = target; }
static inline void pfx_sm_snap(pfx_smooth *s) { s->cur = s->target; }
static inline int pfx_sm_moving(const pfx_smooth *s) { return s->cur != s->target; }

/* One control-rate step. Snaps once within 1e-5 so "moving" goes false and
 * the dependent coefficient work stops. */
static inline float pfx_sm_step(pfx_smooth *s) {
    const float d = s->target - s->cur;
    if (fabsf(d) < 1e-5f) s->cur = s->target;
    else s->cur += d * s->coef;
    return s->cur;
}

/* Linear audio-rate ramp from the previous control value to the next. */
typedef struct {
    float value, step;
} pfx_ramp;

static inline void pfx_ramp_to(pfx_ramp *r, float next, int frames) {
    r->step = (next - r->value) / (float)frames;
}
static inline float pfx_ramp_tick(pfx_ramp *r) {
    const float v = r->value;
    r->value += r->step;
    return v;
}
static inline void pfx_ramp_end(pfx_ramp *r, float next) { r->value = next; r->step = 0.0f; }

#endif
