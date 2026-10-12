/* Schwung audio FX API v2 adapter shared by every pedal.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Hiro Ikezawa. Original work.
 *
 * Included LAST by src/<pedal>/<pedal>.c, after the generated pedal_meta.h and
 * after the pedal defines:
 *
 *   PFX_K_INPUT / PFX_K_MIX / PFX_K_OUTPUT   knob index, or -1 if absent
 *   struct pedal and
 *     pedal_init(p)            fresh instance: coefficients + clear state
 *     pedal_reset(p)           clear audio state only
 *     pedal_set(p, idx, v)     knob target (0..1); ignore framework knobs
 *     pedal_snap(p)            jump every smoother to its target
 *     pedal_control(p)         one control-rate step (smoothers + coeffs)
 *     pedal_run(p, l, r, n)    process n <= PFX_CTRL frames of the wet path
 *     pedal_flush(p)           denormal flush of the state, once per block
 *
 * The adapter owns Input (pre-pedal gain), Mix (equal-power) and Output, the
 * zero-cost bypass at Mix = 0, the NaN/Inf guard, presets and state.
 *
 * Real-time rules: nothing here allocates, locks, logs or touches a file.
 * create/destroy take a slot from a static pool with one atomic CAS, because
 * the chain host calls them on the SPI callback too.
 */
#ifndef PFX_PLUGIN_V2_H
#define PFX_PLUGIN_V2_H

#include "host/audio_fx_api_v2.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#define PFX_INSTANCES 16
#define PFX_WET_LIMIT 4.0f /* +12 dBFS: only a broken state gets here */

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "instance pool needs lock-free atomics");

typedef struct {
    atomic_int used;
    float knob[PFX_NPARAM];
    int preset;
    int cold; /* wet path skipped (Mix = 0); state is stale */
    pfx_smooth in_s, mix_s, out_s;
    pfx_ramp in_r, dry_r, wet_r, out_r;
    struct pedal eng;
} pfx_instance;

static pfx_instance pfx_pool[PFX_INSTANCES];

/* ---- denormals: flush-to-zero for our block only ------------------------- *
 * FZ is a per-thread control bit and this thread is Move's SPI thread, so it
 * is restored before returning rather than left set. */
#if defined(__aarch64__)
typedef uint64_t pfx_fpmode;
static inline pfx_fpmode pfx_ftz_enter(void) {
    uint64_t old;
    __asm__ volatile("mrs %0, fpcr" : "=r"(old));
    __asm__ volatile("msr fpcr, %0" ::"r"(old | (1ull << 24)));
    return old;
}
static inline void pfx_ftz_leave(pfx_fpmode m) { __asm__ volatile("msr fpcr, %0" ::"r"(m)); }
#elif defined(__x86_64__) || defined(__i386__)
typedef unsigned pfx_fpmode;
static inline pfx_fpmode pfx_ftz_enter(void) {
    const unsigned old = _mm_getcsr();
    _mm_setcsr(old | 0x8040u); /* FTZ | DAZ */
    return old;
}
static inline void pfx_ftz_leave(pfx_fpmode m) { _mm_setcsr(m); }
#else
typedef int pfx_fpmode;
static inline pfx_fpmode pfx_ftz_enter(void) { return 0; }
static inline void pfx_ftz_leave(pfx_fpmode m) { (void)m; }
#endif

/* ---- framework knobs ------------------------------------------------------ */
static inline float pfx_input_gain(float x) { return pfx_db2lin(pfx_db_knob(x, 24.0f)); }
static inline float pfx_output_gain(float x) { return pfx_db2lin(pfx_db_knob(x, 18.0f)); }

/* Knobs are continuous 0..1; a switch (pfx_param_steps > 0) is an integer
 * index 0..steps, so any number is rounded and clamped onto it. */
static void pfx_apply_knob(pfx_instance *in, int idx, float v) {
    const int steps = pfx_param_steps[idx];
    const float hi = steps ? (float)steps : 1.0f;
    v = v < 0.0f ? 0.0f : (v > hi ? hi : v);
    if (steps) v = floorf(v + 0.5f);
    in->knob[idx] = v;
    if (idx == PFX_K_INPUT) pfx_sm_set(&in->in_s, pfx_input_gain(v));
    else if (idx == PFX_K_MIX) pfx_sm_set(&in->mix_s, v);
    else if (idx == PFX_K_OUTPUT) pfx_sm_set(&in->out_s, pfx_output_gain(v));
    else pedal_set(&in->eng, idx, v);
}

static void pfx_snap_all(pfx_instance *in) {
    pfx_sm_snap(&in->in_s);
    pfx_sm_snap(&in->mix_s);
    pfx_sm_snap(&in->out_s);
    const float m = in->mix_s.cur * 1.57079632679f;
    pfx_ramp_end(&in->in_r, in->in_s.cur);
    pfx_ramp_end(&in->dry_r, cosf(m));
    pfx_ramp_end(&in->wet_r, sinf(m));
    pfx_ramp_end(&in->out_r, in->out_s.cur);
    pedal_snap(&in->eng);
}

static void pfx_load_preset(pfx_instance *in, int idx) {
    if (idx < 0 || idx >= PFX_PRESET_COUNT) return;
    in->preset = idx;
    for (int i = 0; i < PFX_NPARAM; ++i) pfx_apply_knob(in, i, pfx_presets[idx].v[i]);
}

/* ---- lifecycle ------------------------------------------------------------ */
static void *pfx_create(const char *dir, const char *config) {
    (void)dir; (void)config;
    for (int i = 0; i < PFX_INSTANCES; ++i) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&pfx_pool[i].used, &expected, 1)) continue;
        pfx_instance *in = &pfx_pool[i];
        pfx_sm_init(&in->in_s, 1.0f, 15.0f);
        pfx_sm_init(&in->mix_s, 1.0f, 15.0f);
        pfx_sm_init(&in->out_s, 1.0f, 15.0f);
        pedal_init(&in->eng);
        in->preset = 0;
        in->cold = 0;
        for (int k = 0; k < PFX_NPARAM; ++k) pfx_apply_knob(in, k, pfx_defaults[k]);
        pfx_snap_all(in);
        return in;
    }
    return NULL;
}

static void pfx_destroy(void *ptr) {
    if (ptr) atomic_store(&((pfx_instance *)ptr)->used, 0);
}

/* ---- parameters ------------------------------------------------------------ */
static int pfx_key_index(const char *key) {
    for (int i = 0; i < PFX_NPARAM; ++i)
        if (!strcmp(key, pfx_keys[i])) return i;
    return -1;
}

static const char *pfx_skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

static int pfx_parse_number(const char *s, float *out) {
    char *end;
    const float v = strtof(s, &end);
    if (end == s || *pfx_skip_ws(end) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* State: one flat JSON object of numbers, {"v":1,"preset":n,"<key>":x,...}.
 * Bounded and transactional: nothing is applied unless the whole document
 * parses. Unknown keys are ignored (a newer build may add some); a missing
 * knob keeps its default. */
static int pfx_restore(pfx_instance *in, const char *text) {
    if (strlen(text) > 2048) return 0;
    float next[PFX_NPARAM];
    memcpy(next, pfx_defaults, sizeof next);
    int version = 0, preset = 0;
    const char *p = pfx_skip_ws(text);
    if (*p++ != '{') return 0;
    for (int field = 0; field < 32; ++field) {
        p = pfx_skip_ws(p);
        if (*p == '}' && field == 0) break;
        if (*p++ != '"') return 0;
        char key[32];
        int n = 0;
        while (*p && *p != '"' && n < (int)sizeof key - 1) key[n++] = *p++;
        key[n] = 0;
        if (*p++ != '"') return 0;
        p = pfx_skip_ws(p);
        if (*p++ != ':') return 0;
        p = pfx_skip_ws(p);
        char *end;
        const float v = strtof(p, &end);
        if (end == p || !isfinite(v)) return 0;
        p = pfx_skip_ws(end);
        const int idx = pfx_key_index(key);
        if (!strcmp(key, "v")) version = (int)v;
        else if (!strcmp(key, "preset")) preset = (int)v;
        else if (idx >= 0) {
            if (v < 0.0f || v > (pfx_param_steps[idx] ? (float)pfx_param_steps[idx] : 1.0f)) return 0;
            next[idx] = v;
        }
        if (*p == '}') break;
        if (*p++ != ',') return 0;
    }
    if (*p++ != '}' || *pfx_skip_ws(p) || version != 1) return 0;
    for (int i = 0; i < PFX_NPARAM; ++i) pfx_apply_knob(in, i, next[i]);
    in->preset = (preset >= 0 && preset < PFX_PRESET_COUNT) ? preset : 0;
    pfx_snap_all(in); /* a restored slot starts where it was, not ramping */
    return 1;
}

static void pfx_set_param(void *ptr, const char *key, const char *val) {
    if (!ptr || !key || !val) return;
    pfx_instance *in = ptr;
    float v;
    if (!strcmp(key, "state")) { (void)pfx_restore(in, val); return; }
    if (!strcmp(key, "preset")) {
        if (pfx_parse_number(val, &v) && v == floorf(v)) pfx_load_preset(in, (int)v);
        return;
    }
    const int idx = pfx_key_index(key);
    if (idx < 0) return;
    if (pfx_param_options[idx]) { /* a switch also accepts its option name */
        const char *s = pfx_skip_ws(val);
        for (int o = 0; pfx_param_options[idx][o]; ++o) {
            const size_t n = strlen(pfx_param_options[idx][o]);
            if (!strncmp(s, pfx_param_options[idx][o], n) && !*pfx_skip_ws(s + n)) {
                pfx_apply_knob(in, idx, (float)o);
                return;
            }
        }
    }
    if (pfx_parse_number(val, &v)) pfx_apply_knob(in, idx, v);
}

static int pfx_copy(char *buf, int size, const char *text) {
    const size_t n = strlen(text);
    if (n >= (size_t)size) { buf[0] = 0; return -1; }
    memcpy(buf, text, n + 1);
    return (int)n;
}

static int pfx_get_param(void *ptr, const char *key, char *buf, int size) {
    if (!ptr || !key || !buf || size <= 0) return -1;
    pfx_instance *in = ptr;
    buf[0] = 0;
    int n = -1;
    const int idx = pfx_key_index(key);
    if (idx >= 0 && pfx_param_steps[idx]) n = snprintf(buf, size, "%d", (int)in->knob[idx]);
    else if (idx >= 0) n = snprintf(buf, size, "%.4f", in->knob[idx]);
    else if (!strcmp(key, "name")) return pfx_copy(buf, size, PFX_NAME);
    else if (!strcmp(key, "preset")) n = snprintf(buf, size, "%d", in->preset);
    else if (!strcmp(key, "preset_count")) n = snprintf(buf, size, "%d", PFX_PRESET_COUNT);
    else if (!strcmp(key, "preset_name")) return pfx_copy(buf, size, pfx_presets[in->preset].name);
    else if (!strcmp(key, "chain_params")) return pfx_copy(buf, size, pfx_chain_params);
    else if (!strcmp(key, "ui_hierarchy")) return pfx_copy(buf, size, pfx_ui_hierarchy);
    else if (!strcmp(key, "state")) {
        n = snprintf(buf, size, "{\"v\":1,\"preset\":%d", in->preset);
        for (int i = 0; i < PFX_NPARAM && n > 0 && n < size; ++i)
            n += pfx_param_steps[i] ? snprintf(buf + n, size - n, ",\"%s\":%d", pfx_keys[i], (int)in->knob[i])
                                    : snprintf(buf + n, size - n, ",\"%s\":%.4f", pfx_keys[i], in->knob[i]);
        if (n > 0 && n < size) n += snprintf(buf + n, size - n, "}");
    }
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

/* ---- audio ----------------------------------------------------------------- */
static void pfx_process(void *ptr, int16_t *audio, int frames) {
    if (!ptr || !audio || frames <= 0) return;
    pfx_instance *in = ptr;
    const pfx_fpmode fp = pfx_ftz_enter();
    float dl[MOVE_FRAMES_PER_BLOCK], dr[MOVE_FRAMES_PER_BLOCK];
    float wl[MOVE_FRAMES_PER_BLOCK], wr[MOVE_FRAMES_PER_BLOCK];
    for (int done = 0; done < frames;) {
        int n = frames - done;
        if (n > MOVE_FRAMES_PER_BLOCK) n = MOVE_FRAMES_PER_BLOCK;
        int16_t *io = audio + 2 * done;
        for (int i = 0; i < n; ++i) {
            dl[i] = io[2 * i] * (1.0f / 32768.0f);
            dr[i] = io[2 * i + 1] * (1.0f / 32768.0f);
        }
        for (int s = 0; s < n; s += PFX_CTRL) {
            const int m = n - s < PFX_CTRL ? n - s : PFX_CTRL;
            const float g_in = pfx_sm_step(&in->in_s);
            const float mix = pfx_sm_step(&in->mix_s);
            const float g_out = pfx_sm_step(&in->out_s);
            const float g_dry = cosf(mix * 1.57079632679f), g_wet = sinf(mix * 1.57079632679f);
            pfx_ramp_to(&in->in_r, g_in, m);
            pfx_ramp_to(&in->dry_r, g_dry, m);
            pfx_ramp_to(&in->wet_r, g_wet, m);
            pfx_ramp_to(&in->out_r, g_out, m);
            float *xl = dl + s, *xr = dr + s, *yl = wl + s, *yr = wr + s;
            if (mix == 0.0f && in->mix_s.target == 0.0f && in->wet_r.value == 0.0f) {
                /* Bypass: the wet path costs nothing and its state goes cold. */
                in->cold = 1;
                for (int i = 0; i < m; ++i) {
                    const float g = pfx_ramp_tick(&in->out_r);
                    xl[i] *= g;
                    xr[i] *= g;
                }
            } else {
                if (in->cold) {
                    pedal_reset(&in->eng);
                    pedal_snap(&in->eng);
                    in->cold = 0;
                }
                pedal_control(&in->eng);
                for (int i = 0; i < m; ++i) {
                    const float g = pfx_ramp_tick(&in->in_r);
                    yl[i] = xl[i] * g;
                    yr[i] = xr[i] * g;
                }
                pedal_run(&in->eng, yl, yr, m);
                int bad = 0;
                for (int i = 0; i < m; ++i) {
                    if (!(fabsf(yl[i]) <= PFX_WET_LIMIT)) { bad |= !isfinite(yl[i]); yl[i] = isfinite(yl[i]) ? copysignf(PFX_WET_LIMIT, yl[i]) : 0.0f; }
                    if (!(fabsf(yr[i]) <= PFX_WET_LIMIT)) { bad |= !isfinite(yr[i]); yr[i] = isfinite(yr[i]) ? copysignf(PFX_WET_LIMIT, yr[i]) : 0.0f; }
                    const float gd = pfx_ramp_tick(&in->dry_r), gw = pfx_ramp_tick(&in->wet_r);
                    const float go = pfx_ramp_tick(&in->out_r);
                    xl[i] = (xl[i] * gd + yl[i] * gw) * go;
                    xr[i] = (xr[i] * gd + yr[i] * gw) * go;
                }
                if (bad) pedal_reset(&in->eng); /* recover rather than stay broken */
            }
            pfx_ramp_end(&in->in_r, g_in);
            pfx_ramp_end(&in->dry_r, g_dry);
            pfx_ramp_end(&in->wet_r, g_wet);
            pfx_ramp_end(&in->out_r, g_out);
        }
        pedal_flush(&in->eng);
        for (int i = 0; i < n; ++i) {
            const float l = dl[i] * 32768.0f, r = dr[i] * 32768.0f;
            io[2 * i] = (int16_t)(l >= 32767.0f ? 32767.0f : (l <= -32768.0f ? -32768.0f : l));
            io[2 * i + 1] = (int16_t)(r >= 32767.0f ? 32767.0f : (r <= -32768.0f ? -32768.0f : r));
        }
        done += n;
    }
    pfx_ftz_leave(fp);
}

static audio_fx_api_v2_t pfx_api = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .create_instance = pfx_create,
    .destroy_instance = pfx_destroy,
    .process_block = pfx_process,
    .set_param = pfx_set_param,
    .get_param = pfx_get_param,
    .on_midi = NULL,
};

__attribute__((visibility("default")))
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    /* Every coefficient assumes 44.1 kHz; refuse a host that says otherwise. */
    if (host && host->sample_rate && host->sample_rate != (int)PFX_FS) return NULL;
    return &pfx_api;
}

#endif
