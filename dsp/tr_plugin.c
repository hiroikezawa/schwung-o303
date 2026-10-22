/* o303 - Schwung plugin API v2 adapter: one sound generator made of
 * three existing ones.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 *   TB-3PO sequencer  ->  Open303 (+ VCO2, hard-synced)  ->  RAT  ->  Volume  ->  out
 *   (tb_engine)           (voice303 / open303_sync.h)         (rat_stage)
 *
 * The sequencer's MIDI never leaves this file: seq_midi() below is the whole
 * connection, and what crosses it is exactly what TB-3PO sends a 303 in the
 * next chain slot -- note-on velocity 118 for an accent and 72 for a plain
 * step, a slide as the next note-on BEFORE the previous note-off (which the
 * 303 hears as legato, and glides), a rest as a note-off. Octave, transpose
 * and scale are already in the note number.
 *
 * EVERY entry point here runs on the SPI audio callback (plugin_api_v1.h):
 * instances come from static pools, nothing logs or touches a file. The one
 * exception is inherited: Open303 is C++ and allocates when it is constructed
 * and in its held-note list, exactly as the 303 module does.
 *
 * Incoming notes are ignored: the pads belong to the sequencer while its
 * screen is up and to Move otherwise, and a second note source into a
 * monophonic slide voice would fight the pattern.
 */
#include "host/plugin_api_v1.h"
#include "tb_engine.h"
#include "voice303.h"
#include "rat_stage.h"
#include "tr_metadata.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "instance pool needs lock-free atomics");

#define TR_INSTANCES 6
#define TR_SR 44100.0f

/* The shim parks a slot whose output stays below 5 LSB for ~1 s and then
 * renders it only every ~0.5 s (schwung_shim.c, DSP_IDLE_THRESHOLD). The
 * sequencer is clocked through on_midi, which does not wake a parked slot, so
 * the first notes after Play would be lost. A block that would be silent
 * carries a 6 LSB DC offset instead (-75 dBFS) -- except at VOLUME 0, which is
 * exact digital silence. */
#define TR_KEEPALIVE_LSB 6
#define TR_SILENCE_LSB 4

/* ---- parameters ----------------------------------------------------------- */
typedef enum { D_OWN, D_SEQ, D_303, D_RAT } tr_dest;
typedef enum { K_FLOAT, K_INT, K_ENUM, K_TEXT } tr_kind;   /* K_TEXT: an enum whose wire value is its option text */

static const char *const o_root[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", 0};
static const char *const o_scale[] = {"Minor", "Phrygian", "HarmMinor", "MinPent", "Dorian", "Major", 0};
static const char *const o_length[] = {"8", "16", "24", "32", 0};
static const char *const o_dir[] = {"Fwd", "Rev", "Ping", "Rnd", 0};
static const char *const o_transpose[] = {"-4 oct", "-3 oct", "-2 oct", "-1 oct", "0 oct",
                                          "+1 oct", "+2 oct", "+3 oct", "+4 oct", 0};
static const char *const o_wave[] = {"Saw", "Square", 0};
static const char *const o_onoff[] = {"Off", "On", 0};
static const char *const o_model[] = {"RAT", "RAT2", 0};

enum {
    P_VOLUME, P_CUTOFF, P_RESONANCE, P_ENV_MOD, P_DECAY, P_ACC_LEVEL, P_RAT_DIST, P_RAT_FILTER,
    P_DENSITY, P_ACCENT, P_SLIDE, P_GATE, P_ROOT, P_SCALE, P_LENGTH, P_OCTAVES,
    P_DIRECTION, P_TRANSPOSE, P_BANK,
    P_WAVEFORM, P_TUNING, P_OSC_LEVEL, P_RAT_ON, P_RAT_MODEL, P_RAT_LEVEL, P_RAT_LOW_CUT, P_RAT_HI_CUT,
    P_VCO2_LEVEL, P_VCO2_TUNE, P_VCO2_FINE, P_VCO2_WAVE, P_VCO2_SYNC, P_SYNC_ENV, P_VCO1_LEVEL,
    P_DEVIL, P_NORMAL_DECAY, P_ACCENT_DECAY, P_FEEDBACK_HPF, P_SOFT_ATTACK, P_SLIDE_TIME, P_SHAPER,
    P_COUNT
};

typedef struct {
    const char *key;      /* this module's key */
    const char *up;       /* the key of the engine it belongs to (RAT: unused) */
    tr_dest dest;
    tr_kind kind;
    float lo, hi, def;
    const char *const *opts;
    int rat;              /* D_RAT: the pedal knob */
} tr_param;

/* scripts/metadata.py checks module.json's chain_params against this table
 * (order, type, range, default, option count). One row per line. */
static const tr_param tr_params[P_COUNT] = {
    {"volume", "", D_OWN, K_FLOAT, 0.0f, 1.0f, 0.75f, 0, 0},
    {"cutoff", "cutoff", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"resonance", "resonance", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"env_mod", "env_mod", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"decay", "decay", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"acc_level", "accent", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"rat_dist", "", D_RAT, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, RAT_DISTORTION},
    {"rat_filter", "", D_RAT, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, RAT_FILTER},
    {"density", "density", D_SEQ, K_FLOAT, 0.0f, 1.0f, 0.7f, 0, 0},
    {"accent", "accent", D_SEQ, K_FLOAT, 0.0f, 1.0f, 0.4f, 0, 0},
    {"slide", "slide", D_SEQ, K_FLOAT, 0.0f, 1.0f, 0.25f, 0, 0},
    {"gate", "gate", D_SEQ, K_FLOAT, 0.1f, 1.0f, 0.5f, 0, 0},
    {"root", "root", D_SEQ, K_ENUM, 0.0f, 11.0f, 9.0f, o_root, 0},
    {"scale", "scale", D_SEQ, K_ENUM, 0.0f, 5.0f, 0.0f, o_scale, 0},
    {"length", "length", D_SEQ, K_TEXT, 0.0f, 3.0f, 1.0f, o_length, 0},
    {"octaves", "octaves", D_SEQ, K_INT, 1.0f, 3.0f, 2.0f, 0, 0},
    {"direction", "direction", D_SEQ, K_ENUM, 0.0f, 3.0f, 0.0f, o_dir, 0},
    {"transpose", "transpose", D_SEQ, K_ENUM, 0.0f, 8.0f, 4.0f, o_transpose, 0},
    {"current_bank", "current_bank", D_SEQ, K_INT, 1.0f, 8.0f, 1.0f, 0, 0},
    {"waveform", "waveform", D_303, K_ENUM, 0.0f, 1.0f, 0.0f, o_wave, 0},
    {"tuning", "tuning", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"osc_level", "volume", D_303, K_FLOAT, 0.0f, 1.0f, 0.8f, 0, 0},
    {"rat_on", "", D_OWN, K_ENUM, 0.0f, 1.0f, 1.0f, o_onoff, 0},
    {"rat_model", "", D_RAT, K_ENUM, 0.0f, 1.0f, 0.0f, o_model, RAT_MODEL},
    {"rat_level", "", D_RAT, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, RAT_LEVEL},
    {"rat_low_cut", "", D_RAT, K_FLOAT, 0.0f, 1.0f, 0.0f, 0, RAT_LOW_CUT},
    {"rat_hi_cut", "", D_RAT, K_FLOAT, 0.0f, 1.0f, 0.0f, 0, RAT_HI_CUT},
    {"vco2_level", "vco2_level", D_303, K_FLOAT, 0.0f, 1.0f, 0.0f, 0, 0},
    {"vco2_tune", "vco2_tune", D_303, K_INT, -24.0f, 24.0f, 7.0f, 0, 0},
    {"vco2_fine", "vco2_tune", D_303, K_INT, -50.0f, 50.0f, 0.0f, 0, 0},
    {"vco2_wave", "vco2_wave", D_303, K_ENUM, 0.0f, 1.0f, 0.0f, o_wave, 0},
    {"vco2_sync", "vco2_sync", D_303, K_ENUM, 0.0f, 1.0f, 1.0f, o_onoff, 0},
    {"sync_env", "sync_env", D_303, K_FLOAT, 0.0f, 1.0f, 0.0f, 0, 0},
    {"vco1_level", "vco1_level", D_303, K_FLOAT, 0.0f, 1.0f, 1.0f, 0, 0},
    {"devil_mod_switch", "devil_mod_switch", D_303, K_ENUM, 0.0f, 1.0f, 0.0f, o_onoff, 0},
    {"normal_decay", "normal_decay", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"accent_decay", "accent_decay", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"feedback_hpf", "feedback_hpf", D_303, K_FLOAT, 0.0f, 1.0f, 0.5f, 0, 0},
    {"soft_attack", "soft_attack", D_303, K_FLOAT, 0.0f, 1.0f, 0.0f, 0, 0},
    {"slide_time", "slide_time", D_303, K_FLOAT, 0.0f, 1.0f, 0.3f, 0, 0},
    {"tanh_shaper_drive", "tanh_shaper_drive", D_303, K_FLOAT, 0.0f, 1.0f, 0.0f, 0, 0},
};

/* What a pad or a button does to the sequencer: TB-3PO's own verbs, passed
 * through untouched (ui_core.mjs sends what TB-3PO's ui.js sends). */
static const char *const tr_seq_verbs[] = {
    "set_step", "store_bank", "recall_bank", "recall_bank_now",
    "generate", "mutate", "clear", "undo",
    "spread", "focus",                         /* this module's own: OUT / IN (tb_engine.c) */
    0,
};

/* The knob grid is Schwung's own page engine, driven from ui_chain.js so the
 * module can take the pads. Served as `grid_hierarchy`, while `ui_hierarchy`
 * is served EMPTY on purpose: a hierarchy there makes the shadow UI draw its
 * stock grid, which runs no module code. The level
 * order is the page order ui_core.mjs relies on. */
static const char tr_hierarchy[] =
    "{\"modes\":null,\"levels\":{"
      "\"root\":{\"label\":\"Synth\","
        "\"knobs\":[\"volume\",\"cutoff\",\"resonance\",\"env_mod\",\"decay\",\"acc_level\",\"rat_dist\",\"rat_filter\"],"
        "\"params\":[{\"level\":\"pattern\",\"label\":\"Pattern\"},{\"level\":\"setup\",\"label\":\"Setup\"},"
          "{\"level\":\"tone\",\"label\":\"Tone\"},{\"level\":\"vco\",\"label\":\"VCO\"},"
          "{\"level\":\"devilfish\",\"label\":\"Devilfish\"}]},"
      "\"pattern\":{\"label\":\"Pattern\","
        "\"knobs\":[\"density\",\"accent\",\"slide\",\"gate\",\"root\",\"scale\",\"length\",\"octaves\"]},"
      "\"setup\":{\"label\":\"Setup\","
        "\"knobs\":[\"direction\",\"transpose\",\"current_bank\"]},"
      "\"tone\":{\"label\":\"Tone\","
        "\"knobs\":[\"waveform\",\"tuning\",\"osc_level\",\"rat_on\",\"rat_model\",\"rat_level\",\"rat_low_cut\",\"rat_hi_cut\"]},"
      "\"vco\":{\"label\":\"VCO\","
        "\"knobs\":[\"vco2_level\",\"vco2_tune\",\"vco2_fine\",\"vco2_wave\",\"vco2_sync\",\"sync_env\",\"vco1_level\"]},"
      "\"devilfish\":{\"label\":\"Devilfish\","
        "\"knobs\":[\"devil_mod_switch\",\"normal_decay\",\"accent_decay\",\"feedback_hpf\",\"soft_attack\",\"slide_time\",\"tanh_shaper_drive\"]}"
    "}}";

typedef struct {
    atomic_int used;
    tb_engine *seq;
    voice303 *voice;
    rat_stage *rat;
    float value[P_COUNT];     /* D_OWN / D_303 / D_RAT; the sequencer keeps its own */
    float gain;               /* VOLUME, smoothed */
    float mono[MOVE_FRAMES_PER_BLOCK];
} tr_instance;

static tr_instance g_pool[TR_INSTANCES];
static const host_api_v1_t *g_host;

/* ---- the connection: TB-3PO's MIDI out is the 303's MIDI in --------------- */
static void seq_midi(void *ctx, const uint8_t *msg) {
    voice303_midi(((tr_instance *)ctx)->voice, msg);
}

static float seq_bpm(void *ctx) {
    (void)ctx;
    return (g_host && g_host->get_bpm) ? g_host->get_bpm() : 120.0f;
}

static int seq_clock_status(void *ctx) {
    (void)ctx;
    return (g_host && g_host->get_clock_status) ? g_host->get_clock_status()
                                                : MOVE_CLOCK_STATUS_UNAVAILABLE;
}

/* ---- numbers -------------------------------------------------------------- */
static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

/* A whole-string number, rejecting NaN/Inf/trailing junk. */
static int parse_float(const char *s, float *out) {
    char *end;
    const float v = strtof(s, &end);
    if (end == s || *skip_ws(end) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

static int option_index(const char *const *opts, const char *text) {
    const char *s = skip_ws(text);
    for (int i = 0; opts && opts[i]; ++i) {
        const size_t n = strlen(opts[i]);
        if (!strncmp(s, opts[i], n) && !*skip_ws(s + n)) return i;
    }
    return -1;
}

static int param_index(const char *key) {
    for (int i = 0; i < P_COUNT; ++i) if (!strcmp(key, tr_params[i].key)) return i;
    return -1;
}

/* The value a parameter is being set to, on its own scale: a number, or for
 * an enum its option's name. 0 = not a value this parameter can take. */
static int param_value(const tr_param *p, const char *val, float *out) {
    float v;
    const int named = p->opts ? option_index(p->opts, val) : -1;
    if (p->kind == K_TEXT) {
        /* "16" is the option, not index 16; a bare index is accepted too,
         * and cannot be mistaken for one (no option reads as 0..3). */
        if (named >= 0) v = (float)named;
        else if (!parse_float(val, &v)) return 0;
    } else if (!parse_float(val, &v)) {
        if (named < 0) return 0;
        v = (float)named;
    }
    if (p->kind != K_FLOAT) v = floorf(v + 0.5f);
    if (v < p->lo) v = p->lo;
    if (v > p->hi) v = p->hi;
    *out = v;
    return 1;
}

/* ---- applying ------------------------------------------------------------- */
static void apply(tr_instance *in, int idx, float v) {
    const tr_param *p = &tr_params[idx];
    char text[24];
    switch (p->dest) {
    case D_OWN:
        in->value[idx] = v;
        break;
    case D_303:
        in->value[idx] = v;
        /* Tune and Fine are two knobs on one pitch: semitones + cents. */
        if (idx == P_VCO2_TUNE || idx == P_VCO2_FINE)
            snprintf(text, sizeof text, "%.4f", in->value[P_VCO2_TUNE] + in->value[P_VCO2_FINE] / 100.0f);
        else if (p->kind == K_FLOAT) snprintf(text, sizeof text, "%.6f", v);
        else snprintf(text, sizeof text, "%d", (int)v);
        voice303_set(in->voice, p->up, text);
        break;
    case D_RAT:
        in->value[idx] = v;
        rat_stage_set(in->rat, p->rat, v);
        break;
    case D_SEQ:
        if (idx == P_BANK) return;                       /* a readout: banks are the pads */
        if (idx == P_LENGTH) snprintf(text, sizeof text, "%s", o_length[(int)v]);
        else if (idx == P_TRANSPOSE) snprintf(text, sizeof text, "%d", ((int)v - 4) * 12);
        else if (p->kind == K_FLOAT) snprintf(text, sizeof text, "%.3f", v);
        else snprintf(text, sizeof text, "%d", (int)v);
        tb_engine_set(in->seq, p->up, text);
        break;
    }
}

/* ---- lifecycle ------------------------------------------------------------ */
static void destroy_instance(void *ptr);

static void *create_instance(const char *module_dir, const char *defaults) {
    (void)module_dir; (void)defaults;
    for (int i = 0; i < TR_INSTANCES; ++i) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&g_pool[i].used, &expected, 1)) continue;
        tr_instance *in = &g_pool[i];
        const tb_engine_io io = {in, seq_midi, seq_bpm, seq_clock_status};
        in->voice = voice303_create();
        in->rat = rat_stage_create();
        in->seq = (in->voice && in->rat) ? tb_engine_create(&io) : NULL;
        if (!in->seq) { destroy_instance(in); return NULL; }
        /* Devilfish switch first: it decides the range `decay` is mapped to. */
        apply(in, P_DEVIL, tr_params[P_DEVIL].def);
        for (int k = 0; k < P_COUNT; ++k)
            if (tr_params[k].dest != D_SEQ) apply(in, k, tr_params[k].def);
        rat_stage_snap(in->rat);
        in->gain = in->value[P_VOLUME] * in->value[P_VOLUME];
        return in;
    }
    return NULL;
}

static void destroy_instance(void *ptr) {
    tr_instance *in = ptr;
    if (!in) return;
    /* The sequencer first: it releases its held note into the voice. */
    if (in->seq) tb_engine_destroy(in->seq);
    if (in->voice) voice303_destroy(in->voice);
    if (in->rat) rat_stage_destroy(in->rat);
    in->seq = NULL; in->voice = NULL; in->rat = NULL;
    atomic_store(&in->used, 0);
}

/* ---- MIDI: Move's transport, nothing else --------------------------------- */
static void on_midi(void *ptr, const uint8_t *msg, int len, int source) {
    (void)source;
    tr_instance *in = ptr;
    if (!in || !msg || len < 1) return;
    if (msg[0] == 0xF8 || msg[0] == 0xFA || msg[0] == 0xFB || msg[0] == 0xFC)
        tb_engine_realtime(in->seq, msg[0]);
}

/* ---- state ---------------------------------------------------------------- *
 * One flat JSON object: {"v":1,"<key>":<number>,...,"seq":"<tb_engine token>"}.
 * Transactional: nothing is applied unless the whole document parses. Unknown
 * keys are ignored (a newer build may add some); a missing one keeps its
 * default. */
#define TR_STATE_VERSION 1
#define TR_SEQ_TOKEN 1024

static int write_state(const tr_instance *in, char *buf, int size) {
    int n = snprintf(buf, (size_t)size, "{\"v\":%d", TR_STATE_VERSION);
    for (int i = 0; i < P_COUNT && n > 0 && n < size; ++i) {
        const tr_param *p = &tr_params[i];
        if (p->dest == D_SEQ) continue;
        n += p->kind == K_FLOAT
            ? snprintf(buf + n, (size_t)(size - n), ",\"%s\":%.4f", p->key, in->value[i])
            : snprintf(buf + n, (size_t)(size - n), ",\"%s\":%d", p->key, (int)in->value[i]);
    }
    if (n <= 0 || n + 10 >= size) return -1;
    n += snprintf(buf + n, (size_t)(size - n), ",\"seq\":\"");
    const int m = tb_engine_state_write(in->seq, buf + n, size - n - 2);
    if (m < 0) return -1;
    n += m;
    buf[n++] = '"';
    buf[n++] = '}';
    buf[n] = 0;
    return n;
}

static int restore_state(tr_instance *in, const char *text) {
    static char seq[TR_SEQ_TOKEN];        /* off the callback's stack */
    float next[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) next[i] = tr_params[i].def;
    int version = 0, have_seq = 0;
    const char *p = skip_ws(text);
    if (*p++ != '{') return 0;
    for (int field = 0; field < 64; ++field) {
        p = skip_ws(p);
        if (*p++ != '"') return 0;
        char key[32];
        int n = 0;
        while (*p && *p != '"' && n < (int)sizeof key - 1) key[n++] = *p++;
        key[n] = 0;
        if (*p++ != '"') return 0;
        p = skip_ws(p);
        if (*p++ != ':') return 0;
        p = skip_ws(p);
        if (*p == '"') {                  /* the one string: the sequencer */
            int m = 0;
            for (++p; *p && *p != '"' && *p != '\\' && m < TR_SEQ_TOKEN - 1;) seq[m++] = *p++;
            seq[m] = 0;
            if (*p++ != '"') return 0;
            if (!strcmp(key, "seq")) have_seq = 1;
        } else {
            char *end;
            const float v = strtof(p, &end);
            if (end == p || !isfinite(v)) return 0;
            p = end;
            const int idx = param_index(key);
            if (!strcmp(key, "v")) version = (int)v;
            else if (idx >= 0 && tr_params[idx].dest != D_SEQ) {
                if (v < tr_params[idx].lo || v > tr_params[idx].hi) return 0;
                next[idx] = tr_params[idx].kind == K_FLOAT ? v : floorf(v + 0.5f);
            }
        }
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p++ != ',') return 0;
    }
    if (*p++ != '}' || *skip_ws(p) || version != TR_STATE_VERSION) return 0;
    if (have_seq && !tb_engine_state_read(in->seq, seq)) return 0;
    apply(in, P_DEVIL, next[P_DEVIL]);
    for (int i = 0; i < P_COUNT; ++i)
        if (tr_params[i].dest != D_SEQ) apply(in, i, next[i]);
    rat_stage_snap(in->rat);              /* a restored slot starts where it was */
    return 1;
}

/* ---- parameters ----------------------------------------------------------- */
static void set_param(void *ptr, const char *key, const char *val) {
    tr_instance *in = ptr;
    if (!in || !key || !val) return;
    if (!strcmp(key, "state")) { (void)restore_state(in, val); return; }
    for (int i = 0; tr_seq_verbs[i]; ++i)
        if (!strcmp(key, tr_seq_verbs[i])) { tb_engine_set(in->seq, key, val); return; }
    const int idx = param_index(key);
    float v;
    if (idx >= 0 && param_value(&tr_params[idx], val, &v)) apply(in, idx, v);
}

static int copy_text(char *buf, int size, const char *text) {
    const size_t n = strlen(text);
    if (n >= (size_t)size) { buf[0] = 0; return -1; }
    memcpy(buf, text, n + 1);
    return (int)n;
}

static int get_seq(tr_instance *in, int idx, char *buf, int size) {
    char raw[32];
    if (tb_engine_get(in->seq, tr_params[idx].up, raw, sizeof raw) < 0) return -1;
    if (idx == P_TRANSPOSE) return snprintf(buf, (size_t)size, "%d", atoi(raw) / 12 + 4);
    if (idx == P_BANK) return snprintf(buf, (size_t)size, "%d", atoi(raw) + 1);
    return snprintf(buf, (size_t)size, "%s", raw);   /* length: the step count IS the option text */
}

static int get_param(void *ptr, const char *key, char *buf, int size) {
    tr_instance *in = ptr;
    if (!in || !key || !buf || size <= 0) return -1;
    buf[0] = 0;
    int n = -1;
    const int idx = param_index(key);
    if (idx >= 0 && tr_params[idx].dest == D_SEQ) n = get_seq(in, idx, buf, size);
    else if (idx >= 0 && tr_params[idx].kind == K_FLOAT) n = snprintf(buf, (size_t)size, "%.4f", in->value[idx]);
    else if (idx >= 0) n = snprintf(buf, (size_t)size, "%d", (int)in->value[idx]);
    /* One read for the UI: "<bpm>;" + tb_engine_view(). */
    else if (!strcmp(key, "seq_view")) {
        n = snprintf(buf, (size_t)size, "%d;", (int)(seq_bpm(in) + 0.5f));
        const int m = (n > 0 && n < size) ? tb_engine_view(in->seq, buf + n, size - n) : -1;
        n = m < 0 ? -1 : n + m;
    }
    else if (!strcmp(key, "state")) return write_state(in, buf, size);
    else if (!strcmp(key, "chain_params")) return copy_text(buf, size, tr_chain_params);
    else if (!strcmp(key, "ui_hierarchy")) return 0;
    else if (!strcmp(key, "grid_hierarchy")) return copy_text(buf, size, tr_hierarchy);
    else if (!strcmp(key, "name")) return copy_text(buf, size, "o303");
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

static int get_error(void *ptr, char *buf, int size) {
    (void)ptr; (void)buf; (void)size;
    return 0;
}

/* ---- audio ---------------------------------------------------------------- */
/* Flush-to-zero for our block only: FZ is a per-thread bit and this thread is
 * Move's SPI thread, so it is restored on the way out. */
#if defined(__aarch64__)
typedef uint64_t tr_fpmode;
static inline tr_fpmode ftz_enter(void) {
    uint64_t old;
    __asm__ volatile("mrs %0, fpcr" : "=r"(old));
    __asm__ volatile("msr fpcr, %0" ::"r"(old | (1ull << 24)));
    return old;
}
static inline void ftz_leave(tr_fpmode m) { __asm__ volatile("msr fpcr, %0" ::"r"(m)); }
#elif defined(__x86_64__) || defined(__i386__)
typedef unsigned tr_fpmode;
static inline tr_fpmode ftz_enter(void) {
    const unsigned old = _mm_getcsr();
    _mm_setcsr(old | 0x8040u);
    return old;
}
static inline void ftz_leave(tr_fpmode m) { _mm_setcsr(m); }
#else
typedef int tr_fpmode;
static inline tr_fpmode ftz_enter(void) { return 0; }
static inline void ftz_leave(tr_fpmode m) { (void)m; }
#endif

/* Transparent below 0.7 of full scale, then a tanh knee that reaches 1.0 only
 * asymptotically: a hot RAT setting rounds off instead of wrapping into the
 * int16 clamp. */
static inline float soft_limit(float x) {
    const float a = fabsf(x);
    if (a <= 0.7f) return x;
    return copysignf(0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f), x);
}

static void render_block(void *ptr, int16_t *out, int frames) {
    tr_instance *in = ptr;
    if (!out || frames <= 0) return;
    if (!in || !in->seq) { memset(out, 0, (size_t)frames * 2 * sizeof *out); return; }
    if (frames > MOVE_FRAMES_PER_BLOCK) {
        memset(out + 2 * MOVE_FRAMES_PER_BLOCK, 0,
               (size_t)(frames - MOVE_FRAMES_PER_BLOCK) * 2 * sizeof *out);
        frames = MOVE_FRAMES_PER_BLOCK;
    }
    const tr_fpmode fp = ftz_enter();
    float *x = in->mono;

    tb_engine_block(in->seq, frames);                     /* sequencer -> 303 notes */
    voice303_render(in->voice, x, frames);                /* Open303 */
    for (int i = 0; i < frames; ++i) if (!isfinite(x[i])) x[i] = 0.0f;
    rat_stage_process(in->rat, x, frames, in->value[P_RAT_ON] >= 0.5f);   /* RAT */

    /* VOLUME: after the RAT, the last gain in the module. Squared (an audio
     * taper), smoothed over ~8 ms, and exactly zero at zero. */
    const float target = in->value[P_VOLUME] * in->value[P_VOLUME];
    const float coef = 1.0f - expf(-1.0f / (0.008f * TR_SR));
    float g = in->gain;
    int peak = 0;
    for (int i = 0; i < frames; ++i) {
        g += (target - g) * coef;
        if (fabsf(target - g) < 1e-6f) g = target;
        const int16_t s = (int16_t)lrintf(soft_limit(x[i] * g) * 32767.0f);
        out[2 * i] = out[2 * i + 1] = s;
        const int a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    in->gain = g;
    if (peak <= TR_SILENCE_LSB && in->value[P_VOLUME] > 0.0f)
        for (int i = 0; i < frames * 2; ++i) out[i] = TR_KEEPALIVE_LSB;
    ftz_leave(fp);
}

static plugin_api_v2_t g_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .get_error = get_error,
    .render_block = render_block,
};

__attribute__((visibility("default")))
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    /* Every coefficient assumes 44.1 kHz; refuse a host that says otherwise. */
    if (host && host->sample_rate && host->sample_rate != (int)TR_SR) return NULL;
    g_host = host;
    return &g_api;
}
