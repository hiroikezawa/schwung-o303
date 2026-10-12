/*
 * o303 - Signal Chain UI core.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * TB-3PO's screen and TB-3PO's pads, running as a chain synth's own page
 * instead of as an overtake tool. Three files carry it:
 *
 *   seq_pads.mjs   what a pad or a button DOES, and what colour a pad is
 *                  (TB-3PO's pad layer, checked against TB-3PO's ui.js)
 *   pages.mjs      what the screen shows (TB-3PO's page composition)
 *   this file      the glue TB-3PO's ui.js is for an overtake tool, redone for
 *                  a chain slot: which page is up, where input goes, when the
 *                  pads are ours, and the polling of the DSP
 *
 * WHY THE MODULE DRAWS ITS OWN SCREEN: Schwung's stock knob grid runs no
 * module code, so it could neither take the pads nor draw a step lane. The
 * DSP therefore serves `ui_hierarchy` empty (the shadow UI then opens
 * ui_chain.js) and this file drives Schwung's OWN page engine
 * (page_controller.mjs) from the hierarchy served as `grid_hierarchy`: same
 * knob grid, same widgets, same knob feel.
 *
 * WHAT A CHAIN SLOT CHANGES AGAINST THE TOOL (everything else is TB-3PO's):
 *
 *   pads      taken with host_pad_block while this screen is up, and handed
 *             back -- with Move's own colours -- when it is not. Restated
 *             every frame: the shim drops the block on its own.
 *   Shift     is not MIDI here. The shim swallows CC 49 in shadow mode and
 *             publishes it in shared memory (shadow_get_shift_held).
 *   Back      is the host's: it calls handleBack(), which peels one layer
 *             (peek, option list) and otherwise lets the host leave.
 *   pages     the jog, as in TB-3PO. TB-3PO's other two page selectors do not
 *             exist in a chain slot: the track buttons are Schwung's slot
 *             switch, and a step-button tap is replayed to Move's sequencer.
 *   + - < >   claimed through capabilities.claims_ccs (module.json), so they
 *   Undo  X   do TB-3PO's job while this screen is up and Move's otherwise.
 *   CLEAR     TB-3PO's Shift+X cannot exist: Shift+Delete is Schwung's own
 *             snapshot recall and a Shift press is never handed to a module.
 *             X asks ("Clear? / X again"), a second X within the prompt does it.
 *   slots     one sequencer. TB-3PO's slot B and its MIDI channel are gone
 *             with the reason for them: the 303 is inside.
 *
 * Every dependency is injected (ui_chain.js binds the device, the tests bind
 * fakes), so nothing here touches a global.
 */
import { makeSeq, padPress, padColor, buttonPress, applyView, clampStepView, stepPageCount,
         NOTE_PAD_BASE, NOTE_PAD_LAST, CC_DOWN, CC_UP, CC_UNDO, CC_LEFT, CC_RIGHT, CC_DELETE }
    from "./seq_pads.mjs";
import { PAGES, PERFORM_KNOBS } from "./pages.mjs";

/* One IPC read costs ~2.8 ms (a whole page render is 1.68 ms), so the
 * sequencer is polled with ONE combined key, every other frame: ~30 ms behind
 * the playhead at worst, against a 1/16 of 107 ms at 140 BPM. */
export const VIEW_EVERY_TICKS = 2;
/*
 * MOVE KEEPS PAINTING THE PADS UNDER US.
 *
 * host_pad_block takes the pad PRESSES from Move, not its LEDs: Move goes on
 * drawing its own track on them (a Drum Rack's pads lighting as the clip
 * plays), and nothing in a chain slot can stop that -- the shim's LED strip
 * belongs to Schwung's Scenes screen alone. TB-3PO never met this: as an
 * overtake tool it owns the whole surface.
 *
 * So the module takes the pads BACK as fast as it can see them taken: the
 * shim publishes the last colour Move wrote to every pad (the same cache it
 * restores from when we let go), and a pad whose Move colour changed since
 * the last frame is repainted with ours in this one. A repaint Move makes
 * with an unchanged colour is invisible to that test, so every pad is also
 * repainted this often regardless (~150 ms; 32 packets of the 128 a flush
 * holds). What is left is Move's colour for about a frame.
 */
export const LED_REFRESH_TICKS = 9;
/* How long X's "Clear?" stays armed. */
export const CLEAR_CONFIRM_MS = 1500;

const CC_KNOB_FIRST = 71, CC_KNOB_LAST = 78, CC_JOG = 14, CC_JOG_CLICK = 3, CC_SHIFT = 49;
const TRANSPOSE_INDEX = (semis) => Math.round(semis / 12) + 4;   /* the DSP's `transpose` is an option index */

export function createUi(deps) {
    const {
        createController, decodeInput, applyInput, decodeDelta, layoutGrid,
        listKnobInit, listKnobStep, drawEnumList, pages,
        overlay = { show: () => {}, tick: () => {}, draw: () => {} },
        getParam, setParam, announce = () => {}, draw, clear,
        now = () => Date.now(), shiftHeld = () => false,
        displayOn = () => true, padBlock = () => {},
        sendLed = () => true, padSnapshot = () => null,
    } = deps;

    const st = {
        controller: null,
        seq: makeSeq(),
        pageIndex: 0,          /* into PAGES; lands on Synth, knob 1 = VOLUME */
        ctlIndex: [],          /* PAGES[i] -> the controller page behind it, or -1 */
        tick: 0,
        shift: false,
        shiftCc: false,        /* a host that does forward CC 49 */
        visible: false,
        picker: null,          /* the option list opened from a held enum knob */
        clearArmedAt: -1,
        heldPads: new Set(),
        sent: {},              /* pad note -> colour Move last accepted */
        moveLeds: null,        /* the colours Move last wrote, as of the previous frame */
        polled: null,          /* what the last poll said, for edge detection */
    };

    const bare = (fullKey) => {
        const i = fullKey.indexOf(":");
        return i < 0 ? fullKey : fullKey.slice(i + 1);
    };
    const page = () => PAGES[st.pageIndex];

    /* The controller asks for `synth:<key>`; the host's per-slot shims add the
     * prefix themselves, so it is stripped here. The hierarchy lives under
     * grid_hierarchy (tr_plugin.c). null passes through untouched: a read that
     * did not complete must stay distinguishable from an empty answer. */
    function ioGet(fullKey) {
        const k = bare(fullKey);
        return getParam(k === "ui_hierarchy" ? "grid_hierarchy" : k);
    }

    /* ---- which controller page is behind the page on screen ---------------- */
    function mapPages() {
        const planned = st.controller.pages || [];
        st.ctlIndex = PAGES.map((p) => p.first
            ? planned.findIndex((q) => q && Array.isArray(q.keys) && q.keys[0] === p.first) : -1);
    }

    /* ONE function, because there are two page indices: this file owns the
     * outer one (PAGES), the controller its own. Every path that moves the
     * outer index ends here. `remember: false`: go exactly there, not where
     * the controller's section memory would land. */
    function syncController() {
        const target = st.ctlIndex[st.pageIndex];
        const c = st.controller;
        if (target >= 0 && target !== c.pageIndex) c.goToPage(target, { remember: false });
    }

    /* Which knobs are live on the page the user is looking at. The controller
     * is parked on Pattern while Steps is shown, so its knobs 5-8 have keys --
     * but Steps draws one row, and only those four do anything. */
    function knobLive(slot) {
        const kind = page().kind;
        if (st.ctlIndex[st.pageIndex] < 0) return false;
        if (kind === "knobs") return true;
        return kind === "perform" && slot < PERFORM_KNOBS;
    }

    function heldSlot() {
        const t = st.controller ? st.controller.state.touched : -1;
        return t >= 0 && knobLive(t) ? t : -1;
    }

    /* ---- values the grid did not write ---------------------------------------- *
     * A pad, a button or a bank recall changes a param the grid also shows.
     * The controller holds two things per key -- the cached reading and the
     * knob engine's running value, which is seeded ONCE -- so leaving it alone
     * is a knob that snaps the value BACK on its next detent. Dropping the
     * engine state re-seeds it. (TB-3PO's noteExternalChange.) */
    function tell(key, text) {
        const s = st.controller && st.controller.state;
        if (!s) return;
        if (String(s.values[key]) === text) return;
        s.values[key] = text;
        delete s.knobStates[key];
    }

    function tellChanged(keys) {
        const q = st.seq;
        for (const k of keys) {
            if (k === "direction") tell(k, String(q.direction));
            else if (k === "transpose") tell(k, String(TRANSPOSE_INDEX(q.transpose)));
            else if (k === "current_bank") tell(k, String(q.currentBank + 1));
            else if (k === "length") tell(k, String(q.length));
        }
    }

    /* ---- the DSP ------------------------------------------------------------------ */
    function send(writes) {
        for (const [key, val] of writes)
            setParam(key, key === "transpose" ? String(TRANSPOSE_INDEX(Number(val))) : val);
    }

    function act(result) {
        send(result.writes);
        if (result.overlay) overlay.show(result.overlay[0], result.overlay[1]);
        tellChanged(result.changed);
        if (result.changed.includes("step_view")) announceStepWindow();
    }

    /* The step window is the one page-ish change that is not always on screen:
     * Steps' header and the pad map name it, no other page does. */
    function announceStepWindow() {
        if (page().kind === "perform" || page().kind === "pads") return;
        const q = st.seq;
        overlay.show("Steps", (q.stepView * 16 + 1) + "-" + Math.min(q.length, (q.stepView + 1) * 16));
    }

    function poll() {
        const q = st.seq;
        if (!applyView(q, getParam("seq_view"))) return;      /* null / "": keep the last answer */
        /* EDGE-triggered against the previous POLL, not against the grid: a
         * value the knob just wrote is already in the grid (and the DSP may be
         * a write behind it), so only a change the DSP made by itself -- a
         * bank recall bringing its length back -- is told to the grid. */
        const seen = { length: q.length, current_bank: q.currentBank, direction: q.direction,
                       transpose: q.transpose };
        if (st.polled) {
            const moved = Object.keys(seen).filter((k) => seen[k] !== st.polled[k]);
            if (moved.length) tellChanged(moved);
        }
        st.polled = seen;
    }

    /* ---- the option list (TB-3PO's picker) ----------------------------------------- *
     * Hold an enum knob, click the jog: its options open full screen. The jog
     * or the knob still under the hand scrolls, a click sets, Back cancels.
     * The gesture and the decision are the controller's (applyInput -> onClick
     * -> takePending); the list is drawn here because the controller hands the
     * host an intent and expects the host to own the screen. Nothing is
     * written while scrolling: the commit is one commitEnum at close. */
    function openPickerFrom(pending) {
        if (!pending || pending.action !== "open") return false;
        const options = Array.isArray(pending.options) ? pending.options.map(String) : [];
        if (!options.length) return false;
        const meta = pending.meta || {};
        const live = Math.max(0, Math.min(options.length - 1, pending.index | 0));
        st.picker = { key: pending.key, title: meta.label || meta.name || pending.key, options,
                      index: live, openIndex: live, knob: listKnobInit() };
        /* A peek under a picker would invert the Back ladder. */
        if (st.controller.dismissPeek) st.controller.dismissPeek();
        return true;
    }

    function pickerMove(delta) {
        const p = st.picker;
        if (p && delta) p.index = Math.max(0, Math.min(p.options.length - 1, p.index + delta));
    }

    function closePicker(commit) {
        const p = st.picker;
        st.picker = null;
        if (p && commit) st.controller.commitEnum(p.key, p.index);
    }

    /* ---- shift ------------------------------------------------------------------------ */
    function syncShift() {
        st.shift = st.shiftCc || !!shiftHeld();
    }

    /* ---- pads -------------------------------------------------------------------------- */
    const flash = () => Math.floor(st.tick / 4) & 1;       /* TB-3PO's phase */

    function colorOf(note) {
        return padColor(st.seq, note, { shift: st.shift, flash: flash() });
    }

    /* Only what changed is sent: the shadow UI's MIDI-out buffer is 128
     * packets a flush, and a packet it refuses is not cached, so it is retried
     * next frame instead of leaving the pad wrong for good. */
    function paintPads() {
        const snap = padSnapshot();
        if (snap) {
            const before = st.moveLeds;
            if (before)
                for (let n = NOTE_PAD_BASE; n <= NOTE_PAD_LAST; n++)
                    if (snap[n] !== before[n]) delete st.sent[n];       /* Move drew here */
            st.moveLeds = snap;
        }
        if (st.tick % LED_REFRESH_TICKS === 0) st.sent = {};
        for (let n = NOTE_PAD_BASE; n <= NOTE_PAD_LAST; n++) {
            const c = colorOf(n);
            if (st.sent[n] === c) continue;
            if (sendLed(n, c) !== false) st.sent[n] = c;
        }
    }

    /* Move's own pad colours come back when we let go of the pads. */
    function restorePads() {
        const snap = padSnapshot();
        for (let n = NOTE_PAD_BASE; n <= NOTE_PAD_LAST; n++) sendLed(n, (snap && snap[String(n)]) || 0);
        st.sent = {};
    }

    function onPad(note, down) {
        if (!down) { st.heldPads.delete(note); return; }
        if (st.heldPads.has(note)) return;
        st.heldPads.add(note);
        if (st.picker) return;                               /* the list owns the surface */
        act(padPress(st.seq, note, { shift: st.shift }));
    }

    /* ---- buttons ------------------------------------------------------------------------ */
    function onDelete() {
        const t = now();
        if (st.clearArmedAt >= 0 && t - st.clearArmedAt <= CLEAR_CONFIRM_MS) {
            st.clearArmedAt = -1;
            setParam("clear", "1");
            overlay.show("Pattern", "cleared");
        } else {
            st.clearArmedAt = t;
            overlay.show("Clear?", "X again");
        }
    }

    /* ---- lifecycle ---------------------------------------------------------------------- */
    function init() {
        st.controller = createController({ getParam: ioGet, setParam: (k, v) => setParam(bare(k), v),
                                           announce, now });
        if (layoutGrid) st.controller.setLayout(layoutGrid);
        st.controller.load({ slot: 0, component: "synth", prefix: "synth" });
        mapPages();
        syncController();
        poll();
    }

    function view() {
        const q = st.seq;
        return { pageIndex: st.pageIndex, steps: q.steps, position: q.position, stepView: q.stepView,
                 bpm: q.bpm, shiftHeld: st.shift, touched: heldSlot(), ctl: st.controller, nowMs: now() };
    }

    function render() {
        clear();
        const p = st.picker;
        if (p) {
            /* The list OWNS the screen: the page's knob row is what it replaced. */
            drawEnumList(draw, { title: p.title, headerRight: "SELECT", options: p.options, index: p.index,
                                 markIndex: p.openIndex,
                                 footer: [["JOG", "SEL"], ["CLK", "SET"], ["BACK", "EXIT"]] });
            return;
        }
        pages.drawPage(draw, view());
        overlay.draw();
        /* The enum peek, over everything: turn a multi-option enum and its
         * options rise for a moment. The controller tracks it and swallows
         * the Back that dismisses it either way; this is what paints it. */
        st.controller.renderOverlays(draw, { clearScreen: clear });
    }

    function tick() {
        const c = st.controller;
        if (!c) return;
        if (!displayOn()) { if (st.visible) releaseAll(); return; }
        if (!st.visible) { st.visible = true; st.sent = {}; }
        padBlock(true);                    /* restated every frame: the shim drops it on its own */
        syncShift();
        overlay.tick();
        if (st.tick % VIEW_EVERY_TICKS === 0) poll();
        st.tick++;
        /* A contract that could not be read at init plans no pages; the
         * controller retries on its own, and the map follows it. */
        if (st.ctlIndex[0] < 0) { mapPages(); syncController(); }
        c.tick();
        paintPads();
        render();
    }

    function onMidi(data) {
        const c = st.controller;
        if (!c || !data || data.length < 3 || !displayOn()) return;
        const status = data[0] & 0xf0, d1 = data[1], d2 = data[2];

        if (status === 0x90 || status === 0x80) {
            if (d1 >= NOTE_PAD_BASE && d1 <= NOTE_PAD_LAST) {
                syncShift();               /* read at the press itself, not a tick later */
                onPad(d1, status === 0x90 && d2 > 0);
                return;
            }
            /* Knob touch 0-7 goes to the controller ALWAYS, even on Pads and
             * Keys: a touch is an edge pair, and gating the release on the page
             * is how a knob is left stuck as held. 8/9 (master, jog) and the
             * step buttons (16-31, which Move still plays) are nobody's here. */
            if (d1 < 8) applyInput(c, decodeInput(data, {}), { nowMs: now() });
            return;
        }
        if (status !== 0xb0) return;
        if (d1 === CC_SHIFT) { st.shiftCc = d2 > 0; syncShift(); return; }
        syncShift();

        /* The list owns the surface, and this sits ABOVE everything it
         * suppresses: the draw path puts it last, so the input path feeds it
         * first. */
        if (st.picker) {
            if (d1 === CC_JOG) pickerMove(decodeDelta(d2));
            else if (d1 >= CC_KNOB_FIRST && d1 <= CC_KNOB_LAST) {
                /* The hand is still on the knob that opened it. Through the
                 * list accumulator, not 1:1: a knob detent is a fraction of a
                 * twist, a jog detent is a click you feel. */
                const p = st.picker;
                const step = listKnobStep(p.knob, decodeDelta(d2), now(), p.options.length);
                if (step) pickerMove(step);
            } else if (d1 === CC_JOG_CLICK && d2 > 0) closePicker(true);
            return;
        }

        if (d1 === CC_DELETE) { if (d2 > 0) onDelete(); return; }
        if (d1 === CC_UNDO || d1 === CC_DOWN || d1 === CC_UP || d1 === CC_LEFT || d1 === CC_RIGHT) {
            if (d2 > 0) act(buttonPress(st.seq, d1));
            return;
        }

        if (d1 >= CC_KNOB_FIRST && d1 <= CC_KNOB_LAST) {
            if (!knobLive(d1 - CC_KNOB_FIRST)) return;
            /* Shift is NOT passed as `fine`: on this surface Shift means "the
             * bank row is save targets" (TB-3PO). */
            applyInput(c, decodeInput(data, {}), { nowMs: now() });
            return;
        }

        if (d1 === CC_JOG) {
            /* Pages, CAPPED not wrapped: the ends are something to run into.
             * Not routed through applyInput -- its page intent moves the
             * controller's own index, and wraps. */
            const delta = decodeDelta(d2);
            if (!delta) return;
            const next = st.pageIndex + (delta > 0 ? 1 : -1);
            st.pageIndex = Math.max(0, Math.min(PAGES.length - 1, next));
            syncController();
            return;
        }

        if (d1 === CC_JOG_CLICK && d2 > 0) {
            /* Dive into the cell under the hand, if it has anything to dive
             * into; nothing held, nothing happens. Shift+click would open the
             * controller's own section list -- five entries under a bank bar
             * showing eight -- so it is not routed. */
            if (st.shift || heldSlot() < 0) return;
            openPickerFrom(applyInput(c, decodeInput(data, {}), { nowMs: now() }));
        }
    }

    /* Back peels one layer before it leaves: hint, peek, option list. */
    function handleBack() {
        const c = st.controller;
        if (!c) return false;
        if (c.dismissHint && c.dismissHint()) return true;
        if (c.dismissPeek && c.dismissPeek()) return true;
        if (st.picker) { closePicker(false); return true; }    /* cancel: nothing was written */
        return false;
    }

    /* Leaving: the pads (and their colours) go back to Move. */
    function releaseAll() {
        st.picker = null;
        st.heldPads.clear();
        st.clearArmedAt = -1;
        if (st.controller && st.controller.clearTouch) st.controller.clearTouch();
        restorePads();
        padBlock(false);
        st.visible = false;
    }

    return { init, tick, onMidi, handleBack, releaseAll, state: st,
             /* for the tests: which key a physical knob edits right now */
             keyForKnob: (slot) => (slot >= 0 && slot < 8 && knobLive(slot) ? st.controller.keyAt(slot) || null : null),
             colorOf, stepPageCount: () => stepPageCount(st.seq), clampStepView: () => clampStepView(st.seq) };
}
