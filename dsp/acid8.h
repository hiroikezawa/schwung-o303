/* o303 - the 8-step acid line generator behind NEW at Length 8.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * TB-3PO's own generator (vendor/tb3po/tb3po.c generate_pattern) rolls each
 * step on its own: fine over 16-32 steps, where the dice average out, and
 * thin over 8, where one loop is heard a hundred times a minute. This one
 * writes a short loop the way an acid line is played:
 *
 *   1. a STYLE is picked at random (hidden -- nothing to choose):
 *        HYPNOTIC    the root, repeated; accents make the rhythm
 *        INDUSTRIAL  the root with semitone neighbours, hard accents
 *        ROLLING     dense; root, fifth and octave, more slides
 *        MINIMAL     one note (80 %+ root), rests and accents, octave hops
 *        BROKEN      sparse; semitone / tritone colour, offbeat accents
 *   2. the RHYTHM first: one of a set of 8-step skeletons (X-XX-X-X ...),
 *      varied by a shifted hit, then grown or thinned toward the hit count
 *      Density asks for -- never all rests, never wall to wall by accident,
 *      always a note on step 1;
 *   3. the PITCHES, from intervals that suit the style, resolved against
 *      Root/Scale/Octaves -- an interval the scale does not have is never
 *      produced, and leaps are kept small, the last note leading home;
 *   4. ACCENTS on the beats the style favours, how many from Accent;
 *   5. SLIDES between sounding steps (the loop's end into step 1 included),
 *      semitone and same-note moves first, how many from Slide;
 *   6. eight candidates are made this way and the one that scores best as
 *      a loop is kept.
 *
 * The output is TB-3PO's own step data -- kind (rest / note / accent /
 * slide), scale degree, octave -- so it plays, saves to a bank, recalls and
 * travels in the slot state exactly like a pattern TB-3PO made itself.
 * Deterministic: one seed, one pattern. No allocation, no globals.
 */
#ifndef ACID8_H
#define ACID8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACID8_STEPS 8

enum { ACID8_REST = 0, ACID8_NOTE = 1, ACID8_ACCENT = 2, ACID8_SLIDE = 3 };
enum { ACID8_HYPNOTIC, ACID8_INDUSTRIAL, ACID8_ROLLING, ACID8_MINIMAL, ACID8_BROKEN, ACID8_STYLES };

typedef struct {
    const int *scale;        /* semitones above the root of each degree; scale[0] == 0 */
    int scale_len;           /* degrees in the scale (TB-3PO: 5 or 7) */
    int octaves;             /* TB-3PO's Octaves: octave indices 0 .. octaves-1 */
    float density, accent, slide;   /* TB-3PO's probabilities, 0..1 */
} acid8_in;

typedef struct {
    uint8_t steps[ACID8_STEPS];     /* ACID8_REST / NOTE / ACCENT / SLIDE */
    uint8_t degrees[ACID8_STEPS];   /* index into scale */
    uint8_t octaves[ACID8_STEPS];   /* 0 .. octaves-1 */
    int style;                      /* ACID8_* -- for the tests; never shown */
    int target_hits;                /* what Density asked for */
} acid8_out;

/* Writes one pattern. `style` < 0 picks one from the seed (what NEW does);
 * 0..ACID8_STYLES-1 forces it (the tests). */
void acid8_generate(const acid8_in *in, uint32_t seed, int style, acid8_out *out);

/* The pitch of a step in semitones above the root of octave 0. */
int acid8_pitch(const acid8_in *in, const acid8_out *p, int step);

#ifdef __cplusplus
}
#endif
#endif
