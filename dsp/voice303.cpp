/* o303 - the Schwung 303 synth, embedded. See voice303.h.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * plugin.cpp is included, not copied: every parameter goes through its own
 * set_param, every note through its own on_midi. Its entry point is renamed
 * so this module exports its own.
 *
 * Its engine is Open303Sync rather than Open303 (open303_sync.h: the same
 * engine plus a second VCO hard-synced to the first). plugin.cpp names the
 * type exactly once -- `rosic::Open303 engine;` in its instance struct -- and
 * the macro below changes that one name while it is compiled; the header
 * itself is already included, so nothing else in it is touched.
 */
#include "open303_sync.h"
#define move_plugin_init_v2 p303_upstream_init
#define Open303 Open303Sync
#include "schwung-303/plugin.cpp"
#undef Open303
#undef move_plugin_init_v2
static_assert(sizeof(((P303 *)nullptr)->engine) == sizeof(rosic::Open303Sync), "plugin.cpp must hold the sync engine");

#include "voice303.h"

/* The handle IS the upstream instance. */
static inline P303 *p303(voice303 *v) { return reinterpret_cast<P303 *>(v); }

extern "C" {

voice303 *voice303_create(void) {
    return reinterpret_cast<voice303 *>(g_api_v2.create_instance("", nullptr));
}

void voice303_destroy(voice303 *v) {
    if (v) g_api_v2.destroy_instance(v);
}

void voice303_midi(voice303 *v, const uint8_t *msg) {
    if (v && msg) g_api_v2.on_midi(v, msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
}

/* The second VCO's keys are this module's; everything else is plugin.cpp's. */
void voice303_set(voice303 *v, const char *key, const char *val) {
    if (!v || !key || !val) return;
    rosic::Open303Sync &e = p303(v)->engine;
    const double x = std::atof(val);
    if (!std::strcmp(key, "vco1_level")) e.setVco1Level(x);
    else if (!std::strcmp(key, "vco2_level")) e.setVco2Level(x);
    else if (!std::strcmp(key, "vco2_wave")) e.setVco2Wave(std::atoi(val));
    else if (!std::strcmp(key, "vco2_tune")) e.setVco2Tune(x);     /* semitones, coarse + fine */
    else if (!std::strcmp(key, "vco2_sync")) e.setVco2Sync(std::atoi(val));
    else if (!std::strcmp(key, "sync_env")) e.setSyncEnv(x);
    else g_api_v2.set_param(v, key, val);
}

int voice303_get(voice303 *v, const char *key, char *buf, int len) {
    return v ? g_api_v2.get_param(v, key, buf, len) : -1;
}

/* v2_render_block's first loop, without its drive stage and int16 clamp. */
void voice303_render(voice303 *v, float *mono, int frames) {
    P303 *p = p303(v);
    for (int i = 0; i < frames; ++i) mono[i] = static_cast<float>(p->engine.getSample());
}

} // extern "C"
