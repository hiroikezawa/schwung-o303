/* Knob tapers: every host parameter is 0..1 and is mapped here.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Hiro Ikezawa. Original work.
 */
#ifndef PFX_TAPERS_H
#define PFX_TAPERS_H

#include <math.h>

/* Volts at the pedal input for digital full scale. A Move track peaking at
 * -6 dBFS then arrives as 250 mV, a hot humbucker; the Input knob (+-24 dB)
 * moves from single-coil politeness to synth-line brutality. */
#define PFX_V_IN 0.5f

static inline float pfx_db2lin(float db) { return expf(db * 0.11512925465f); }

/* Audio ("A", logarithmic) pot: fraction of the track resistance at knob
 * position x. k = 4.6 puts noon at ~9%, a typical 10% A-taper. */
static inline float pfx_taper_log(float x, float k) {
    return (expf(k * x) - 1.0f) / (expf(k) - 1.0f);
}
/* Reverse log ("C") pot. */
static inline float pfx_taper_rlog(float x, float k) { return 1.0f - pfx_taper_log(1.0f - x, k); }

/* "G" / S-curve pot (slow at both ends, fast through the middle), p > 1. */
static inline float pfx_taper_s(float x, float p) {
    return x < 0.5f ? 0.5f * powf(2.0f * x, p) : 1.0f - 0.5f * powf(2.0f - 2.0f * x, p);
}

/* Frequency knob: logarithmic sweep f0 .. f1. */
static inline float pfx_freq_log(float x, float f0, float f1) { return f0 * powf(f1 / f0, x); }

/* Bipolar dB knob: 0.5 = 0 dB, 0 / 1 = -range / +range. */
static inline float pfx_db_knob(float x, float range_db) { return (2.0f * x - 1.0f) * range_db; }

/* Cut/boost tone control through a real pot circuit: barely moves around
 * noon and races at the ends. dB = max * tan(pi*bend*d) / tan(pi*bend/2),
 * d = x - 0.5. bend 0.88 matches the MT-2's Low/High law (+-1 dB at 40/60 %,
 * +-7 dB at 10/90 %, full at the stops); 0.5 its Mid; 0 would be linear. */
static inline float pfx_cutboost_db(float x, float max_db, float bend) {
    const float d = x - 0.5f;
    if (bend <= 0.0f) return 2.0f * d * max_db;
    return max_db * tanf(3.14159265f * bend * d) / tanf(1.57079633f * bend);
}

/* Pedal Level / Volume: linear in dB (an idealised audio-taper pot),
 * 0.5 = the pedal's calibrated unity, +-20 dB, and a fade to silence below
 * 5% so fully counter-clockwise is off, as on the pedal. */
static inline float pfx_level_gain(float x) {
    if (x <= 0.0f) return 0.0f;
    const float g = pfx_db2lin((x - 0.5f) * 40.0f);
    return x < 0.05f ? g * (x / 0.05f) : g;
}

#endif
