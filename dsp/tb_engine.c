/* o303 - TB-3PO's DSP plugin, embedded. See tb_engine.h.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * tb3po.c is included, not copied: this file changes what it is CONNECTED to
 * and nothing about how it sequences.
 *
 *   MIDI out      host->midi_send_internal  ->  tb_engine_io.midi
 *   tempo/clock   host->get_bpm / get_clock_status  ->  tb_engine_io
 *   entry point   move_plugin_init_v2 renamed, so the module exports its own
 *
 * and three things a tool may do that a chain synth may not, because every
 * entry point of a chain synth is the SPI audio callback:
 *
 *   heap     calloc/free  ->  a static pool
 *   thread   pthread_create (the 2 s state saver; it would inherit the
 *            callback's FIFO 70)  ->  refused, which tb3po.c already handles
 *   file     tool_state/tb3po.bin  ->  never opened. The sequencer's state
 *            travels in the slot's `state` blob instead (tb_engine_state_*),
 *            so it follows the set and two instances cannot share one file.
 *
 * tb3po.c carries two sequencers ("slots" A and B). This module has one 303,
 * so A is the sequencer and B's notes are dropped.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tb_engine.h"

static void *tb_pool_take(size_t bytes);
static void tb_pool_give(void *p);

#define move_plugin_init_v2 tb3po_upstream_init
#define calloc(n, size) tb_pool_take((size_t)(n) * (size_t)(size))
#define free(p) tb_pool_give(p)
#define pthread_create(thread, attr, fn, arg) ((void)(thread), (void)(fn), (void)(arg), 1)
#define fopen(path, mode) ((FILE *)0)
#define stat(path, st) ((void)(st), -1)
#define mkdir(path, mode) (-1)
#include "tb3po/tb3po.c"
#undef move_plugin_init_v2
#undef calloc
#undef free
#undef pthread_create
#undef fopen
#undef stat
#undef mkdir

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "instance pool needs lock-free atomics");

/* Four chain slots can each hold one; spares cover a swap in flight. */
#define TB_INSTANCES 6
#define TB_SEQ 0                 /* the sequencer: tb3po.c's slot A */
#define TB_SEQ_CHANNEL 1
#define TB_SPARE_CHANNEL 2       /* slot B: runs, is never heard */

struct tb_engine {
    atomic_int used;
    tb_engine_io io;
    plugin_api_v2_t *api;
    tb3po_inst_t inst;
};

static struct tb_engine tb_pool[TB_INSTANCES];
/* The engine an upstream call is being made for. The upstream host callbacks
 * carry no context, and every caller is the one SPI callback thread. */
static struct tb_engine *tb_cur;

static void *tb_pool_take(size_t bytes) {
    if (!tb_cur || bytes != sizeof(tb3po_inst_t)) return NULL;
    memset(&tb_cur->inst, 0, sizeof tb_cur->inst);
    return &tb_cur->inst;
}

static void tb_pool_give(void *p) { (void)p; }

/* ---- the host tb3po.c sees ------------------------------------------------ */
static int tb_host_midi(const uint8_t *pkt, int len) {
    /* pkt = [CIN, status, d1, d2], as for Move's internal MIDI. */
    if (!tb_cur || !pkt || len < 4 || !tb_cur->io.midi) return 0;
    if ((pkt[1] & 0x0F) != TB_SEQ_CHANNEL - 1) return len;
    const uint8_t msg[3] = { (uint8_t)(pkt[1] & 0xF0), pkt[2], pkt[3] };
    tb_cur->io.midi(tb_cur->io.ctx, msg);
    return len;
}

static int tb_host_clock_status(void) {
    return (tb_cur && tb_cur->io.clock_status) ? tb_cur->io.clock_status(tb_cur->io.ctx)
                                               : MOVE_CLOCK_STATUS_UNAVAILABLE;
}

static float tb_host_bpm(void) {
    return (tb_cur && tb_cur->io.bpm) ? tb_cur->io.bpm(tb_cur->io.ctx) : 120.0f;
}

/* No `log`: tb3po.c logs every pad tap, and a log line is file I/O. */
static const host_api_v1_t tb_host = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .sample_rate = SAMPLE_RATE,
    .frames_per_block = MOVE_FRAMES_PER_BLOCK,
    .midi_send_internal = tb_host_midi,
    .get_clock_status = tb_host_clock_status,
    .get_bpm = tb_host_bpm,
};

/* ---- lifecycle ------------------------------------------------------------ */
tb_engine *tb_engine_create(const tb_engine_io *io) {
    for (int i = 0; i < TB_INSTANCES; ++i) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&tb_pool[i].used, &expected, 1)) continue;
        struct tb_engine *e = &tb_pool[i];
        e->io = *io;
        e->api = tb3po_upstream_init(&tb_host);
        tb_cur = e;
        void *inst = e->api->create_instance("", NULL);
        tb_cur = NULL;
        if (inst != &e->inst) { atomic_store(&e->used, 0); return NULL; }
        e->inst.slots[TB_SEQ].channel = TB_SEQ_CHANNEL;
        e->inst.slots[1].channel = TB_SPARE_CHANNEL;
        e->inst.active_slot = TB_SEQ;
        return e;
    }
    return NULL;
}

void tb_engine_destroy(tb_engine *e) {
    if (!e) return;
    tb_cur = e;
    e->api->destroy_instance(&e->inst);   /* releases a held note through io.midi */
    tb_cur = NULL;
    atomic_store(&e->used, 0);
}

void tb_engine_realtime(tb_engine *e, uint8_t status) {
    if (!e) return;
    tb_cur = e;
    e->api->on_midi(&e->inst, &status, 1, MOVE_MIDI_SOURCE_EXTERNAL);
    tb_cur = NULL;
}

void tb_engine_block(tb_engine *e, int frames) {
    if (!e) return;
    tb_cur = e;
    e->api->render_block(&e->inst, NULL, frames);
    tb_cur = NULL;
}

void tb_engine_set(tb_engine *e, const char *key, const char *val) {
    if (!e || !key) return;
    /* The channels are how A is told from B; they are not a setting here. */
    if (!strcmp(key, "channel") || !strcmp(key, "active_slot")) return;
    /* "idx:kind[:degree:octave]". tb3po.c stores degree and octave unchecked
     * and later indexes a 12-entry scale table with the degree, so a value
     * no pad can produce is refused here rather than read past that table. */
    if (!strcmp(key, "set_step")) {
        int idx = 0, kind = 0, deg = 0, oct = 0;
        const int n = val ? sscanf(val, "%d:%d:%d:%d", &idx, &kind, &deg, &oct) : 0;
        if (n < 2 || idx < 0 || idx >= MAX_STEPS || kind < 0 || kind > 3 ||
            deg < 0 || deg > 6 || oct < 0 || oct > 2) return;
    }
    tb_cur = e;
    e->api->set_param(&e->inst, key, val);
    tb_cur = NULL;
}

int tb_engine_get(tb_engine *e, const char *key, char *buf, int len) {
    if (!e || !key) return -1;
    tb_cur = e;
    int n = e->api->get_param(&e->inst, key, buf, len);
    tb_cur = NULL;
    return n;
}

int tb_engine_view(const tb_engine *e, char *buf, int len) {
    if (!e || !buf || len < 1) return -1;
    const tb3po_slot_t *s = &e->inst.slots[TB_SEQ];
    char filled[NUM_BANKS + 1];
    for (int b = 0; b < NUM_BANKS; ++b) filled[b] = s->bank_filled[b] ? '1' : '0';
    filled[NUM_BANKS] = 0;
    int n = snprintf(buf, (size_t)len, "%d,%d,%d,%d,%s,%d,%d,%d,%d|", s->ui_current_step,
                     e->inst.running ? 1 : 0, s->current_bank, s->pending_recall, filled,
                     s->length, s->direction, s->transpose, s->undo_valid ? 1 : 0);
    if (n < 0 || n + s->length >= len) { buf[0] = 0; return -1; }
    for (int i = 0; i < s->length; ++i) buf[n++] = (char)('0' + (s->steps[i] & 3));
    buf[n] = 0;
    return n;
}

/* ---- state ---------------------------------------------------------------- *
 * "1,<len>,<dir>,<density>,<accent>,<slide>,<octaves>,<root>,<scale>,<gate>,
 *  <transpose>,<seed>,<bank>,<64 hex>" then, per bank, ",-" (empty) or
 * ",<len>:<64 hex>". The three probabilities and the gate are thousandths.
 * A step is one byte, two hex digits: kind | degree << 2 | octave << 5.
 * Always all MAX_STEPS: steps past the length are part of the pattern (they
 * come back when the length is turned up again). */
#define TB_STATE_VERSION 1
#define TB_PATTERN_HEX (MAX_STEPS * 2)

static int tb_hex_put(char *buf, int len, int n, const uint8_t *steps, const uint8_t *deg,
                      const uint8_t *oct) {
    static const char digits[] = "0123456789abcdef";
    if (n < 0 || n + TB_PATTERN_HEX >= len) return -1;
    for (int i = 0; i < MAX_STEPS; ++i) {
        const unsigned d = deg[i] > 7 ? 7u : deg[i], o = oct[i] > 3 ? 3u : oct[i];
        const unsigned b = (steps[i] & 3u) | (d << 2) | (o << 5);
        buf[n++] = digits[b >> 4];
        buf[n++] = digits[b & 15];
    }
    buf[n] = 0;
    return n;
}

static int tb_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static const char *tb_hex_get(const char *p, uint8_t *steps, uint8_t *deg, uint8_t *oct) {
    for (int i = 0; i < MAX_STEPS; ++i) {
        const int hi = tb_hex_digit(p[0]), lo = hi < 0 ? -1 : tb_hex_digit(p[1]);
        if (lo < 0) return NULL;
        const int b = hi << 4 | lo;
        steps[i] = (uint8_t)(b & 3);
        deg[i] = (uint8_t)((b >> 2) & 7);
        oct[i] = (uint8_t)((b >> 5) & 3);
        p += 2;
    }
    return p;
}

int tb_engine_state_write(const tb_engine *e, char *buf, int len) {
    if (!e || !buf || len < 1) return -1;
    const tb3po_slot_t *s = &e->inst.slots[TB_SEQ];
    int n = snprintf(buf, (size_t)len, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,", TB_STATE_VERSION,
                     s->length, s->direction, (int)(s->density * 1000.0f + 0.5f),
                     (int)(s->accent * 1000.0f + 0.5f), (int)(s->slide * 1000.0f + 0.5f),
                     s->octave_range, s->root, s->scale, (int)(s->gate * 1000.0f + 0.5f),
                     s->transpose, (unsigned)s->seed, s->current_bank);
    if (n < 0 || n >= len) return -1;
    n = tb_hex_put(buf, len, n, s->steps, s->degrees, s->octaves);
    for (int b = 0; b < NUM_BANKS && n >= 0; ++b) {
        if (!s->bank_filled[b]) {
            if (n + 2 >= len) return -1;
            buf[n++] = ',';
            buf[n++] = '-';
            buf[n] = 0;
            continue;
        }
        const int m = snprintf(buf + n, (size_t)(len - n), ",%d:", s->bank_lengths[b]);
        if (m < 0 || n + m >= len) return -1;
        n = tb_hex_put(buf, len, n + m, s->bank_steps[b], s->bank_degrees[b], s->bank_octaves[b]);
    }
    return n;
}

/* A decimal integer followed by `end`. */
static const char *tb_int(const char *p, char end, long lo, long hi, long *out) {
    if (!p) return NULL;
    char *stop;
    const long v = strtol(p, &stop, 10);
    if (stop == p || *stop != end || v < lo || v > hi) return NULL;
    *out = v;
    return stop + 1;
}

int tb_engine_state_read(tb_engine *e, const char *text) {
    if (!e || !text) return 0;
    /* Parsed into a copy; the live sequencer changes only if all of it is good. */
    static tb3po_slot_t next;             /* 1.2 KB: off the callback's stack */
    next = e->inst.slots[TB_SEQ];
    long v[13];
    static const long lo[13] = {TB_STATE_VERSION, 2, 0, 0, 0, 0, 1, 0, 0, 50, -48, 0, 0};
    static const long hi[13] = {TB_STATE_VERSION, MAX_STEPS, 3, 1000, 1000, 1000, 3, 11,
                                NUM_SCALES - 1, 1000, 48, 0xFFFFFFFFL, NUM_BANKS - 1};
    const char *p = text;
    for (int i = 0; i < 13; ++i) p = tb_int(p, ',', lo[i], hi[i], &v[i]);
    if (!p || !(p = tb_hex_get(p, next.steps, next.degrees, next.octaves))) return 0;
    for (int b = 0; b < NUM_BANKS; ++b) {
        if (*p++ != ',') return 0;
        if (*p == '-') { next.bank_filled[b] = 0; ++p; continue; }
        long len;
        if (!(p = tb_int(p, ':', 1, MAX_STEPS, &len))) return 0;
        if (!(p = tb_hex_get(p, next.bank_steps[b], next.bank_degrees[b], next.bank_octaves[b])))
            return 0;
        next.bank_lengths[b] = (int)len;
        next.bank_filled[b] = 1;
    }
    if (*p) return 0;
    next.length = (int)v[1];
    next.direction = (int)v[2];
    next.density = (float)v[3] / 1000.0f;
    next.accent = (float)v[4] / 1000.0f;
    next.slide = (float)v[5] / 1000.0f;
    next.octave_range = (int)v[6];
    next.root = (int)v[7];
    next.scale = (int)v[8];
    next.gate = (float)v[9] / 1000.0f;
    next.transpose = (int)v[10];
    next.seed = (uint32_t)v[11];
    next.rng = next.seed ? next.seed : 1;
    next.current_bank = (int)v[12];
    for (int b = 0; b < NUM_BANKS; ++b)
        if (!next.bank_filled[b]) next.bank_lengths[b] = next.length;
    clamp_slot_ranges(&next);             /* tb3po.c's own loader guard */
    next.pending_recall = -1;
    next.undo_valid = 0;
    if (next.position >= next.length) next.position = next.length - 1;
    /* The note that is sounding belongs to the pattern being replaced. */
    tb_cur = e;
    kill_last_note(&e->inst, &e->inst.slots[TB_SEQ]);
    tb_cur = NULL;
    next.last_note_on = -1;
    next.gate_samples_remaining = 0;
    e->inst.slots[TB_SEQ] = next;
    return 1;
}
