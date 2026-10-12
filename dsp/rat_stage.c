/* o303 - the Rat Drive pedal, embedded. See rat_stage.h.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * rat.c is included, not copied. Its last line pulls in the pedal project's
 * audio-FX adapter (common/plugin_v2.h: the stereo int16 process_block, the
 * Input/Mix/Output knobs, presets). That adapter is the part an FX slot needs
 * and a synth voice does not, so its include guard is claimed here and what
 * is left is the pedal: pedal_init / pedal_set / pedal_control / pedal_run.
 *
 * This file is the adapter's job done for one mono voice: the control-rate
 * step every PFX_CTRL frames, the +-4 / NaN guard, the denormal flush, and
 * the cold-state reset after a bypass.
 */
#include <stdatomic.h>
#include <string.h>

#define PFX_PLUGIN_V2_H               /* see above: found, and skipped whole */
#include "pedal-fx/rat/rat.c"

#include "rat_stage.h"

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "instance pool needs lock-free atomics");

#define RAT_INSTANCES 6
#define RAT_LIMIT 4.0f                /* +12 dBFS: only a broken state gets here */
#define RAT_MAX_BLOCK 128

struct rat_stage {
    atomic_int used;
    struct pedal eng;
    float mix;                        /* 0 dry .. 1 pedal, ramped */
    int cold;                         /* bypassed: the pedal's state is stale */
};

static struct rat_stage rat_pool[RAT_INSTANCES];

static const int rat_index[RAT_KNOBS] = {
    [RAT_DISTORTION] = PK_DISTORTION, [RAT_FILTER] = PK_FILTER, [RAT_LEVEL] = PK_VOLUME,
    [RAT_LOW_CUT] = PK_LOW_CUT, [RAT_HI_CUT] = PK_HI_CUT, [RAT_MODEL] = PK_MODEL,
};

rat_stage *rat_stage_create(void) {
    for (int i = 0; i < RAT_INSTANCES; ++i) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&rat_pool[i].used, &expected, 1)) continue;
        rat_stage *r = &rat_pool[i];
        pedal_init(&r->eng);
        /* The pedal's own defaults, from its pedal.json. */
        for (int k = 0; k < RAT_KNOBS; ++k) pedal_set(&r->eng, rat_index[k], pfx_defaults[rat_index[k]]);
        pedal_snap(&r->eng);
        r->mix = 1.0f;
        r->cold = 0;
        return r;
    }
    return NULL;
}

void rat_stage_destroy(rat_stage *r) {
    if (r) atomic_store(&r->used, 0);
}

void rat_stage_set(rat_stage *r, int knob, float value) {
    if (!r || knob < 0 || knob >= RAT_KNOBS || !(value >= 0.0f && value <= 1.0f)) return;
    pedal_set(&r->eng, rat_index[knob], value);
}

void rat_stage_snap(rat_stage *r) {
    if (r) pedal_snap(&r->eng);
}

void rat_stage_process(rat_stage *r, float *mono, int frames, int on) {
    if (!r || !mono) return;
    const float target = on ? 1.0f : 0.0f;
    float wet[RAT_MAX_BLOCK], twin[RAT_MAX_BLOCK];
    for (int done = 0; done < frames;) {
        int n = frames - done;
        if (n > PFX_CTRL) n = PFX_CTRL;
        float *x = mono + done;
        done += n;
        if (target == 0.0f && r->mix == 0.0f) { r->cold = 1; continue; }
        if (r->cold) {
            pedal_reset(&r->eng);
            pedal_snap(&r->eng);
            r->cold = 0;
        }
        pedal_control(&r->eng);
        /* pedal_run is stereo; identical channels through identical states
         * cost one channel (its own mono path). */
        memcpy(wet, x, (size_t)n * sizeof *x);
        memcpy(twin, x, (size_t)n * sizeof *x);
        pedal_run(&r->eng, wet, twin, n);
        int bad = 0;
        /* ~6 ms on/off crossfade: 1/16 per control block. */
        const float step = (target - r->mix) > 0.0f ? 0.0625f / (float)PFX_CTRL
                         : (target - r->mix) < 0.0f ? -0.0625f / (float)PFX_CTRL : 0.0f;
        for (int i = 0; i < n; ++i) {
            float y = wet[i];
            if (!(fabsf(y) <= RAT_LIMIT)) {
                bad |= !isfinite(y);
                y = isfinite(y) ? copysignf(RAT_LIMIT, y) : 0.0f;
            }
            r->mix = pfx_clampf(r->mix + step, 0.0f, 1.0f);
            x[i] += r->mix * (y - x[i]);
        }
        if (bad) pedal_reset(&r->eng);   /* recover rather than stay broken */
    }
    if (!r->cold) pedal_flush(&r->eng);
}
