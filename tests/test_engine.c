/* o303 - is the embedded sequencer still TB-3PO?
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * Runs the stand-alone TB-3PO plugin (tests/ref_tb3po.c) and dsp/tb_engine.c
 * side by side through the same transport and the same edits, and requires
 * the MIDI TB-3PO would send a synth and the MIDI the engine hands the built-in
 * 303 to be the same bytes in the same audio blocks. That one comparison
 * covers step timing, tempo changes, Start/Stop/restart, gate length, accent
 * velocity, slide overlap, rests, octave/transpose, direction, banks and
 * the queued bank recall at the bar line.
 *
 * Then the part TB-3PO does not have: the sequencer's state as text.
 */
#include "tb3po/include/plugin_api_v1.h"
#include "tb_engine.h"
#include "acid8.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

typedef struct { long block; uint8_t b[3]; } event;
#define MAX_EVENTS 200000
static event ref_ev[MAX_EVENTS], eng_ev[MAX_EVENTS];
static int ref_n, eng_n;
static long now_block;
static float g_bpm = 120.0f;
static int g_clock = MOVE_CLOCK_STATUS_STOPPED;

static int ref_midi(const uint8_t *pkt, int len) {
    CHECK(len == 4 && ref_n < MAX_EVENTS);
    if ((pkt[1] & 0x0F) != 0) return len;      /* slot B: the tool's second sequencer */
    ref_ev[ref_n++] = (event){now_block, {(uint8_t)(pkt[1] & 0xF0), pkt[2], pkt[3]}};
    return len;
}
static int ref_clock(void) { return g_clock; }
static float ref_bpm(void) { return g_bpm; }

static void eng_midi(void *ctx, const uint8_t *msg) {
    CHECK(ctx == (void *)eng_ev && eng_n < MAX_EVENTS);
    eng_ev[eng_n++] = (event){now_block, {msg[0], msg[1], msg[2]}};
}
static float eng_bpm(void *ctx) { (void)ctx; return g_bpm; }
static int eng_clock(void *ctx) { (void)ctx; return g_clock; }

static plugin_api_v2_t *ref;
static void *ref_inst;
static tb_engine *eng;

static void both_set(const char *key, const char *val) {
    char prefixed[64];
    snprintf(prefixed, sizeof prefixed, "a.%s", key);    /* the tool addresses slot A */
    ref->set_param(ref_inst, prefixed, val);
    tb_engine_set(eng, key, val);
}

static void both_rt(uint8_t status) {
    ref->on_midi(ref_inst, &status, 1, MOVE_MIDI_SOURCE_EXTERNAL);
    tb_engine_realtime(eng, status);
}

static double next_pulse, t_samples;
static int pulses_on;

static void both_block(void) {
    if (pulses_on)
        while (next_pulse < t_samples + 128.0) {
            both_rt(0xF8);
            next_pulse += 44100.0 * 60.0 / (double)g_bpm / 24.0;
        }
    ref->render_block(ref_inst, NULL, 128);
    tb_engine_block(eng, 128);
    t_samples += 128.0;
    ++now_block;
}

static void run(double seconds) {
    for (long i = 0, n = (long)(seconds * 44100.0 / 128.0); i < n; ++i) both_block();
}

static void play(void) { g_clock = MOVE_CLOCK_STATUS_RUNNING; both_rt(0xFA); pulses_on = 1; next_pulse = t_samples; }
static void stop(void) { g_clock = MOVE_CLOCK_STATUS_STOPPED; both_rt(0xFC); pulses_on = 0; }

static void same_get(const char *key) {
    char a[512], b[512], prefixed[64];
    snprintf(prefixed, sizeof prefixed, "a.%s", key);
    const int na = ref->get_param(ref_inst, prefixed, a, sizeof a);
    const int nb = tb_engine_get(eng, key, b, sizeof b);
    if (na != nb || strcmp(a, b)) { fprintf(stderr, "FAIL get %s: '%s' vs '%s'\n", key, a, b); exit(1); }
}

static void compare(const char *what) {
    if (ref_n != eng_n) { fprintf(stderr, "FAIL %s: %d vs %d events\n", what, ref_n, eng_n); exit(1); }
    for (int i = 0; i < ref_n; ++i)
        if (ref_ev[i].block != eng_ev[i].block || memcmp(ref_ev[i].b, eng_ev[i].b, 3)) {
            fprintf(stderr, "FAIL %s: event %d: block %ld %02x %d %d vs block %ld %02x %d %d\n", what, i,
                    ref_ev[i].block, ref_ev[i].b[0], ref_ev[i].b[1], ref_ev[i].b[2],
                    eng_ev[i].block, eng_ev[i].b[0], eng_ev[i].b[1], eng_ev[i].b[2]);
            exit(1);
        }
    static const char *keys[] = {"pattern", "position", "length", "density", "accent", "slide", "octaves",
                                 "root", "scale", "gate", "transpose", "direction", "seed", "current_bank",
                                 "pending_recall", "bank_filled", "running", "sync_source", 0};
    for (int i = 0; keys[i]; ++i) same_get(keys[i]);
}

int main(void) {
    unlink(TB3PO_STATE_PATH_FOR_TEST);       /* the reference must cold-start, as the engine does */
    static const host_api_v1_t host = {.api_version = 1, .sample_rate = 44100, .frames_per_block = 128,
                                       .midi_send_internal = ref_midi, .get_clock_status = ref_clock,
                                       .get_bpm = ref_bpm};
    ref = move_plugin_init_v2(&host);
    ref_inst = ref->create_instance("", NULL);
    const tb_engine_io io = {eng_ev, eng_midi, eng_bpm, eng_clock};
    eng = tb_engine_create(&io);
    CHECK(ref_inst && eng);

    /* --- stopped: nothing plays ------------------------------------------- */
    run(0.5);
    CHECK(ref_n == 0);
    compare("stopped");

    /* --- Play, on Move's clock --------------------------------------------- */
    play();
    both_block();
    CHECK(ref_n >= 1 && ref_ev[0].block == 0 + (long)(0.5 * 44100.0 / 128.0));   /* step 1 on the first pulse */
    run(4.0);
    compare("first bars");
    const int after_first = ref_n;
    CHECK(after_first > 20);

    /* --- every edit a pad, knob or button makes, while it plays --------------- */
    static const char *edits[][2] = {
        {"set_step", "0:2"}, {"set_step", "1:3"}, {"set_step", "2:1:4:1"}, {"set_step", "3:0"},
        {"gate", "0.900"}, {"transpose", "12"}, {"direction", "1"}, {"generate", "0"},
        {"store_bank", "0"}, {"mutate", "1"}, {"store_bank", "3"}, {"direction", "2"},
        {"length", "32"}, {"density", "1.000"}, {"slide", "0.900"}, {"accent", "0.900"},
        {"generate", "0"}, {"undo", "1"}, {"direction", "3"}, {"recall_bank", "0"},
        {"root", "2"}, {"scale", "3"}, {"octaves", "3"}, {"generate", "0"}, {"store_bank", "7"},
        /* MUT at Length 8 is TB-3PO's, unchanged (only NEW is not). */
        {"length", "8"}, {"mutate", "1"}, {"mutate", "1"},
        {"direction", "0"}, {"recall_bank", "3"}, {"clear", "1"}, {"undo", "1"},
        {"transpose", "-24"}, {"gate", "0.100"}, {"recall_bank_now", "7"}, {"regen", "1"},
        {"seed", "12345"}, {"length", "24"}, {"mutate", "1"}, {"mutate", "1"},
        /* NEW at 16 and 24 (32 above) is TB-3PO's own generator, unchanged. */
        {"generate", "0"}, {"length", "16"}, {"generate", "0"}, {"store_bank", "5"},
    };
    for (unsigned i = 0; i < sizeof edits / sizeof edits[0]; ++i) {
        both_set(edits[i][0], edits[i][1]);
        g_bpm = 90.0f + 17.0f * (float)(i % 7);          /* 90..192 BPM */
        run(1.3);
        compare(edits[i][0]);
    }
    CHECK(ref_n > after_first + 300);

    /* --- Stop releases the note; Play restarts on the downbeat ------------------ */
    g_bpm = 120.0f;
    stop();
    CHECK(ref_ev[ref_n - 1].b[0] == 0x80 || ref_ev[ref_n - 1].b[0] == 0xB0);
    const int at_stop = ref_n;
    run(1.0);
    CHECK(ref_n == at_stop);
    play(); run(2.0); stop(); run(0.3);
    compare("restart");

    /* --- no MIDI clock: the internal timer at the host's tempo ------------------- */
    run(1.0);                                            /* pulse sync ages out (~0.6 s) */
    both_set("running", "1");
    g_bpm = 133.0f;
    run(3.0);
    both_set("running", "0");
    compare("internal timer");

    /* --- what crossed the wire is what a 303 needs ------------------------------- */
    int accents = 0, plains = 0, slides = 0, held = -1;
    for (int i = 0; i < eng_n; ++i) {
        const uint8_t *b = eng_ev[i].b;
        if (b[0] == 0x90) {
            CHECK(b[2] == 118 || b[2] == 72);            /* >= 100 is Open303's accent */
            b[2] == 118 ? ++accents : ++plains;
            if (held >= 0 && held != b[1]) ++slides;     /* note-on over a held note: legato */
            held = b[1];
        } else if (b[0] == 0x80 && b[1] == held) held = -1;
    }
    CHECK(accents > 50 && plains > 50 && slides > 30);

    /* --- NEW at Length 8: the acid line generator ------------------------------------ *
     * From here on the engine and the reference part ways on purpose. */
    stop(); run(0.5);
    static const int SC[6][7] = {               /* tb3po.c's SCALES */
        {0, 2, 3, 5, 7, 8, 10}, {0, 1, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 8, 11},
        {0, 3, 5, 7, 10, 0, 0}, {0, 2, 3, 5, 7, 9, 10}, {0, 2, 4, 5, 7, 9, 11},
    };
    static const int SC_LEN[6] = {7, 7, 7, 5, 7, 7};
    static const char *const s8[][2] = {
        {"length", "8"}, {"root", "3"}, {"scale", "1"}, {"octaves", "2"}, {"density", "0.700"},
        {"accent", "0.400"}, {"slide", "0.250"}, {"transpose", "0"}, {"direction", "0"},
    };
    for (unsigned i = 0; i < sizeof s8 / sizeof s8[0]; ++i) both_set(s8[i][0], s8[i][1]);
    compare("settings for 8 steps");
    static char pat_before[512], ref_pat[512], eng_pat[512], ref_seed[32], eng_seed[32], want[512];
    tb_engine_get(eng, "pattern", pat_before, sizeof pat_before);
    both_set("generate", "0");
    ref->get_param(ref_inst, "a.pattern", ref_pat, sizeof ref_pat);
    ref->get_param(ref_inst, "a.seed", ref_seed, sizeof ref_seed);
    tb_engine_get(eng, "pattern", eng_pat, sizeof eng_pat);
    tb_engine_get(eng, "seed", eng_seed, sizeof eng_seed);
    CHECK(!strcmp(ref_seed, eng_seed));          /* the seed comes from TB-3PO's own stream ... */
    CHECK(strcmp(ref_pat, eng_pat) != 0);        /* ... the pattern from the other generator */
    {
        /* Exactly acid8_generate's line, written in TB-3PO's own representation. */
        const acid8_in in = {SC[1], SC_LEN[1], 2, 0.7f, 0.4f, 0.25f};
        acid8_out o;
        acid8_generate(&in, (uint32_t)strtoul(eng_seed, NULL, 10), -1, &o);
        int k = snprintf(want, sizeof want, "8|");
        for (int i = 0; i < 8; ++i) k += snprintf(want + k, sizeof want - k, "%s%d", i ? "," : "", o.steps[i]);
        k += snprintf(want + k, sizeof want - k, "|");
        for (int i = 0; i < 8; ++i) k += snprintf(want + k, sizeof want - k, "%s%d", i ? "," : "", o.degrees[i]);
        k += snprintf(want + k, sizeof want - k, "|");
        for (int i = 0; i < 8; ++i) k += snprintf(want + k, sizeof want - k, "%s%d", i ? "," : "", o.octaves[i]);
        CHECK(!strcmp(eng_pat, want));
        CHECK(tb_engine_generate8_style(eng) == o.style);
    }
    tb_engine_set(eng, "undo", "1");             /* Undo brings back the pattern before NEW */
    tb_engine_get(eng, "pattern", eng_pat, sizeof eng_pat);
    CHECK(!strcmp(eng_pat, pat_before));
    tb_engine_set(eng, "generate", "0");

    /* Playing it: every note is in D# Phrygian, step 1 is the root, NEW on the
     * fly never strands a note. */
    {
        const int from = eng_n;
        int styles[ACID8_STYLES] = {0};
        play();
        for (int k = 0; k < 60; ++k) {
            if (k % 3 == 0) {
                tb_engine_set(eng, "generate", "0");
                styles[tb_engine_generate8_style(eng)]++;
            }
            run(0.27);                           /* half a bar at 120 */
        }
        stop(); run(1.0);
        int open[128] = {0}, ons = 0, first_seen = 0, same_note_slides = 0;
        for (int i = from; i < eng_n; ++i) {
            const uint8_t *b = eng_ev[i].b;
            if (b[0] == 0x90) {
                ++ons;
                CHECK(b[2] == 118 || b[2] == 72);
                int ok = 0;
                for (int d = 0; d < 7; ++d)
                    for (int o = 0; o < 2; ++o) ok |= b[1] == 24 + 3 + SC[1][d] + 12 * o;
                CHECK(ok);
                if (!first_seen) { CHECK((b[1] - 27) % 12 == 0); first_seen = 1; }   /* Play: step 1, the root */
                same_note_slides += open[b[1]] > 0;     /* a note-on over itself: a same-note slide */
                open[b[1]] = 1;
            } else if (b[0] == 0x80) {
                open[b[1]] = 0;                          /* Open303: one note-off releases the key */
            }
        }
        for (int nn = 0; nn < 128; ++nn) CHECK(open[nn] == 0);      /* nothing left hanging */
        int kinds = 0;
        for (int s = 0; s < ACID8_STYLES; ++s) kinds += styles[s] > 0;
        printf("8-step NEW while playing: %d note-ons (%d same-note slides), %d of 5 styles heard\n",
               ons, same_note_slides, kinds);
        CHECK(ons > 40 && kinds >= 3);
    }

    /* Eight banks, eight different 8-step lines; a 16-step bank beside them. */
    {
        static char saved[8][512], got8[512];
        for (int bnk = 0; bnk < 8; ++bnk) {
            if (bnk == 7) { tb_engine_set(eng, "length", "16"); }
            tb_engine_set(eng, "generate", "0");
            tb_engine_get(eng, "pattern", saved[bnk], sizeof saved[bnk]);
            char idx[16]; snprintf(idx, sizeof idx, "%d", bnk);
            tb_engine_set(eng, "store_bank", idx);
            for (int j = 0; j < bnk; ++j) CHECK(strcmp(saved[j], saved[bnk]) != 0);
        }
        CHECK(!strncmp(saved[0], "8|", 2) && !strncmp(saved[7], "16|", 3));
        tb_engine_set(eng, "length", "8");
        for (int k = 0; k < 3; ++k) tb_engine_set(eng, "generate", "0");    /* something else on top */
        for (int bnk = 7; bnk >= 0; --bnk) {
            char idx[16]; snprintf(idx, sizeof idx, "%d", bnk);
            tb_engine_set(eng, "recall_bank_now", idx);
            tb_engine_get(eng, "pattern", got8, sizeof got8);
            CHECK(!strcmp(got8, saved[bnk]));     /* length included */
        }
        /* Through the state text and into another engine: the same eight. */
        static char st_a[2048], st_b[2048];
        CHECK(tb_engine_state_write(eng, st_a, sizeof st_a) > 0);
        tb_engine *copy = tb_engine_create(&(tb_engine_io){0});
        CHECK(copy && tb_engine_state_read(copy, st_a) == 1);
        CHECK(tb_engine_state_write(copy, st_b, sizeof st_b) > 0 && !strcmp(st_a, st_b));
        for (int bnk = 0; bnk < 8; ++bnk) {
            char idx[16]; snprintf(idx, sizeof idx, "%d", bnk);
            tb_engine_set(copy, "recall_bank_now", idx);
            tb_engine_get(copy, "pattern", got8, sizeof got8);
            CHECK(!strcmp(got8, saved[bnk]));
        }
        tb_engine_destroy(copy);
        tb_engine_set(eng, "recall_bank_now", "0");
    }

    /* --- state: text, and transactional ---------------------------------------------- */
    static char a[2048], b[2048], bad[2048];
    const int n = tb_engine_state_write(eng, a, sizeof a);
    CHECK(n > 100 && n < 1024 && (int)strlen(a) == n);
    for (int i = 0; i < n; ++i) CHECK(a[i] != '"' && a[i] != '\\' && a[i] >= ' ');   /* safe inside JSON */
    CHECK(tb_engine_state_write(eng, b, 40) == -1);
    tb_engine *twin = tb_engine_create(&(tb_engine_io){0});
    CHECK(twin && tb_engine_state_read(twin, a) == 1);
    CHECK(tb_engine_state_write(twin, b, sizeof b) == n && !strcmp(a, b));
    static char va[2048], vb[2048];
    tb_engine_get(eng, "pattern", va, sizeof va); tb_engine_get(twin, "pattern", vb, sizeof vb);
    CHECK(!strcmp(va, vb));
    CHECK(tb_engine_view(twin, vb, sizeof vb) > 0 && strstr(vb, ",8,"));      /* its length came with it */
    /* Every single-character corruption is either refused (nothing changes)
     * or accepted as a valid state -- never a crash, never a half-applied one. */
    unsigned seed = 1;
    for (int trial = 0; trial < 20000; ++trial) {
        memcpy(bad, a, (size_t)n + 1);
        seed = seed * 1664525u + 1013904223u;
        const int at = (int)((seed >> 8) % (unsigned)n);
        static const char junk[] = "0123456789abcdefg,:-. x";
        bad[at] = junk[(seed >> 20) % (sizeof junk - 1)];
        if (trial % 7 == 0) bad[at] = 0;                 /* truncation */
        CHECK(tb_engine_state_write(twin, va, sizeof va) > 0);
        const int ok = tb_engine_state_read(twin, bad);
        CHECK(tb_engine_state_write(twin, vb, sizeof vb) > 0);
        if (!ok) CHECK(!strcmp(va, vb));
        else {
            tb_engine *third = tb_engine_create(&(tb_engine_io){0});
            if (!third || tb_engine_state_read(third, vb) != 1) { fprintf(stderr, "bad: %s\nout: %s\n", bad, vb); CHECK(0); }
            tb_engine_destroy(third);
        }
        tb_engine_block(twin, 128);
    }
    CHECK(!tb_engine_state_read(twin, "") && !tb_engine_state_read(twin, "2,16"));
    tb_engine_destroy(twin);

    /* --- the pool: bounded, reusable ---------------------------------------------- */
    tb_engine *more[8];
    int got = 0;
    for (int i = 0; i < 8; ++i) got += (more[i] = tb_engine_create(&(tb_engine_io){0})) != NULL;
    CHECK(got == 5);
    for (int i = 0; i < 8; ++i) tb_engine_destroy(more[i]);

    /* Destroy releases a sounding note. */
    play(); run(0.2);
    const int before = eng_n;
    tb_engine_destroy(eng);
    CHECK(eng_n == before || eng_ev[eng_n - 1].b[0] == 0x80 || eng_ev[eng_n - 1].b[0] == 0xB0);
    ref->destroy_instance(ref_inst);
    printf("PASS: engine == stand-alone TB-3PO over %d MIDI events (%d accents, %d slides)\n",
           ref_n, accents, slides);
    return 0;
}
