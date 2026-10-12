/* o303 - the Rat Drive pedal, as a mono stage.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * vendor/pedal-fx/rat/rat.c is the Rat Drive audio FX's DSP (schwung-pedal-fx),
 * compiled in UNMODIFIED: the same op-amp model, diode/Filter node solve, 4x
 * oversampling and RAT (LM308) / RAT2 (OP07DP) switch.
 */
#ifndef RAT_STAGE_H
#define RAT_STAGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rat_stage rat_stage;

/* The pedal's knobs, each 0..1 (MODEL: 0 = RAT, 1 = RAT2). */
enum { RAT_DISTORTION, RAT_FILTER, RAT_LEVEL, RAT_LOW_CUT, RAT_HI_CUT, RAT_MODEL, RAT_KNOBS };

rat_stage *rat_stage_create(void);
void rat_stage_destroy(rat_stage *r);
void rat_stage_set(rat_stage *r, int knob, float value);
/* Jump every smoother to its target (after a state restore). */
void rat_stage_snap(rat_stage *r);
/* on = 0 bypasses (crossfaded, and the pedal costs nothing once it is out).
 * In place; the result is finite and within +-4. */
void rat_stage_process(rat_stage *r, float *mono, int frames, int on);

#ifdef __cplusplus
}
#endif
#endif
