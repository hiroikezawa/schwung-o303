/* o303 - the 8-step acid line generator. See acid8.h.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 */
#include "acid8.h"

#include <string.h>

#define N ACID8_STEPS
#define CANDIDATES 8

/* ---- random: xorshift32, as TB-3PO uses ------------------------------------ */
typedef struct { uint32_t s; } rng_t;

static uint32_t rnd_u32(rng_t *r) {
    uint32_t x = r->s ? r->s : 0x2F6E2B1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}
static float rnd_f(rng_t *r) { return (float)(rnd_u32(r) & 0xFFFFFF) / (float)0x1000000; }
static int rnd_below(rng_t *r, int n) { return n > 0 ? (int)(rnd_u32(r) % (uint32_t)n) : 0; }

/* Index of one of `n` weights, drawn in proportion; -1 if they are all 0. */
static int pick_weighted(rng_t *r, const float *w, int n) {
    float total = 0.0f;
    for (int i = 0; i < n; ++i) total += w[i] > 0.0f ? w[i] : 0.0f;
    if (total <= 0.0f) return -1;
    float x = rnd_f(r) * total;
    for (int i = 0; i < n; ++i) {
        if (w[i] <= 0.0f) continue;
        if (x < w[i]) return i;
        x -= w[i];
    }
    for (int i = n - 1; i >= 0; --i) if (w[i] > 0.0f) return i;
    return -1;
}

/* ---- styles ------------------------------------------------------------------ */
typedef struct { int semi; float weight; } interval_t;

typedef struct {
    float hits_bias;          /* added to Density's hit count */
    int max_hits;
    float repeat;             /* chance a note repeats the previous pitch */
    int max_leap;             /* semitones between neighbours (an exact octave is always allowed) */
    float accent_mul, slide_mul;
    float accent_at[N];       /* where accents like to fall */
    float min_root;           /* share of sounding steps on the root (any octave) */
    int below_root;           /* may sit an octave up to reach notes under the root */
    interval_t iv[10];        /* weight 0 ends the list */
} style_t;

static const style_t STYLES[ACID8_STYLES] = {
    [ACID8_HYPNOTIC] = {
        -0.5f, 7, 0.45f, 7, 1.0f, 0.5f,
        {2, 1, 3, 2, 2, 1, 3, 2}, 0.5f, 0,
        {{0, 60}, {7, 10}, {3, 8}, {4, 8}, {12, 8}, {10, 6}, {5, 4}, {-2, 4}, {0, 0}},
    },
    [ACID8_INDUSTRIAL] = {
        0.0f, 7, 0.30f, 7, 1.3f, 0.7f,
        {5, 1, 2, 2, 5, 1, 2, 2}, 0.4f, 1,
        {{0, 42}, {1, 24}, {-1, 24}, {12, 6}, {3, 5}, {4, 5}, {6, 4}, {7, 4}, {10, 4}, {0, 0}},
    },
    [ACID8_ROLLING] = {
        1.0f, 8, 0.15f, 12, 0.8f, 1.4f,
        {4, 1, 2, 1, 4, 1, 2, 1}, 0.3f, 0,
        {{0, 34}, {7, 20}, {12, 18}, {3, 8}, {4, 8}, {10, 8}, {5, 6}, {-5, 4}, {0, 0}},
    },
    [ACID8_MINIMAL] = {
        -1.0f, 7, 0.50f, 12, 1.0f, 0.5f,
        {3, 1, 1, 3, 1, 1, 3, 1}, 0.8f, 0,
        {{0, 84}, {12, 12}, {7, 4}, {0, 0}},
    },
    [ACID8_BROKEN] = {
        -1.5f, 7, 0.20f, 6, 1.0f, 0.6f,
        {2, 2, 2, 2, 2, 2, 2, 2}, 0.4f, 1,
        {{0, 42}, {1, 14}, {-1, 12}, {6, 10}, {3, 8}, {4, 8}, {7, 6}, {10, 6}, {-2, 4}, {0, 0}},
    },
};

/* The rhythm skeletons, step 1 first (X = a note). */
static const char *const SKELETONS[] = {
    "X-XX-X-X", "XX-XX-XX", "X-XXX-X-", "XXX-XX-X", "X-X-XX-X", "XX--XXX-",
    "X--X--X-", "X-X-X-X-", "XXX-XXXX", "X--XX-X-", "X-XX-XX-", "XX-X-XX-",
};
#define NUM_SKELETONS ((int)(sizeof SKELETONS / sizeof SKELETONS[0]))

/* ---- pitch resolution ------------------------------------------------------- *
 * An interval in semitones from the pattern's home root -> a scale degree and
 * an octave, or not at all: the scale has to contain the pitch class, and the
 * octave has to be one Octaves allows. */
static int resolve(const acid8_in *in, int home, int semi, int *deg, int *oct) {
    const int pc = ((semi % 12) + 12) % 12;
    const int o = home + (semi >= 0 ? semi / 12 : -((11 - semi) / 12));
    if (o < 0 || o >= in->octaves) return 0;
    for (int d = 0; d < in->scale_len; ++d)
        if (in->scale[d] == pc) { *deg = d; *oct = o; return 1; }
    return 0;
}

static int pitch_of(const acid8_in *in, int deg, int oct) { return in->scale[deg] + 12 * oct; }

int acid8_pitch(const acid8_in *in, const acid8_out *p, int step) {
    return pitch_of(in, p->degrees[step], p->octaves[step]);
}

static int iabs(int x) { return x < 0 ? -x : x; }

/* ---- one candidate ------------------------------------------------------------ */
static int hits_of(const uint8_t *on) { int n = 0; for (int i = 0; i < N; ++i) n += on[i]; return n; }

static void make_rhythm(rng_t *r, int target, uint8_t *on) {
    const char *sk = SKELETONS[rnd_below(r, NUM_SKELETONS)];
    for (int i = 0; i < N; ++i) on[i] = sk[i] == 'X';
    /* A shifted hit: the groove moves without losing its shape. */
    if (rnd_f(r) < 0.6f) {
        const int i = 1 + rnd_below(r, N - 1), j = i == N - 1 ? i - 1 : i + 1;
        if (on[i] != on[j] && j != 0) { const uint8_t t = on[i]; on[i] = on[j]; on[j] = t; }
    }
    on[0] = 1;                                    /* the loop starts on a note */
    /* Toward what Density asks for: add on offbeats, take from weak steps. */
    for (int guard = 0; guard < 16 && hits_of(on) < target; ++guard) {
        float w[N];
        for (int i = 0; i < N; ++i) w[i] = on[i] ? 0.0f : (i & 1 ? 2.0f : 1.0f);
        const int i = pick_weighted(r, w, N);
        if (i < 0) break;
        on[i] = 1;
    }
    for (int guard = 0; guard < 16 && hits_of(on) > target; ++guard) {
        float w[N];
        for (int i = 0; i < N; ++i) w[i] = (on[i] && i) ? (i & 1 ? 2.0f : (i == 4 ? 0.5f : 1.0f)) : 0.0f;
        const int i = pick_weighted(r, w, N);
        if (i < 0) break;
        on[i] = 0;
    }
}

static void make_pitches(rng_t *r, const acid8_in *in, const style_t *st, int home,
                         const uint8_t *on, acid8_out *out) {
    int prev = -1000, first = 0;
    int last_hit = 0;
    for (int i = 0; i < N; ++i) if (on[i]) last_hit = i;
    for (int i = 0; i < N; ++i) {
        out->degrees[i] = 0;
        out->octaves[i] = (uint8_t)home;
        if (!on[i]) continue;
        int deg = 0, oct = home;
        if (i == 0) {
            resolve(in, home, 0, &deg, &oct);    /* home: the root */
        } else if (prev > -1000 && rnd_f(r) < st->repeat) {
            /* the previous SOUNDING step, again */
            for (int k = i - 1; k >= 0; --k) if (on[k]) { deg = out->degrees[k]; oct = out->octaves[k]; break; }
        } else {
            float w[10];
            int dg[10], oc[10], n = 0;
            for (int k = 0; k < 10 && st->iv[k].weight > 0.0f; ++k, ++n) {
                w[k] = 0.0f;
                if (!resolve(in, home, st->iv[k].semi, &dg[k], &oc[k])) continue;
                const int p = pitch_of(in, dg[k], oc[k]);
                const int leap = iabs(p - prev);
                if (leap > st->max_leap && leap != 12) continue;
                w[k] = st->iv[k].weight;
                /* The last note leads home: root, fifth, a semitone, the octave. */
                if (i == last_hit) {
                    const int to_first = iabs(p - first);
                    if (to_first <= 2 || to_first == 7 || to_first == 5 || to_first == 12) w[k] *= 2.0f;
                    else if (to_first > 7) w[k] *= 0.3f;
                }
            }
            const int k = pick_weighted(r, w, n);
            if (k >= 0) { deg = dg[k]; oct = oc[k]; }
            else resolve(in, home, 0, &deg, &oct);
        }
        out->degrees[i] = (uint8_t)deg;
        out->octaves[i] = (uint8_t)oct;
        prev = pitch_of(in, deg, oct);
        if (i == 0) first = prev;
    }
}

/* Draw `count` of the steps whose weight is > 0, without replacement. */
static int draw(rng_t *r, float *w, int count, uint8_t *chosen) {
    int got = 0;
    memset(chosen, 0, N);
    for (; got < count; ++got) {
        const int i = pick_weighted(r, w, N);
        if (i < 0) break;
        chosen[i] = 1;
        w[i] = 0.0f;
    }
    return got;
}

static int round_i(float x) { return (int)(x + 0.5f); }

static void make_accents_and_slides(rng_t *r, const acid8_in *in, const style_t *st,
                                    const uint8_t *on, acid8_out *out) {
    const int hits = hits_of(on);
    for (int i = 0; i < N; ++i) out->steps[i] = on[i] ? ACID8_NOTE : ACID8_REST;

    /* Accents: on the beats the style likes, never on every note. */
    int na = round_i(in->accent * (float)hits * st->accent_mul);
    if (in->accent > 0.02f && na < 1) na = 1;
    if (na > hits - 1) na = hits - 1;
    float w[N];
    uint8_t acc[N], sl[N];
    for (int i = 0; i < N; ++i) w[i] = on[i] ? st->accent_at[i] * (0.6f + 0.8f * rnd_f(r)) : 0.0f;
    draw(r, w, na, acc);

    /* Slides: from a sounding step into the next sounding step (step 8 into
     * step 1 across the loop), never out of an accented one (a step is one
     * of note / accent / slide), semitone and same-note moves first. */
    int ns = round_i(in->slide * (float)hits * st->slide_mul);
    if (ns > hits / 2) ns = hits / 2;
    for (int i = 0; i < N; ++i) {
        const int j = (i + 1) % N;
        w[i] = 0.0f;
        if (!on[i] || !on[j] || acc[i]) continue;
        const int d = iabs(acid8_pitch(in, out, j) - acid8_pitch(in, out, i));
        w[i] = d == 1 ? 6.0f : d == 0 ? 3.0f : d == 2 ? 2.0f : d <= 5 ? 1.5f
             : d == 12 ? (st == &STYLES[ACID8_ROLLING] ? 3.0f : 0.8f) : d <= 7 ? 1.0f : 0.2f;
        if (st == &STYLES[ACID8_INDUSTRIAL] && d > 2) w[i] *= 0.3f;    /* short slides */
    }
    draw(r, w, ns, sl);
    /* At most two slides in a row: a chain longer than that is one long note. */
    for (int i = 0; i < N; ++i)
        if (sl[i] && sl[(i + N - 1) % N] && sl[(i + N - 2) % N]) sl[i] = 0;

    for (int i = 0; i < N; ++i)
        if (on[i]) out->steps[i] = acc[i] ? ACID8_ACCENT : sl[i] ? ACID8_SLIDE : ACID8_NOTE;
}

/* ---- how good a loop is ---------------------------------------------------------- */
static float score(const acid8_in *in, const style_t *st, const acid8_out *p) {
    uint8_t on[N];
    int hits = 0, roots = 0, accents = 0, slides = 0, distinct = 0, seen[64] = {0};
    for (int i = 0; i < N; ++i) {
        on[i] = p->steps[i] != ACID8_REST;
        if (!on[i]) continue;
        ++hits;
        const int pt = acid8_pitch(in, p, i);
        roots += in->scale[p->degrees[i]] == 0;
        if (pt >= 0 && pt < 64 && !seen[pt]++) ++distinct;
        accents += p->steps[i] == ACID8_ACCENT;
        slides += p->steps[i] == ACID8_SLIDE;
    }
    float s = 0.0f;
    s -= 2.0f * (float)iabs(hits - p->target_hits);
    const float root_share = (float)roots / (float)(hits ? hits : 1);
    s += 4.0f * root_share;
    if (root_share < st->min_root) s -= 20.0f * (st->min_root - root_share) + 5.0f;
    if (st == &STYLES[ACID8_MINIMAL]) {
        if (distinct > 3) s -= 4.0f;
    } else {
        if (distinct < 2) s -= 6.0f;                       /* one note, all through */
        if (distinct > 4) s -= 1.5f * (float)(distinct - 4);
    }
    /* Leaps, the loop's wrap included. */
    int prev = -1, first = -1, longest_rest = 0, run = 0;
    for (int k = 0; k < 2 * N; ++k) {                        /* twice round: the rest run wraps */
        const int i = k % N;
        if (!on[i]) { if (++run > longest_rest) longest_rest = run; continue; }
        run = 0;
        if (k >= N) continue;
        const int pt = acid8_pitch(in, p, i);
        if (prev >= 0) { const int d = iabs(pt - prev); if (d > 7 && d != 12) s -= 0.5f * (float)(d - 7); }
        if (first < 0) first = pt;
        prev = pt;
    }
    {
        const int d = iabs(first - prev);                    /* the last note into step 1 */
        if (d <= 2 || d == 5 || d == 7 || d == 12) s += 2.0f;
        else if (d > 7) s -= 1.0f;
    }
    if (longest_rest > 3) s -= 3.0f;

    /* What makes each style itself, counted between neighbouring notes. */
    int semis = 0, odd = 0, fifths = 0, repeats = 0, rests = N - hits;
    for (int i = 0, last = -1; i < N; ++i) {
        if (!on[i]) continue;
        const int pt = acid8_pitch(in, p, i);
        if (last >= 0) {
            const int d = iabs(pt - last);
            semis += d == 1;
            odd += d == 1 || d == 6;
            fifths += d == 7 || d == 5 || d == 12;
            repeats += d == 0;
        }
        last = pt;
    }
    if (st == &STYLES[ACID8_INDUSTRIAL]) s += 1.5f * (float)(semis < 3 ? semis : 3);
    else if (st == &STYLES[ACID8_BROKEN]) s += (float)(odd < 2 ? odd : 2) + (rests >= 3 ? 1.0f : 0.0f);
    else if (st == &STYLES[ACID8_ROLLING]) s += (float)(fifths < 3 ? fifths : 3) + (slides >= 2 ? 1.0f : 0.0f);
    else if (st == &STYLES[ACID8_HYPNOTIC]) s += 0.5f * (float)(repeats < 4 ? repeats : 4);
    if (in->accent > 0.02f && accents == 0) s -= 5.0f;
    if (accents >= hits) s -= 10.0f;
    if (slides >= hits) s -= 10.0f;
    /* The same note, the same way, on every step: nothing for the ear. */
    int same = 1;
    for (int i = 1; i < N; ++i)
        if (p->steps[i] != p->steps[0] || p->degrees[i] != p->degrees[0] || p->octaves[i] != p->octaves[0]) same = 0;
    if (same) s -= 15.0f;
    return s;
}

void acid8_generate(const acid8_in *in, uint32_t seed, int style, acid8_out *out) {
    rng_t r = {seed ? seed : 1u};
    for (int i = 0; i < 4; ++i) rnd_u32(&r);                 /* decorrelate nearby seeds */
    if (style < 0 || style >= ACID8_STYLES) style = rnd_below(&r, ACID8_STYLES);
    const style_t *st = &STYLES[style];

    float t = 2.0f + 6.0f * in->density + st->hits_bias;
    int target = round_i(t);
    if (target < 2) target = 2;
    if (target > st->max_hits) target = st->max_hits;

    acid8_out best, cand;
    float best_score = -1e30f;
    memset(&best, 0, sizeof best);
    for (int c = 0; c < CANDIDATES; ++c) {
        memset(&cand, 0, sizeof cand);
        cand.style = style;
        cand.target_hits = target;
        /* Notes under the root are reached by sitting one octave up. */
        const int home = (st->below_root && in->octaves >= 2 && rnd_f(&r) < 0.4f) ? 1 : 0;
        uint8_t on[N];
        make_rhythm(&r, target, on);
        make_pitches(&r, in, st, home, on, &cand);
        make_accents_and_slides(&r, in, st, on, &cand);
        const float sc = score(in, st, &cand);
        if (sc > best_score) { best_score = sc; best = cand; }
    }
    *out = best;
}
