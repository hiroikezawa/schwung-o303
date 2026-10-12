/* o303 - the 8-step acid line generator (dsp/acid8.c), on its own.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * Every rule the generator promises, over tens of thousands of random
 * settings: the loop starts on the root, has a sane number of notes, uses
 * only the scale and the octaves allowed, keeps leaps small, never slides
 * into a rest (the wrap into step 1 included), never accents or slides every
 * note -- and that Density, Accent, Slide and Octaves each move what they
 * should. The five styles must all occur, and MINIMAL must stay on the root.
 */
#include "acid8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define N ACID8_STEPS

/* tb3po.c's SCALES, degree by degree. */
static const int SC[6][7] = {
    {0, 2, 3, 5, 7, 8, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11},
    {0, 3, 5, 7, 10, 0, 0}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 11},
};
static const int SC_LEN[6] = {7, 7, 7, 5, 7, 7};
enum { MINOR, PHRYGIAN, HARMMINOR, MINPENT, DORIAN, MAJOR };

static unsigned r = 99;
static unsigned rnd(void) { r = r * 1664525u + 1013904223u; return r >> 8; }
static float rndf(void) { return (float)(rnd() & 0xFFFF) / 65536.0f; }
static int iabs(int x) { return x < 0 ? -x : x; }

typedef struct { int hits, accents, slides, roots, semis, top_oct; } stats_t;

static stats_t check_one(const acid8_in *in, const acid8_out *o) {
    stats_t st = {0, 0, 0, 0, 0, 0};
    CHECK(o->style >= 0 && o->style < ACID8_STYLES);
    CHECK(o->steps[0] != ACID8_REST);                              /* the loop starts on a note ... */
    CHECK(in->scale[o->degrees[0]] == 0);                           /* ... on the root */
    int slide_run = 0, max_run = 0, prev = -1;
    for (int k = 0; k < 2 * N; ++k) {
        const int i = k % N;
        if (o->steps[i] == ACID8_SLIDE) { if (++slide_run > max_run) max_run = slide_run; }
        else slide_run = 0;
    }
    CHECK(max_run <= 2);                                            /* no endless legato */
    for (int i = 0; i < N; ++i) {
        CHECK(o->steps[i] <= ACID8_SLIDE);
        if (o->steps[i] == ACID8_REST) continue;
        CHECK(o->degrees[i] < in->scale_len);                       /* only the scale */
        CHECK(o->octaves[i] < in->octaves);                         /* only the octaves allowed */
        ++st.hits;
        st.accents += o->steps[i] == ACID8_ACCENT;
        st.slides += o->steps[i] == ACID8_SLIDE;
        st.roots += in->scale[o->degrees[i]] == 0;
        if (o->octaves[i] > st.top_oct) st.top_oct = o->octaves[i];
        if (o->steps[i] == ACID8_SLIDE) CHECK(o->steps[(i + 1) % N] != ACID8_REST);   /* never into a rest */
        const int p = acid8_pitch(in, o, i);
        if (prev >= 0) {
            CHECK(iabs(p - prev) <= 12);                            /* no wild leaps */
            st.semis += iabs(p - prev) == 1;
        }
        prev = p;
    }
    CHECK(st.hits >= 2 && st.hits <= 8);
    if (st.hits == 8) CHECK(o->style == ACID8_ROLLING);
    CHECK(st.accents < st.hits && st.slides < st.hits);             /* not every note */
    if (in->accent == 0.0f) CHECK(st.accents == 0);
    if (in->slide == 0.0f) CHECK(st.slides == 0);
    if (o->style == ACID8_MINIMAL) CHECK(st.roots * 5 >= st.hits * 4);   /* 80 %+ root */
    return st;
}

static acid8_in make_in(int scale, int octaves, float d, float a, float s) {
    acid8_in in = {SC[scale], SC_LEN[scale], octaves, d, a, s};
    return in;
}

int main(void) {
    /* --- every rule, over random settings --------------------------------- */
    int style_count[ACID8_STYLES] = {0}, boring = 0;
    const int TRIALS = 40000;
    for (int t = 0; t < TRIALS; ++t) {
        const int scale = (int)(rnd() % 6);
        acid8_in in = make_in(scale, 1 + (int)(rnd() % 3), rndf(), rndf(), rndf());
        if (t % 9 == 0) in.accent = 0.0f;
        if (t % 11 == 0) in.slide = 0.0f;
        if (t % 13 == 0) in.density = (t & 1) ? 0.0f : 1.0f;
        acid8_out o;
        acid8_generate(&in, rnd(), t % 7 == 0 ? (int)(rnd() % ACID8_STYLES) : -1, &o);
        check_one(&in, &o);
        if (t % 7) style_count[o.style]++;
        int same = 1;
        for (int i = 1; i < N; ++i)
            if (o.steps[i] != o.steps[0] || acid8_pitch(&in, &o, i) != acid8_pitch(&in, &o, 0)) same = 0;
        boring += same;
    }
    for (int s = 0; s < ACID8_STYLES; ++s) {
        printf("style %d: %d\n", s, style_count[s]);
        CHECK(style_count[s] > TRIALS / 10 && style_count[s] < TRIALS * 3 / 10);   /* all five, none dominant */
    }
    CHECK(boring == 0);                                             /* never one note the same way 8 times */

    /* --- one seed, one pattern ---------------------------------------------- */
    {
        acid8_in in = make_in(PHRYGIAN, 2, 0.7f, 0.4f, 0.25f);
        acid8_out a, b;
        acid8_generate(&in, 12345, -1, &a);
        acid8_generate(&in, 12345, -1, &b);
        CHECK(!memcmp(&a, &b, sizeof a));
        int distinct = 0;
        static acid8_out seen[300];
        for (int k = 0; k < 300; ++k) {
            acid8_generate(&in, 1000u + (unsigned)k * 7919u, -1, &seen[k]);
            int dup = 0;
            for (int j = 0; j < k && !dup; ++j)
                dup = !memcmp(seen[j].steps, seen[k].steps, N) && !memcmp(seen[j].degrees, seen[k].degrees, N) &&
                      !memcmp(seen[j].octaves, seen[k].octaves, N);
            distinct += !dup;
        }
        printf("distinct patterns from 300 NEWs: %d\n", distinct);
        CHECK(distinct > 270);
    }

    /* --- the settings move what they should ----------------------------------- */
    {
        const float levels[4] = {0.0f, 0.3f, 0.7f, 1.0f};
        double hits[4] = {0}, acc[4] = {0}, sl[4] = {0};
        const int M = 3000;
        for (int l = 0; l < 4; ++l)
            for (int k = 0; k < M; ++k) {
                acid8_out o;
                acid8_in in = make_in(MINOR, 2, levels[l], 0.4f, 0.25f);
                acid8_generate(&in, 777u + (unsigned)k * 31u, -1, &o);
                hits[l] += check_one(&in, &o).hits;
                in = make_in(MINOR, 2, 0.7f, levels[l], 0.25f);
                acid8_generate(&in, 999u + (unsigned)k * 31u, -1, &o);
                acc[l] += check_one(&in, &o).accents;
                in = make_in(MINOR, 2, 0.7f, 0.4f, levels[l]);
                acid8_generate(&in, 555u + (unsigned)k * 31u, -1, &o);
                sl[l] += check_one(&in, &o).slides;
            }
        printf("mean notes   by Density 0/.3/.7/1: %.2f %.2f %.2f %.2f\n", hits[0] / M, hits[1] / M, hits[2] / M, hits[3] / M);
        printf("mean accents by Accent  0/.3/.7/1: %.2f %.2f %.2f %.2f\n", acc[0] / M, acc[1] / M, acc[2] / M, acc[3] / M);
        printf("mean slides  by Slide   0/.3/.7/1: %.2f %.2f %.2f %.2f\n", sl[0] / M, sl[1] / M, sl[2] / M, sl[3] / M);
        for (int l = 1; l < 4; ++l) {
            CHECK(hits[l] > hits[l - 1] + 0.2 * M);
            CHECK(acc[l] > acc[l - 1] + 0.2 * M);
            /* Slides are capped at half the notes ("never every note"), so the
             * top of the knob may only hold what .7 already reached. */
            CHECK(l == 3 ? sl[l] >= sl[l - 1] : sl[l] > sl[l - 1] + 0.2 * M);
        }
        CHECK(acc[0] == 0 && sl[0] == 0);
    }

    /* --- Octaves: 1 keeps every note in one octave; 3 reaches the third --------- */
    {
        int top[4] = {0};
        for (int oc = 1; oc <= 3; ++oc)
            for (int k = 0; k < 2000; ++k) {
                acid8_in in = make_in(MINOR, oc, 0.8f, 0.4f, 0.3f);
                acid8_out o;
                acid8_generate(&in, 4242u + (unsigned)k * 13u, -1, &o);
                const stats_t s = check_one(&in, &o);
                if (s.top_oct > top[oc]) top[oc] = s.top_oct;
            }
        CHECK(top[1] == 0 && top[2] == 1 && top[3] >= 1);
    }

    /* --- the styles keep their character ---------------------------------------- */
    {
        int with_semis = 0, minimal_root = 0, minimal_n = 0, rolling_dense = 0, broken_sparse = 0;
        const int M = 2000;
        for (int k = 0; k < M; ++k) {
            acid8_out o;
            acid8_in in = make_in(PHRYGIAN, 2, 0.7f, 0.4f, 0.25f);   /* has the semitone above the root */
            acid8_generate(&in, 31337u + (unsigned)k * 17u, ACID8_INDUSTRIAL, &o);
            with_semis += check_one(&in, &o).semis > 0;
            acid8_generate(&in, 31337u + (unsigned)k * 17u, ACID8_MINIMAL, &o);
            const stats_t m = check_one(&in, &o);
            minimal_root += m.roots;
            minimal_n += m.hits;
            acid8_generate(&in, 31337u + (unsigned)k * 17u, ACID8_ROLLING, &o);
            stats_t a = check_one(&in, &o);
            acid8_generate(&in, 31337u + (unsigned)k * 17u, ACID8_BROKEN, &o);
            stats_t b = check_one(&in, &o);
            rolling_dense += a.hits > b.hits;
            broken_sparse += b.hits <= 5;
        }
        printf("INDUSTRIAL with semitone moves (Phrygian): %d%%; MINIMAL on the root: %d%%; "
               "ROLLING denser than BROKEN: %d%%\n",
               100 * with_semis / M, 100 * minimal_root / minimal_n, 100 * rolling_dense / M);
        CHECK(with_semis > M * 7 / 10);
        CHECK(minimal_root * 100 >= minimal_n * 85);
        CHECK(rolling_dense > M * 8 / 10 && broken_sparse > M * 6 / 10);
        /* A scale without a semitone next to the root is never given one. */
        for (int k = 0; k < M; ++k) {
            acid8_in in = make_in(MINPENT, 2, 0.7f, 0.4f, 0.25f);
            acid8_out o;
            acid8_generate(&in, 777u + (unsigned)k, ACID8_INDUSTRIAL, &o);
            CHECK(check_one(&in, &o).semis == 0);
        }
    }
    printf("PASS: acid8 (%d random settings, every rule)\n", TRIALS);
    return 0;
}
