/* Rat Drive: RAT-style distortion for Schwung (Ableton Move).
 *
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa.
 * Copyright (C) 2026 Charles Vestal (charlesvestal/schwung-303, src/dsp/drive.h,
 *   namespace drive::rat — the C++ port this file is derived from).
 * Copyright (C) Dave Mollen (davemollen/dm-Rat, GPL-3.0 — rat/src/op_amp.rs,
 *   op_amp/{bilinear_transform,op_amp_correction,third_order_iir_filter}.rs,
 *   clipper.rs, tone.rs, params.rs, shared/one_pole_filter.rs).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 3 of the License. See LICENSE-GPL-3.0.
 *
 * Unofficial emulation; not affiliated with or endorsed by ProCo / Radial.
 *
 * Signal path (per channel):
 *
 *   in ─ LowCut HPF ─ op-amp (3rd-order IIR, coefficients from the
 *   Distortion pot via bilinear transform) ─ GBW correction LPF ─ [4x:
 *   LM308 slew limit ─ output-swing limit ─ C7 4.7u ─ R6 1k ─ node D:
 *   1N914 pair to ground AND the Filter network (R7 1k5 + Filter pot 100k
 *   into C8 3n3, loaded by the JFET buffer's 1M) solved TOGETHER] ─
 *   Volume ─ HiCut LPF ─ DC block
 *
 * Ported unchanged from dm-Rat (via schwung-303): the op-amp s-domain
 * coefficients and constants, the bilinear transform, the third-order IIR,
 * the op-amp gain-bandwidth correction, the tone RC values, and the cubic
 * Distortion / Filter pot tapers.
 * Changed: 8x FIR (dm-Rat) / 2x biquad (schwung-303) oversampling replaced by
 * a 4x polyphase-IIR half-band around the nonlinear part only; LM308 slew
 * rate (0.3 V/us) and output-swing limits added before the diodes; Volume
 * uses this project's dB taper; LowCut / HiCut / Mix / Input / Output and
 * parameter smoothing at control rate are additions.
 *
 * v0.2: dm-Rat's clipper (a fitted curve x/(1+x^4)^(1/4) followed by a
 * separate one-pole tone filter) is replaced by the circuit itself. On the
 * pedal the diodes and the Filter share node D, so the Filter LOADS the
 * clipper: turned bright, R7 1k5 + C8 hang almost directly across the diodes
 * and pull the unclipped signal down against R6 1k; turned dark they barely
 * load it. The node is solved per sample: C8's node is eliminated
 * analytically (trapezoidal companion), leaving one diode node for the
 * closed-form solver in common/nonlinear.h.
 */
#include <string.h>
#include "common/smoothing.h"
#include "common/filters.h"
#include "common/oversampling.h"
#include "common/nonlinear.h"
#include "common/tapers.h"
#include "pedal_meta.h"

#define PFX_K_INPUT PK_INPUT
#define PFX_K_MIX PK_MIX
#define PFX_K_OUTPUT PK_OUTPUT

/* ---- dm-Rat constants (rat/src/op_amp.rs, op_amp_correction.rs) ---------- */
#define RAT_R1 100000.0f
#define RAT_C1 1e-10f
#define RAT_Z1_B0 2.72149e-7f
#define RAT_Z1_B1 0.0027354f
#define RAT_Z1_A0 6.27638e-9f
#define RAT_Z1_A1 0.0000069f
#define RAT_MAX_GAIN_AT_1HZ 1119360.558108f
#define RAT_MIN_DIST_GAIN 1.0f
#define RAT_MAX_DIST_GAIN 2307.231003f
/* tone.rs */
#define RAT_TONE_R1 100000.0f
#define RAT_TONE_R2 1500.0f
#define RAT_TONE_C1 3.3e-9f
/* clipping stage (ElectroSmash, "ProCo RAT analysis") */
#define RAT_R6 1000.0
#define RAT_C7_HZ 34.0            /* C7 4.7u into R6 1k: coupling high-pass */
#define RAT_BUFFER_R 1.0e6        /* JFET buffer input resistance at the Filter cap */
#define RAT_FS_OS ((double)PFX_FS * PFX_OS)

/* ---- additions ------------------------------------------------------------ */
#ifndef RAT_SLEW_V_PER_S
#define RAT_SLEW_V_PER_S 0.3e6f /* LM308 with its 30 pF compensation cap */
#endif
#define RAT_RAIL 3.9f           /* 9 V supply, 4.5 V bias, output swing */

/* v0.3 op-amp models: RAT = LM308 + 30 pF (the original), RAT2 = OP07DP (what
 * ProCo fitted from ~1996, the LM308 being obsolete). Same 0.3 V/us slew.
 *   GBW      LM308/30p ~52 dB open loop at 1 kHz (measured, PedalPCB forum);
 *            OP07 0.6 MHz (datasheet): ~1.5x the bandwidth -> "more presence"
 *   R_out    LM308 ~400 ohm at high gain (measured) vs OP07 ~60 ohm: in
 *            series with R6 1k, the LM308 drives the diodes softer
 *            ("spongier"), the OP07 harder ("keeps it together")
 *   recovery the externally compensated LM308 winds up past its rail and is
 *            slow to come back ("saggy at high gain"); the OP07 is not.
 *            Modelled as how far the slew-limited internal node may run
 *            past the output limit before it has to come back.
 * The first two are measured / datasheet facts; the recovery figures are
 * estimates chosen to give the reported difference. */
#define RAT_GBW_SCALE_OP07 1.5f
#define RAT_ROUT_LM308 400.0f
#define RAT_ROUT_OP07 60.0f
#define RAT_WINDUP_LM308 3.0f  /* volts past the rail: ~10 us to recover */
#define RAT_WINDUP_OP07 0.3f
#define RAT_CAL 0.2096f          /* output makeup: defaults ~ unity on a -6 dBFS-peak pluck (tools/analyze cal) */

typedef struct {
    float b[4], a[4];
} rat_iir_c;

typedef struct {
    pfx_bq_s lowcut, hicut;
    float z0, z1, z2;  /* op-amp IIR */
    float corr;        /* GBW correction one-pole */
    float slew;        /* last op-amp output (oversampled) */
    pfx_fo_s c7;       /* coupling cap into R6 */
    pfx_cap_s c8;      /* Filter capacitor (companion history) */
    pfx_os_s os;
    pfx_dc_s dc;
} rat_ch;

struct pedal {
    pfx_smooth dist, filt, vol, locut, hicut;
    pfx_smooth opamp;             /* 0 = LM308 (RAT), 1 = OP07DP (RAT2), crossfaded */
    float slew_lim;               /* internal-node limit, rail + windup */
    rat_iir_c iir;
    float corr_b1;
    pfx_diodes d;                /* 1N914 pair */
    pfx_node node;               /* node D, including the Filter branch */
    pfx_fo_c c7_c;
    float g6, gc8, alpha, inv_gt, inv_rf; /* see rat_clip_coefs */
    pfx_bq_c lowcut_c, hicut_c;
    float hicut_mix; /* 0..1: how much of the low-pass is heard */
    pfx_ramp vol_r;
    float vol_next;
    int dirty;
    rat_ch ch[2];
};

/* dm-Rat BilinearTransform: s-polynomials [s^3, s^2, s^1, s^0] -> z. */
static void rat_blt3(const float x[4], float out[4]) {
    const float t = 1.0f / PFX_FS;
    const float x0 = x[0], x1 = x[1] * t * 0.5f, x2 = x[2] * t * t * 0.25f,
                x3 = x[3] * t * t * t * 0.125f;
    out[0] = x0 + x1 + x2 + x3;
    out[1] = -3.0f * x0 - x1 + x2 + 3.0f * x3;
    out[2] = 3.0f * x0 - x1 - x2 + 3.0f * x3;
    out[3] = -x0 + x1 - x2 + x3;
}

/* dm-Rat OpAmp::get_s_domain_coefficients for pot value d (0..1, already
 * cubed): numerator and denominator, [s^3, s^2, s^1, s^0]. */
static void rat_opamp_s(float d, float num_s[4], float den_s[4]) {
    const float z2_b0 = fmaxf(d * RAT_R1, 1.0f);
    const float z2_a0 = z2_b0 * RAT_C1;
    const float a0 = RAT_Z1_B0 * z2_a0;
    const float a1 = RAT_Z1_B0 + RAT_Z1_B1 * z2_a0;
    const float a2 = RAT_Z1_B1 + z2_a0;
    const float b0 = a0;
    const float b1 = a1 + RAT_Z1_A0 * z2_b0;
    const float b2 = RAT_Z1_A1 * z2_b0 + RAT_Z1_B1 + z2_a0;
    num_s[0] = b0; num_s[1] = b1; num_s[2] = b2; num_s[3] = 1.0f;
    den_s[0] = a0; den_s[1] = a1; den_s[2] = a2; den_s[3] = 1.0f;
}

/* ... then bilinear, normalised so a[0] = 1. */
static void rat_opamp_coefs(rat_iir_c *c, float d) {
    float num_s[4], den_s[4], num_z[4], den_z[4];
    rat_opamp_s(d, num_s, den_s);
    rat_blt3(num_s, num_z);
    rat_blt3(den_s, den_z);
    const float inv = 1.0f / den_z[0];
    for (int i = 0; i < 4; ++i) {
        c->b[i] = num_z[i] * inv;
        c->a[i] = den_z[i] * inv;
    }
}

static void rat_coefs(struct pedal *p) {
    const float dk = p->dist.cur, d = dk * dk * dk; /* dm-Rat params.rs taper */
    rat_opamp_coefs(&p->iir, d);
    const float gain = d * (RAT_MAX_DIST_GAIN - RAT_MIN_DIST_GAIN) + RAT_MIN_DIST_GAIN;
    const float b = p->opamp.cur; /* 0 LM308 .. 1 OP07 */
    const float gbw = RAT_MAX_GAIN_AT_1HZ * (1.0f + (RAT_GBW_SCALE_OP07 - 1.0f) * b);
    p->corr_b1 = expf(-2.0f * (float)M_PI * (gbw / gain) / PFX_FS);
    p->g6 = 1.0f / ((float)RAT_R6 + RAT_ROUT_LM308 + (RAT_ROUT_OP07 - RAT_ROUT_LM308) * b);
    p->slew_lim = RAT_RAIL + RAT_WINDUP_LM308 + (RAT_WINDUP_OP07 - RAT_WINDUP_LM308) * b;
    /* Filter branch seen from node D. C8's node T:
     *   (VD - VT)/Rf = Gc VT - Ih + VT/Rbuf  ->  VT = alpha VD + Ih/Gt
     * so the branch draws  VD (1 - alpha)/Rf - (Ih/Gt)/Rf  from node D. */
    const float fk = p->filt.cur, f = fk * fk * fk; /* dm-Rat tone taper */
    const double rf = f * RAT_TONE_R1 + RAT_TONE_R2;
    const double gt = 1.0 / rf + p->gc8 + 1.0 / RAT_BUFFER_R;
    p->inv_rf = (float)(1.0 / rf);
    p->inv_gt = (float)(1.0 / gt);
    p->alpha = (float)((1.0 / rf) / gt);
    pfx_node_set(&p->node, &p->d, p->g6 + (1.0f - p->alpha) * p->inv_rf);
    pfx_bq_hp(&p->lowcut_c, pfx_freq_log(p->locut.cur, 20.0f, 1000.0f), 0.7071, PFX_FS);
    /* Hi Cut: the low-pass always runs (so it never starts from a stale
     * state) and is faded in over the first 5% of the knob, so 0% is
     * exactly transparent. */
    pfx_bq_lp(&p->hicut_c, pfx_freq_log(p->hicut.cur, 20000.0f, 1000.0f), 0.7071, PFX_FS);
    p->hicut_mix = fminf(1.0f, p->hicut.cur / 0.05f);
}

static void pedal_reset(struct pedal *p) { memset(p->ch, 0, sizeof p->ch); }

static void pedal_set(struct pedal *p, int idx, float v) {
    switch (idx) {
    case PK_DISTORTION: pfx_sm_set(&p->dist, v); break;
    case PK_FILTER: pfx_sm_set(&p->filt, v); break;
    case PK_VOLUME: pfx_sm_set(&p->vol, v); break;
    case PK_LOW_CUT: pfx_sm_set(&p->locut, v); break;
    case PK_HI_CUT: pfx_sm_set(&p->hicut, v); break;
    case PK_MODEL: pfx_sm_set(&p->opamp, v); break;
    default: return;
    }
    p->dirty = 1;
}

static void pedal_snap(struct pedal *p) {
    pfx_sm_snap(&p->dist); pfx_sm_snap(&p->filt); pfx_sm_snap(&p->vol);
    pfx_sm_snap(&p->locut); pfx_sm_snap(&p->hicut); pfx_sm_snap(&p->opamp);
    rat_coefs(p);
    p->vol_next = pfx_level_gain(p->vol.cur) * RAT_CAL;
    pfx_ramp_end(&p->vol_r, p->vol_next);
    p->dirty = 0;
}

static void pedal_init(struct pedal *p) {
    pfx_sm_init(&p->dist, 0.5f, 5.0f);   /* drive: fast, still zipper-free */
    pfx_sm_init(&p->filt, 0.5f, 10.0f);
    pfx_sm_init(&p->vol, 0.5f, 15.0f);
    pfx_sm_init(&p->locut, 0.0f, 10.0f);
    pfx_sm_init(&p->hicut, 0.0f, 10.0f);
    pfx_sm_init(&p->opamp, 0.0f, 20.0f); /* switching models glides, never clicks */
    p->d = pfx_diodes_si(1, 1, 1.0f); /* 1N914 ~ 1N4148 */
    p->gc8 = (float)(2.0 * RAT_TONE_C1 * RAT_FS_OS);
    pfx_fo_hp(&p->c7_c, RAT_C7_HZ, RAT_FS_OS);
    pedal_reset(p);
    pedal_snap(p);
}

static void pedal_control(struct pedal *p) {
    const int moving = pfx_sm_moving(&p->dist) | pfx_sm_moving(&p->filt) |
                       pfx_sm_moving(&p->locut) | pfx_sm_moving(&p->hicut) | pfx_sm_moving(&p->opamp);
    pfx_sm_step(&p->dist); pfx_sm_step(&p->filt); pfx_sm_step(&p->locut); pfx_sm_step(&p->hicut);
    pfx_sm_step(&p->opamp);
    pfx_sm_step(&p->vol);
    if (moving || p->dirty) rat_coefs(p);
    p->dirty = 0;
    p->vol_next = pfx_level_gain(p->vol.cur) * RAT_CAL;
    pfx_ramp_to(&p->vol_r, p->vol_next, PFX_CTRL);
}

static inline float rat_sample(const struct pedal *p, rat_ch *c, float x) {
    const rat_iir_c *k = &p->iir;
    x = pfx_bq(&p->lowcut_c, &c->lowcut, x * PFX_V_IN);
    /* dm-Rat ThirdOrderIIRFilter (transposed direct form II) */
    const float y = x * k->b[0] + c->z0;
    c->z0 = x * k->b[1] - y * k->a[1] + c->z1;
    c->z1 = x * k->b[2] - y * k->a[2] + c->z2;
    c->z2 = x * k->b[3] - y * k->a[3];
    /* dm-Rat OnePoleFilter as op-amp gain-bandwidth correction */
    c->corr = y * (1.0f - p->corr_b1) + c->corr * p->corr_b1;

    float up[PFX_OS];
    pfx_os_up(&c->os, c->corr, up);
    const float dmax = RAT_SLEW_V_PER_S / (PFX_FS * PFX_OS);
    for (int j = 0; j < PFX_OS; ++j) {
        const float v = pfx_clampf(c->slew + pfx_clampf(up[j] - c->slew, -dmax, dmax),
                                   -p->slew_lim, p->slew_lim);
        c->slew = v;
        const float vs = pfx_fo(&p->c7_c, &c->c7, pfx_rail(v, RAT_RAIL, RAT_RAIL, 0.7f));
        const float ih_t = c->c8.ih * p->inv_gt;
        const float vd = pfx_diode_fast(&p->d, &p->node, vs * p->g6 + ih_t * p->inv_rf);
        const float vt = p->alpha * vd + ih_t;   /* the Filter capacitor = the output */
        pfx_cap_update(&c->c8, p->gc8, vt);
        up[j] = vt;
    }
    return pfx_os_down(&c->os, up);
}

static inline float rat_full(const struct pedal *p, rat_ch *c, float x, float vol) {
    float y = rat_sample(p, c, x) * vol;
    const float lp = pfx_bq(&p->hicut_c, &c->hicut, y);
    y += p->hicut_mix * (lp - y);
    return pfx_dc(&c->dc, y, 0.9985f); /* ~10 Hz */
}

static void pedal_run(struct pedal *p, float *l, float *r, int n) {
    /* Mono content through identical states: run one channel, copy it
     * (bit-exact with running both); otherwise interleave the channels. */
    const int mono = pfx_is_mono(l, r, n) && !memcmp(&p->ch[0], &p->ch[1], sizeof p->ch[0]);
    pfx_ramp vol = p->vol_r;
    if (mono) {
        for (int i = 0; i < n; ++i) l[i] = rat_full(p, &p->ch[0], l[i], pfx_ramp_tick(&vol));
        p->ch[1] = p->ch[0];
        memcpy(r, l, (size_t)n * sizeof *l);
    } else {
        for (int i = 0; i < n; ++i) {
            const float g = pfx_ramp_tick(&vol);
            l[i] = rat_full(p, &p->ch[0], l[i], g);
            r[i] = rat_full(p, &p->ch[1], r[i], g);
        }
    }
    pfx_ramp_end(&p->vol_r, p->vol_next);
}

static void pedal_flush(struct pedal *p) {
    for (int ch = 0; ch < 2; ++ch) {
        rat_ch *c = &p->ch[ch];
        c->z0 = pfx_flush(c->z0); c->z1 = pfx_flush(c->z1); c->z2 = pfx_flush(c->z2);
        c->corr = pfx_flush(c->corr); c->slew = pfx_flush(c->slew);
        c->c7.z = pfx_flush(c->c7.z); c->c8.ih = pfx_flush(c->c8.ih);
        c->lowcut.z1 = pfx_flush(c->lowcut.z1); c->lowcut.z2 = pfx_flush(c->lowcut.z2);
        c->hicut.z1 = pfx_flush(c->hicut.z1); c->hicut.z2 = pfx_flush(c->hicut.z2);
        c->dc.x1 = pfx_flush(c->dc.x1); c->dc.y1 = pfx_flush(c->dc.y1);
        pfx_os_flush(&c->os);
    }
}

#include "common/plugin_v2.h"
