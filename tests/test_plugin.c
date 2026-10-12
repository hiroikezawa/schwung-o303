/* o303 - the whole dsp.so, built with ASan/UBSan, driven through the
 * v2 ABI with random parameters, pad verbs, transport and state documents.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * Nothing here checks what it SOUNDS like (tests/smoke.py does); it checks
 * that no input sequence reads or writes out of bounds, and that the output
 * stays a finite, bounded signal. */
#include "host/plugin_api_v1.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static unsigned rng = 0x3b03u;
static unsigned rnd(unsigned n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }
static int g_clock = MOVE_CLOCK_STATUS_STOPPED;
static float g_bpm = 120.0f;
static int host_clock(void) { return g_clock; }
static float host_bpm(void) { return g_bpm; }

static const char *const keys[] = {
    "volume", "cutoff", "resonance", "env_mod", "decay", "acc_level", "rat_dist", "rat_filter",
    "density", "accent", "slide", "gate", "root", "scale", "length", "octaves", "direction",
    "transpose", "current_bank", "waveform", "tuning", "osc_level", "rat_on", "rat_model",
    "rat_level", "rat_low_cut", "rat_hi_cut", "devil_mod_switch", "normal_decay", "accent_decay",
    "feedback_hpf", "soft_attack", "slide_time", "tanh_shaper_drive",
    "vco2_level", "vco2_tune", "vco2_fine", "vco2_wave", "vco2_sync", "sync_env", "vco1_level",
    "set_step", "store_bank", "recall_bank", "recall_bank_now", "generate", "mutate", "clear", "undo",
    "state", "seq_view", "chain_params", "grid_hierarchy", "ui_hierarchy", "name", "nonsense",
};
#define NKEYS ((int)(sizeof keys / sizeof keys[0]))
static const char *const values[] = {
    "0", "1", "0.5", "0.999", "-1", "7", "31", "32", "33", "1e9", "-1e9", "nan", "inf", "", " ", "x",
    "16", "24", "Phrygian", "+4 oct", "-4 oct", "RAT2", "On", "Square", "0:3:6:2", "31:2", "32:1",
    "-24", "24", "-50", "50", "-1:1", "99:99:99:99", "5:1:200:9", "4:7", "5:", ":", "{", "{}", "{\"v\":1}", "{\"v\":1,\"seq\":\"1,16\"}",
    "{\"v\":1,\"volume\":0.5,\"seq\":\"\"}", "{\"v\":1,\"volume\":2}",
};
#define NVALUES ((int)(sizeof values / sizeof values[0]))

int main(int argc, char **argv) {
    CHECK(argc == 2);
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    plugin_api_v2_t *(*init)(const host_api_v1_t *) = (plugin_api_v2_t *(*)(const host_api_v1_t *))dlsym(lib, "move_plugin_init_v2");
    CHECK(init);
    static host_api_v1_t host;                 /* zeroed, reserved tail included */
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    host.get_clock_status = host_clock;
    host.get_bpm = host_bpm;
    plugin_api_v2_t *api = init(&host);
    CHECK(api && api->api_version == 2);

    void *inst[4] = {0};
    for (int i = 0; i < 4; ++i) CHECK((inst[i] = api->create_instance(".", NULL)));
    static int16_t out[MOVE_FRAMES_PER_BLOCK * 2];
    static char buf[70000], saved[4][4096];
    long pinned = 0, total = 0;
    double pulse = 0.0, t = 0.0;
    for (long block = 0; block < 60000; ++block) {
        void *p = inst[rnd(4)];
        switch (rnd(12)) {
        case 0: case 1: case 2:
            api->set_param(p, keys[rnd(NKEYS)], values[rnd(NVALUES)]);
            break;
        case 3: {
            const int n = api->get_param(p, keys[rnd(NKEYS)], buf, (int)sizeof buf);
            CHECK(n < (int)sizeof buf && (n < 0 || (int)strlen(buf) == n));
            break;
        }
        case 4: {                              /* a short buffer must be refused, not overrun */
            char small[8];
            const int n = api->get_param(p, keys[rnd(NKEYS)], small, (int)sizeof small);
            CHECK(n < (int)sizeof small);
            break;
        }
        case 5: {                              /* save here, load there */
            const int a = (int)rnd(4);
            CHECK(api->get_param(inst[a], "state", saved[a], (int)sizeof saved[a]) > 0);
            api->set_param(p, "state", saved[a]);
            if (rnd(3) == 0) {                 /* and a damaged copy */
                saved[a][rnd((unsigned)strlen(saved[a]))] = "0,:\"}x"[rnd(6)];
                api->set_param(p, "state", saved[a]);
            }
            break;
        }
        case 6:
            if (rnd(40) == 0) { g_clock = MOVE_CLOCK_STATUS_RUNNING; pulse = t; uint8_t m = 0xFA; for (int i = 0; i < 4; ++i) api->on_midi(inst[i], &m, 1, 2); }
            if (rnd(60) == 0) { g_clock = MOVE_CLOCK_STATUS_STOPPED; uint8_t m = (uint8_t)(rnd(2) ? 0xFC : 0xFB); for (int i = 0; i < 4; ++i) api->on_midi(inst[i], &m, 1, 2); }
            if (rnd(50) == 0) g_bpm = 20.0f + (float)rnd(380);
            break;
        case 7: {                              /* stray notes and CCs from Move's track */
            const uint8_t m[3] = {(uint8_t)(0x80 + 16 * rnd(4)), (uint8_t)rnd(128), (uint8_t)rnd(128)};
            api->on_midi(p, m, 3, 0);
            break;
        }
        case 8:
            if (rnd(200) == 0) {               /* a module swap in flight */
                const int a = (int)rnd(4);
                api->destroy_instance(inst[a]);
                CHECK((inst[a] = api->create_instance(".", NULL)));
            }
            break;
        default: break;
        }
        if (g_clock == MOVE_CLOCK_STATUS_RUNNING)
            while (pulse < t + MOVE_FRAMES_PER_BLOCK) {
                const uint8_t m = 0xF8;
                for (int i = 0; i < 4; ++i) api->on_midi(inst[i], &m, 1, 2);
                pulse += 44100.0 * 60.0 / (double)g_bpm / 24.0;
            }
        t += MOVE_FRAMES_PER_BLOCK;
        for (int i = 0; i < 4; ++i) {
            api->render_block(inst[i], out, MOVE_FRAMES_PER_BLOCK);
            for (int k = 0; k < MOVE_FRAMES_PER_BLOCK; ++k) {
                CHECK(out[2 * k] == out[2 * k + 1]);
                pinned += out[2 * k] == 32767 || out[2 * k] <= -32767;
                ++total;
            }
        }
    }
    CHECK(pinned * 200 < total);               /* the limiter rounds; it does not sit on the rail */
    for (int i = 0; i < 4; ++i) api->destroy_instance(inst[i]);
    api->destroy_instance(NULL);
    api->render_block(NULL, out, MOVE_FRAMES_PER_BLOCK);
    printf("PASS: plugin under ASan/UBSan, %ld frames, %.3f%% at the rail\n", total, 100.0 * (double)pinned / (double)total);
    return 0;
}
