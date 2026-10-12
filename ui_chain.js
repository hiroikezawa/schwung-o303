/*
 * o303 - Signal Chain UI.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * The shadow UI opens this (instead of its stock knob grid) because the DSP
 * answers `ui_hierarchy` with an empty string. Everything it does lives in
 * ui_core.mjs; this file only binds Schwung's globals and shared library,
 * resolved at CALL time because host_module_get/set_param are per-slot shims
 * the shadow UI installs around this module.
 */
import { createUi } from './ui_core.mjs';
import { createPages } from './pages.mjs';
import { createController, LAYOUT_MOVY }
    from '/data/UserData/schwung/shared/param_pages/page_controller.mjs';
import { decodeInput, applyInput }
    from '/data/UserData/schwung/shared/param_pages/page_input.mjs';
import { drawKnobRow, drawHeader, drawBankBar, drawFooter, movyHeaderFor }
    from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';
import { isDivable } from '/data/UserData/schwung/shared/param_pages/param_meta.mjs';
import { createAnimState } from '/data/UserData/schwung/shared/param_pages/anim_state.mjs';
import { fontPrint4x5, fontWidth4x5 } from '/data/UserData/schwung/shared/param_pages/font4x5.mjs';
import { drawEnumList } from '/data/UserData/schwung/shared/param_pages/enum_list.mjs';
import { listKnobInit, listKnobStep } from '/data/UserData/schwung/shared/param_pages/list_knob.mjs';
import { decodeDelta } from '/data/UserData/schwung/shared/input_filter.mjs';
import { showOverlay, tickOverlay, drawOverlay } from '/data/UserData/schwung/shared/menu_layout.mjs';

const has = (name) => typeof globalThis[name] === "function";

const core = createUi({
    createController, decodeInput, applyInput, decodeDelta,
    /* The grid, always: the step lane and the pad map are drawn around its
     * cells (TB-3PO does the same). */
    layoutGrid: LAYOUT_MOVY,
    listKnobInit, listKnobStep, drawEnumList,
    pages: createPages({ drawKnobRow, drawHeader, drawBankBar, drawFooter, movyHeaderFor, isDivable,
                         createAnimState, fontPrint4x5, fontWidth4x5 }),
    overlay: { show: showOverlay, tick: tickOverlay, draw: drawOverlay },
    getParam: (key) => has("host_module_get_param") ? host_module_get_param(key) : null,
    setParam: (key, value) => has("host_module_set_param")
        ? host_module_set_param(key, String(value)) !== false : false,
    /* As screen_reader.mjs announce(): only while the shadow UI is shown. */
    announce: (text) => {
        if (!text || !has("host_send_screenreader")) return;
        if (has("shadow_get_display_mode") && shadow_get_display_mode() !== 1) return;
        host_send_screenreader(text);
    },
    draw: {
        fillRect: (...a) => fill_rect(...a),
        print: (...a) => print(...a),
        textWidth: (t) => has("text_width") ? text_width(t) : String(t).length * 5,
        line: has("draw_line") ? draw_line : undefined,
        fillCircle: has("fill_circle") ? fill_circle : undefined,
        drawCircle: has("draw_circle") ? draw_circle : undefined,
        drawArc: has("draw_arc") ? draw_arc : undefined,
    },
    clear: () => clear_screen(),
    now: () => Date.now(),
    shiftHeld: () => has("shadow_get_shift_held") && shadow_get_shift_held() !== 0,
    displayOn: () => !has("shadow_get_display_mode") || shadow_get_display_mode() === 1,
    padBlock: (on) => { if (has("host_pad_block")) host_pad_block(on ? 1 : 0); },
    sendLed: (note, color) => has("move_midi_internal_send")
        ? move_midi_internal_send([0x09, 0x90, note, color]) !== false : false,
    padSnapshot: () => has("shadow_get_pad_led_snapshot") ? shadow_get_pad_led_snapshot() : null,
});

globalThis.chain_ui = {
    init: core.init,
    tick: core.tick,
    onMidiMessageInternal: core.onMidi,
    handleBack: () => {
        const consumed = core.handleBack();
        if (!consumed) core.releaseAll();
        return consumed;
    },
};
