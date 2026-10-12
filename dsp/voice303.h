/* o303 - the Schwung 303 synth (Open303), as a voice.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * vendor/schwung-303/plugin.cpp is the catalog "303" module's DSP, compiled in
 * UNMODIFIED: the same Open303 engine, the same knob-to-engine mappings
 * (cutoff 314..2394 Hz, the Devilfish ranges, ...) and the same MIDI rules --
 * velocity >= 100 is an accent, a note-on while another note is held slides.
 * Those rules are what TB-3PO was written against, which is why its MIDI can
 * be fed straight in.
 *
 * Two things differ: the 303 module renders int16 through its own drive
 * stage, where here the engine's float mono is handed on to the RAT; and the
 * engine carries a second, hard-synced VCO (open303_sync.h), silent unless
 * its level is raised.
 */
#ifndef VOICE303_H
#define VOICE303_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct voice303 voice303;

voice303 *voice303_create(void);
void voice303_destroy(voice303 *v);
/* A 3-byte channel message: note on/off, or CC 123 (all notes off). */
void voice303_midi(voice303 *v, const uint8_t *msg);
/* The 303 module's own keys: cutoff, resonance, env_mod, decay, accent,
 * volume, waveform, tuning, devil_mod_switch, normal_decay, ...
 * and the second VCO's (open303_sync.h): vco1_level, vco2_level (0..1),
 * vco2_wave (0 saw, 1 square), vco2_tune (semitones), vco2_sync (0/1),
 * sync_env (0..1). */
void voice303_set(voice303 *v, const char *key, const char *val);
int voice303_get(voice303 *v, const char *key, char *buf, int len);
void voice303_render(voice303 *v, float *mono, int frames);

#ifdef __cplusplus
}
#endif
#endif
