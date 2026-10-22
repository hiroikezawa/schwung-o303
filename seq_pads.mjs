/*
 * o303 - the pads and the buttons, as TB-3PO has them.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * This file is TB-3PO's pad layer (vendor/tb3po/ui/ui.js: handlePadNoteOn,
 * cycleStepState, refreshLeds, and the + - < > Undo handlers), lifted out of
 * the overtake tool it is welded into there so a chain synth's screen can use
 * it. Same pad numbers, same tap cycle, same colours, same flash phase, same
 * DSP verbs with the same arguments. tests/test_ui.mjs runs TB-3PO's real
 * ui.js beside it, feeds both the same presses, and requires the same writes
 * and the same LEDs -- so "the same" is checked, not claimed.
 *
 * PURE: state in, { writes, overlay } out. Nothing here touches a global,
 * which is what lets that test run it against a fake sequencer.
 *
 *   row 3 (notes 92-99, top)   steps 1-8 of the step window
 *   row 2 (84-91)              steps 9-16
 *   row 1 (76-83)              banks 1-8: tap recalls (at the bar line while
 *                              playing), Shift+tap saves
 *   row 0 (68-75)              NEW  MUT  DIR  OUT  IN  -  -  -
 *
 * THE ONE DIFFERENCE: TB-3PO's pads 4 and 5 are Ch- / Ch+, the MIDI channel
 * its notes leave on. Here the notes go to the 303 inside this module and
 * there is no channel to choose, so those two pads are this module's own:
 *
 *   OUT (pad 4)   a random note of the phrase moves one scale step AWAY from
 *                 the phrase's mean pitch (up if above it, down if below)
 *   IN  (pad 5)   the highest or lowest note -- whichever is further from the
 *                 mean -- moves one scale step TOWARD it
 *
 * The moves are made by the DSP (tb_engine.c `spread` / `focus`), which
 * knows the scale and the octaves allowed; Undo takes one back.
 */

export const STEP_REST = 0, STEP_NOTE = 1, STEP_ACCENT = 2, STEP_SLIDE = 3;
export const NOTE_PAD_BASE = 68, NOTE_PAD_LAST = 99;
export const MAX_STEPS = 32, NUM_BANKS = 8;

export const CC_DOWN = 54, CC_UP = 55, CC_UNDO = 56, CC_LEFT = 62, CC_RIGHT = 63, CC_DELETE = 119;

/* Move's pad palette indices -- TB-3PO's own table. */
export const LED = Object.freeze({
    OFF: 0, DARK_GREY: 124, WHITE: 120, RED: 1, ORANGE: 47, YELLOW: 50, GREEN: 8,
    TEAL: 87, BLUE: 95, INDIGO: 102, PURPLE: 107, PINK: 25,
});

export const DIRECTIONS = ["Fwd", "Rev", "Ping", "Rnd"];

/** What the screen knows of the sequencer: a cache, the DSP is the truth. */
export function makeSeq() {
    return {
        position: 0,
        running: 0,
        length: 16,
        direction: 0,
        currentBank: 0,
        pendingRecall: -1,
        bankFilled: new Array(NUM_BANKS).fill(false),
        transpose: 0,              /* semitones, as TB-3PO keeps it */
        undo: 0,
        steps: new Array(MAX_STEPS).fill(STEP_REST),
        stepView: 0,               /* which 16-step window the pads edit */
        bpm: 120,
    };
}

export function stepPageCount(seq) { return Math.max(1, Math.ceil(seq.length / 16)); }

export function clampStepView(seq) {
    const m = stepPageCount(seq) - 1;
    if (seq.stepView > m) seq.stepView = m;
    if (seq.stepView < 0) seq.stepView = 0;
}

const none = () => ({ writes: [], overlay: null, changed: [] });

/* rest -> note -> accent -> slide -> rest */
function cycleStep(seq, stepIdx, out) {
    if (stepIdx < 0 || stepIdx >= seq.length) return;
    const next = ((seq.steps[stepIdx] | 0) + 1) & 3;
    seq.steps[stepIdx] = next;
    out.writes.push(["set_step", stepIdx + ":" + next]);
}

/**
 * A pad went down.
 *
 * @param {object} seq   makeSeq() state; updated in place the way TB-3PO
 *                       updates its own cache, so the LED answers the press
 *                       before the next poll confirms it
 * @param {number} note  68..99
 * @param {{shift:boolean}} mods
 * @returns {{writes: Array<[string,string]>, overlay: [string,string]|null,
 *            changed: string[]}}  `changed`: params the knob grid also shows
 */
export function padPress(seq, note, mods = {}) {
    const out = none();
    const padIdx = note - NOTE_PAD_BASE;
    if (padIdx < 0 || padIdx > 31) return out;
    const row = Math.floor(padIdx / 8);            /* 0 bottom, 3 top */
    const col = padIdx % 8;

    if (row === 3) {
        cycleStep(seq, seq.stepView * 16 + col, out);
    } else if (row === 2) {
        cycleStep(seq, seq.stepView * 16 + 8 + col, out);
    } else if (row === 1) {
        const bn = col + 1;
        if (mods.shift) {
            out.writes.push(["store_bank", String(col)]);
            seq.currentBank = col;
            seq.bankFilled[col] = true;
            out.changed.push("current_bank");
            out.overlay = ["Bank " + bn, "saved"];
        } else if (seq.running) {
            /* Queued; the DSP applies it at the next bar boundary. */
            out.writes.push(["recall_bank", String(col)]);
            seq.pendingRecall = col;
            out.overlay = ["Bank " + bn, "queued"];
        } else {
            /* Transport stopped: no bar boundary is coming, apply now. */
            out.writes.push(["recall_bank_now", String(col)]);
            seq.currentBank = col;
            seq.pendingRecall = -1;
            out.changed.push("current_bank");
            out.overlay = ["Bank " + bn, "recalled"];
        }
    } else {
        switch (col) {
            case 0:
                out.writes.push(["generate", "0"]);
                out.overlay = ["Pattern", "regenerated"];
                break;
            case 1:
                out.writes.push(["mutate", "1"]);
                out.overlay = ["Pattern", "mutated"];
                break;
            case 2:
                seq.direction = (seq.direction + 1) & 3;
                out.writes.push(["direction", String(seq.direction)]);
                out.changed.push("direction");
                out.overlay = ["Direction", DIRECTIONS[seq.direction] || "?"];
                break;
            case 3:                               /* TB-3PO's Ch- -- see the header */
                out.writes.push(["spread", "1"]);
                out.overlay = ["Phrase", "out"];
                break;
            case 4:                               /* TB-3PO's Ch+ */
                out.writes.push(["focus", "1"]);
                out.overlay = ["Phrase", "in"];
                break;
            default: break;
        }
    }
    return out;
}

function stepColor(kind) {
    switch (kind) {
        case STEP_NOTE:   return LED.WHITE;
        case STEP_ACCENT: return LED.RED;
        case STEP_SLIDE:  return LED.BLUE;
        default:          return LED.OFF;
    }
}

/**
 * What a pad should be showing.
 *
 * @param {{shift:boolean, flash:number}} mods  flash: TB-3PO's phase,
 *        floor(tick / 4) & 1, from the tick counter of the frame being drawn
 */
export function padColor(seq, note, mods = {}) {
    const padIdx = note - NOTE_PAD_BASE;
    if (padIdx < 0 || padIdx > 31) return LED.OFF;
    const row = Math.floor(padIdx / 8);
    const col = padIdx % 8;
    if (row >= 2) {
        const step = seq.stepView * 16 + (row === 3 ? 0 : 8) + col;
        /* Yellow, not white: white is a plain NOTE step. */
        if (seq.running && step === seq.position) return LED.YELLOW;
        return step < seq.length ? stepColor(seq.steps[step]) : LED.OFF;
    }
    if (row === 1) {
        /* Shift held: the whole row pulses red/pink -- "these are save targets". */
        if (mods.shift) return col === seq.currentBank ? LED.WHITE : (mods.flash ? LED.RED : LED.PINK);
        if (col === seq.pendingRecall && mods.flash) return LED.YELLOW;
        if (col === seq.currentBank) return LED.PURPLE;
        return seq.bankFilled[col] ? LED.TEAL : LED.OFF;
    }
    return col === 0 ? LED.TEAL : col === 1 ? LED.INDIGO : col === 2 ? LED.ORANGE
         : col === 3 ? LED.GREEN : col === 4 ? LED.BLUE : LED.OFF;
}

/**
 * + - (octave), < > (step window), Undo. Delete is not here: TB-3PO's CLEAR
 * is Shift+X, and in a chain slot Shift+Delete is Schwung's own snapshot
 * recall, so ui_core.mjs gives CLEAR its own two-press form.
 */
export function buttonPress(seq, cc) {
    const out = none();
    switch (cc) {
        case CC_UNDO:
            out.writes.push(["undo", "1"]);
            out.overlay = ["Undo", "last op"];
            break;
        case CC_DOWN:
        case CC_UP:
            seq.transpose = cc === CC_UP ? Math.min(48, (seq.transpose | 0) + 12)
                                         : Math.max(-48, (seq.transpose | 0) - 12);
            out.writes.push(["transpose", String(seq.transpose)]);
            out.changed.push("transpose");
            out.overlay = ["Transpose", (seq.transpose / 12) + " oct"];
            break;
        case CC_LEFT:
            if (stepPageCount(seq) > 1 && seq.stepView > 0) { seq.stepView--; out.changed.push("step_view"); }
            break;
        case CC_RIGHT:
            if (stepPageCount(seq) > 1 && seq.stepView < stepPageCount(seq) - 1) {
                seq.stepView++;
                out.changed.push("step_view");
            }
            break;
        default: break;
    }
    return out;
}

/**
 * Parse the DSP's `seq_view` into `seq`. Returns false (and changes nothing)
 * for anything that is not a complete answer -- a read that timed out is not
 * news about the sequencer.
 *
 *   "<bpm>;<position>,<running>,<bank>,<pending>,<8 x 0|1>,<length>,
 *    <direction>,<transpose>,<undo>|<one digit per step>"
 */
export function applyView(seq, raw) {
    if (typeof raw !== "string" || !raw.length) return false;
    const m = /^(\d+);(-?\d+),([01]),(\d+),(-?\d+),([01]{8}),(\d+),(\d+),(-?\d+),([01])\|([0-3]*)$/.exec(raw);
    if (!m) return false;
    const length = parseInt(m[7], 10);
    if (length < 1 || length > MAX_STEPS || m[11].length !== length) return false;
    seq.bpm = parseInt(m[1], 10);
    seq.position = parseInt(m[2], 10);
    seq.running = m[3] === "1" ? 1 : 0;
    seq.currentBank = parseInt(m[4], 10);
    seq.pendingRecall = parseInt(m[5], 10);
    for (let i = 0; i < NUM_BANKS; i++) seq.bankFilled[i] = m[6][i] === "1";
    seq.length = length;
    seq.direction = parseInt(m[8], 10) & 3;
    seq.transpose = parseInt(m[9], 10);
    seq.undo = m[10] === "1" ? 1 : 0;
    /* Only the live steps come back; the rest keep what they last showed and
     * are not drawn (TB-3PO's parsePattern does the same). */
    for (let i = 0; i < length; i++) seq.steps[i] = m[11].charCodeAt(i) - 48;
    clampStepView(seq);
    return true;
}
