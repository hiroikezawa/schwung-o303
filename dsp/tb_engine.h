/* o303 - the TB-3PO sequencer, as an engine.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * vendor/tb3po/tb3po.c is TB-3PO's own DSP plugin, compiled in UNMODIFIED.
 * As a tool it sends MIDI to another chain slot; here its MIDI lands in
 * tb_engine_io.midi instead, in the same order and with the same bytes, so
 * whatever it would have played on a 303 in the next slot it plays on the one
 * in this module. Everything a pad or a knob does to the sequencer is one of
 * TB-3PO's own set_param keys (density, set_step, store_bank, generate, ...).
 */
#ifndef TB_ENGINE_H
#define TB_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_engine tb_engine;

typedef struct {
    void *ctx;
    /* TB-3PO's MIDI out: note on/off (velocity 118 = accent, 72 = plain; a
     * slide is the next note-on BEFORE the previous note-off), CC 65, CC 123.
     * msg is the 3 MIDI bytes; the channel nibble is always 0. */
    void (*midi)(void *ctx, const uint8_t *msg);
    float (*bpm)(void *ctx);
    int (*clock_status)(void *ctx);     /* MOVE_CLOCK_STATUS_* */
} tb_engine_io;

/* Every call below runs on the SPI callback: no heap, no file, no thread. */
tb_engine *tb_engine_create(const tb_engine_io *io);
void tb_engine_destroy(tb_engine *e);

/* MIDI realtime from Move's transport: 0xF8 clock, 0xFA/0xFB/0xFC. */
void tb_engine_realtime(tb_engine *e, uint8_t status);
/* Once per audio block: gate lengths, transport recovery, the internal timer
 * when no MIDI clock arrives. */
void tb_engine_block(tb_engine *e, int frames);

/* TB-3PO's own parameter keys, unprefixed (the one sequencer is its slot A). */
void tb_engine_set(tb_engine *e, const char *key, const char *val);
int tb_engine_get(tb_engine *e, const char *key, char *buf, int len);

/* One read for the UI:
 *   "<position>,<running>,<bank>,<pending or -1>,<8 x 0|1 filled>,<length>,
 *    <direction>,<transpose>,<undo 0|1>|<one digit 0..3 per step>" */
int tb_engine_view(const tb_engine *e, char *buf, int len);

/* The whole sequencer (pattern, 8 banks, every setting) as one token of
 * [0-9A-Za-z,:.-] characters, for the slot's state blob. Returns the length,
 * or -1 if it does not fit. Reading is transactional: 1 = applied, 0 = the
 * text was rejected and nothing changed. */
int tb_engine_state_write(const tb_engine *e, char *buf, int len);
int tb_engine_state_read(tb_engine *e, const char *text);

#ifdef __cplusplus
}
#endif
#endif
