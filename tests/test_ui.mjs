/* o303 UI test. Run by scripts/test.sh, after smoke.py.
 *
 *   A. ui_core.mjs driving Schwung's REAL page controller and input decoder,
 *      drawn through the device-font harness, against a fake param store
 *      serving the DSP's real contract (grid_hierarchy is dumped by smoke.py
 *      from the built dsp.so).
 *   B. THE PADS ARE TB-3PO'S: TB-3PO's own ui.js (vendor/tb3po/ui, unmodified)
 *      and this module's UI get the same presses over the same sequencer and
 *      must ask it for the same things and light the same pads.
 */
import fs from "node:fs";
import path from "node:path";
import { register } from "node:module";
import { fileURLToPath, pathToFileURL } from "node:url";

register("./hooks.mjs", import.meta.url);

const here = path.dirname(fileURLToPath(import.meta.url));
/* Needs a Schwung checkout for the page engine and the draw harness:
 * SCHWUNG_ROOT, else ../schwung beside this repository. */
const root = process.env.SCHWUNG_ROOT || path.resolve(here, "../../schwung");
const load = (p) => import(pathToFileURL(p).href);
/* TR_SHARED: another copy of shared/ -- e.g. one mirrored from the device,
 * whose page engine can differ from this checkout's. */
const shared = process.env.TR_SHARED || path.join(root, "src/shared");
const pp = (f) => load(path.join(shared, "param_pages", f));
const { createController, LAYOUT_MOVY } = await pp("page_controller.mjs");
const { decodeInput, applyInput } = await pp("page_input.mjs");
const { drawKnobRow, drawHeader, drawBankBar, drawFooter, movyHeaderFor } = await pp("render_page_movy.mjs");
const { isDivable } = await pp("param_meta.mjs");
const { createAnimState } = await pp("anim_state.mjs");
const { fontPrint4x5, fontWidth4x5 } = await pp("font4x5.mjs");
const { drawEnumList } = await pp("enum_list.mjs");
const { listKnobInit, listKnobStep } = await pp("list_knob.mjs");
const { decodeDelta } = await load(path.join(shared, "input_filter.mjs"));
const { createFramebuffer, drawContext } = await load(path.join(root, "tools/param-pages/harness.mjs"));
const { createUi, VIEW_EVERY_TICKS, CLEAR_CONFIRM_MS, LED_REFRESH_TICKS } = await load(path.join(here, "../ui_core.mjs"));
const { createPages, PAGES, ACTION_LABELS } = await load(path.join(here, "../pages.mjs"));
const { LED, padColor, makeSeq } = await load(path.join(here, "../seq_pads.mjs"));
const { FakeSeq } = await load(path.join(here, "fake_seq.mjs"));

let failures = 0;
function check(cond, msg) {
    if (!cond) { failures++; console.error("FAIL:", msg); }
}
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);

const manifest = JSON.parse(fs.readFileSync(path.join(here, "../module.json"), "utf8"));
const chainParams = manifest.capabilities.chain_params;
const gridHierarchy = fs.readFileSync(path.join(here, "../build/native/grid_hierarchy.json"), "utf8");

/* ---- the contract passes Schwung's own validator ------------------------------ */
const { validateContract } = await pp("validate_contract.mjs");
const findings = validateContract({ id: manifest.id, hierarchy: JSON.parse(gridHierarchy), chainParams,
                                    capabilities: manifest.capabilities }).findings;
check(findings.every((f) => f.level === "info"), "contract findings: " + JSON.stringify(findings.filter((f) => f.level !== "info")));

/* ---- help.json fits the help viewer: 21 ASCII characters a line --------------- */
(function walk(node) {
    for (const line of node.lines || []) check(line.length <= 21 && /^[\x20-\x7e]*$/.test(line), "help line: " + line);
    for (const child of node.children || []) walk(child);
})(JSON.parse(fs.readFileSync(path.join(here, "../help.json"), "utf8")));

/* ---- a fake of THIS module's DSP: tr_plugin.c's keys over a FakeSeq ------- */
const SEQ_PARAMS = new Set(["length", "direction", "transpose", "current_bank"]);
function makeStore() {
    const seq = new FakeSeq();
    const values = {};
    for (const p of chainParams)
        values[p.key] = p.options_as_string ? p.options[p.default] : String(p.default);
    const s = {
        seq, values, writes: [], reads: [], viewAnswer: undefined,
        get(key) {
            s.reads.push(key);
            if (key === "ui_hierarchy") return "";
            if (key === "grid_hierarchy") return gridHierarchy;
            if (key === "chain_params") return JSON.stringify(chainParams);
            if (key === "seq_view") return s.viewAnswer !== undefined ? s.viewAnswer : seq.view();
            if (key === "length") return String(seq.length);
            if (key === "direction") return String(seq.direction);
            if (key === "transpose") return String(seq.transpose / 12 + 4);
            if (key === "current_bank") return String(seq.bank + 1);
            return key in values ? values[key] : "";
        },
        set(key, value) {
            const v = String(value);
            s.writes.push([key, v]);
            if (key === "transpose") seq.set(key, String((Number(v) - 4) * 12));   /* an option index on this wire */
            else if (!(key in values) || SEQ_PARAMS.has(key)) seq.set(key, v);
            else values[key] = v;
            return true;
        },
    };
    return s;
}

function makeUi(store, env) {
    const fb = createFramebuffer();
    const ctx = drawContext(fb);
    const ui = createUi({
        createController, decodeInput, applyInput, decodeDelta, layoutGrid: LAYOUT_MOVY,
        listKnobInit, listKnobStep, drawEnumList,
        pages: createPages({ drawKnobRow, drawHeader, drawBankBar, drawFooter, movyHeaderFor, isDivable,
                             createAnimState, fontPrint4x5, fontWidth4x5 }),
        overlay: { show: (a, b) => { env.overlays.push([a, b]); }, tick: () => {}, draw: () => {} },
        getParam: (k) => store.get(k),
        setParam: (k, v) => store.set(k, v),
        draw: ctx,
        clear: () => fb.clearScreen(),
        now: () => env.clock,
        /* The device's shift: shared memory, never a CC (shadow_get_shift_held). */
        shiftHeld: () => env.shift,
        displayOn: () => env.shown,
        padBlock: (on) => { env.padBlocked = on; },
        sendLed: (n, col) => { if (env.dropLeds) return false; env.leds[n] = col; env.ledPackets++; return true; },
        /* The shim's cache of the colour MOVE last wrote to each pad. */
        padSnapshot: () => ({ ...env.moveLeds }),
    });
    return { ui, fb };
}

/* =========================== A. this module's screen ======================= */
const store = makeStore();
const env = { clock: 1000, shift: false, shown: true, padBlocked: false, leds: {}, ledPackets: 0, overlays: [], dropLeds: false,
              moveLeds: { "68": 5, "99": 7 } };
const { ui, fb } = makeUi(store, env);
const ticks = (n) => { for (let i = 0; i < n; i++) { env.clock += 17; ui.tick(); } };
const midi = (...d) => ui.onMidi(d);
const pad = (n) => { midi(0x90, n, 100); midi(0x80, n, 0); };
const jog = (n) => { for (let i = 0; i < Math.abs(n); i++) midi(0xb0, 14, n > 0 ? 1 : 127); };
const knob = (slot, n) => { for (let i = 0; i < Math.abs(n); i++) midi(0xb0, 71 + slot, n > 0 ? 1 : 127); };
const press = (cc) => { midi(0xb0, cc, 127); midi(0xb0, cc, 0); };
const goTo = (name) => { jog(-20); jog(PAGES.findIndex((p) => p.name === name)); ticks(2); };
const keysShown = () => [0, 1, 2, 3, 4, 5, 6, 7].map((i) => ui.keyForKnob(i));
const seqWrites = () => store.seq.writes;
const padColorAt = (n, flash) => padColor(ui.state.seq, n, { shift: false, flash });
const show = (label) => console.log(`--- ${label}\n` + (fb.toBlocks ? fb.toBlocks() : fb.toAscii()));

ui.init();
ticks(20);
check(!store.reads.includes("ui_hierarchy"), "the grid reads grid_hierarchy, never the empty ui_hierarchy");
check(env.padBlocked === true, "pads taken while the screen is up");

/* ---- the pages, and which key every knob edits on each ------------------- */
check(eq(PAGES.map((p) => p.name), ["Synth", "Steps", "Pattern", "Setup", "Tone", "VCO", "Devilfish", "Pads", "Keys"]), "page set");
check(eq(keysShown(), ["volume", "cutoff", "resonance", "env_mod", "decay", "acc_level", "rat_dist", "rat_filter"]),
      "lands on Synth, KNOB 1 = VOLUME: " + keysShown());
check(fb.countLit() > 200, "Synth draws");
show("Synth");
goTo("Steps");
check(eq(keysShown(), ["density", "accent", "slide", "gate", null, null, null, null]), "Steps: TB-3PO's four, 5-8 inert: " + keysShown());
show("Steps");
goTo("Pattern");
check(eq(keysShown(), ["density", "accent", "slide", "gate", "root", "scale", "length", "octaves"]), "Pattern = TB-3PO's Pattern: " + keysShown());
goTo("Setup");
check(eq(keysShown(), ["direction", "transpose", "current_bank", null, null, null, null, null]), "Setup: " + keysShown());
goTo("Tone");
check(eq(keysShown(), ["waveform", "tuning", "osc_level", "rat_on", "rat_model", "rat_level", "rat_low_cut", "rat_hi_cut"]), "Tone: " + keysShown());
goTo("VCO");
check(eq(keysShown(), ["vco2_level", "vco2_tune", "vco2_fine", "vco2_wave", "vco2_sync", "sync_env", "vco1_level", null]), "VCO: " + keysShown());
show("VCO");
goTo("Devilfish");
check(ui.keyForKnob(0) === "devil_mod_switch" && ui.keyForKnob(6) === "tanh_shaper_drive" && ui.keyForKnob(7) === null, "Devilfish: " + keysShown());
goTo("Pads");
check(keysShown().every((k) => k === null), "Pads: no knob is live");
check(eq(ACTION_LABELS.filter(Boolean), ["GEN", "MUT", "DIR"]), "the pad map names the three action pads");
show("Pads");
goTo("Keys");
show("Keys");
/* every chain_param is reachable from some page */
const reachable = new Set();
for (const p of PAGES) { goTo(p.name); keysShown().forEach((k) => k && reachable.add(k)); }
goTo("Pattern");
[4, 5, 6, 7].forEach((i) => reachable.add(ui.state.controller.keyAt(i)));
check(chainParams.every((p) => reachable.has(p.key)), "a param no page shows: " + chainParams.filter((p) => !reachable.has(p.key)).map((p) => p.key));

/* ---- jog: pages, capped at both ends ------------------------------------- */
jog(-20); check(ui.state.pageIndex === 0, "capped at the first page");
jog(40); check(ui.state.pageIndex === PAGES.length - 1, "capped at the last page");
goTo("Synth");

/* ---- knobs write the key the cell shows ----------------------------------- */
store.writes.length = 0;
midi(0x90, 0, 127); knob(0, -6); ticks(6); midi(0x80, 0, 0);
check(store.writes.length > 0 && store.writes.every(([k]) => k === "volume") && Number(store.values.volume) < 0.75,
      "knob 1 on Synth turns VOLUME down: " + JSON.stringify(store.writes.slice(-2)));
store.writes.length = 0;
knob(6, 5); ticks(6);
check(store.writes.some(([k]) => k === "rat_dist"), "knob 7 on Synth is RAT distortion");
goTo("Pads");
store.writes.length = 0;
knob(0, 5); ticks(6);
check(store.writes.length === 0, "a knob on the Pads page writes nothing");
goTo("Steps");
store.writes.length = 0;
knob(5, 5); knob(1, 5); ticks(6);
check(store.writes.length > 0 && store.writes.every(([k]) => k === "accent"), "Steps: knob 2 = accent prob, knob 6 inert: " + JSON.stringify(store.writes));

/* ---- pads: steps, banks, actions ------------------------------------------- */
const before = store.seq.steps[0];
seqWrites().length = 0;
pad(92);                                             /* top-left = step 1 */
check(eq(seqWrites(), [["set_step", "0:" + ((before + 1) & 3)]]), "tap cycles the step: " + JSON.stringify(seqWrites()));
midi(0x90, 92, 100); midi(0x90, 92, 100); midi(0x80, 92, 0);
check(seqWrites().length === 2, "a held pad fires once");
seqWrites().length = 0;
pad(84);                                             /* row 2 = step 9 */
check(seqWrites()[0][1].startsWith("8:"), "second row starts at step 9");
seqWrites().length = 0;
env.shift = true; pad(78); env.shift = false;        /* Shift + bank 3 */
check(eq(seqWrites(), [["store_bank", "2"]]) && eq(env.overlays.at(-1), ["Bank 3", "saved"]), "Shift+bank pad saves");
ticks(4);
seqWrites().length = 0;
pad(78);
check(eq(seqWrites(), [["recall_bank_now", "2"]]) && eq(env.overlays.at(-1), ["Bank 3", "recalled"]), "stopped: recall is immediate");
store.seq.running = 1; ticks(4);
seqWrites().length = 0;
pad(78);
check(eq(seqWrites(), [["recall_bank", "2"]]) && eq(env.overlays.at(-1), ["Bank 3", "queued"]), "playing: recall is queued to the bar");
seqWrites().length = 0;
pad(68); pad(69); pad(70);
check(eq(seqWrites(), [["generate", "0"], ["mutate", "1"], ["direction", "1"]]), "NEW / MUT / DIR: " + JSON.stringify(seqWrites()));
/* NEW at Length 8 is the same press, the same write: the DSP decides how to
 * generate (tb_engine.c), so the surface does not change. */
store.seq.set("length", 8); ticks(VIEW_EVERY_TICKS + 1);
seqWrites().length = 0;
pad(68);
check(eq(seqWrites(), [["generate", "0"]]) && eq(env.overlays.at(-1), ["Pattern", "regenerated"]),
      "NEW at 8 steps: same pad, same write, same overlay: " + JSON.stringify(seqWrites()));
store.seq.set("length", 16); ticks(VIEW_EVERY_TICKS + 1);
check(ui.state.controller.state.values.direction === "1", "the DIR pad is told to the grid's Direction cell");
seqWrites().length = 0;
for (const n of [71, 72, 73, 74, 75]) pad(n);
check(seqWrites().length === 0, "pads 4-8 of the action row do nothing (TB-3PO's Ch-/Ch+ have no meaning here)");
ticks(8);
check([env.leds[78], padColorAt(78, 1)].includes(LED.YELLOW), "a queued bank flashes yellow");
store.seq.barLine(); store.seq.running = 0;          /* the bar line: the queued recall lands */

/* ---- pad LEDs --------------------------------------------------------------- */
store.seq.steps[0] = 1; store.seq.steps[1] = 2; store.seq.steps[2] = 3; store.seq.steps[3] = 0;
ticks(4);
check(env.leds[92] === LED.WHITE && env.leds[93] === LED.RED && env.leds[94] === LED.BLUE && env.leds[95] === LED.OFF,
      "white note / red accent / blue slide / dark rest");
check(env.leds[68] === LED.TEAL && env.leds[69] === LED.INDIGO && env.leds[70] === LED.ORANGE && env.leds[71] === LED.OFF, "action row colours");
check(env.leds[78] === LED.PURPLE, "current bank purple");
store.seq.running = 1; store.seq.position = 1; ticks(4);
check(env.leds[93] === LED.YELLOW, "the playing step is yellow");
store.seq.running = 0; ticks(4);
const quiet = env.ledPackets; ticks(LED_REFRESH_TICKS);
check(env.ledPackets - quiet === 32, "nothing changed: one full repaint per refresh period, nothing else: " + (env.ledPackets - quiet));

/* ---- Move paints its own track on our pads: taken back at once ------------- */
{
    /* A Drum Rack hit: Move lights pad 92 and pad 70 in its own colours. */
    const tick0 = ui.state.tick;
    const nextRefresh = Math.ceil((tick0 + 1) / LED_REFRESH_TICKS) * LED_REFRESH_TICKS;
    if (nextRefresh - tick0 < 3) ticks(3);           /* keep the periodic repaint out of this frame */
    const ours92 = env.leds[92], ours70 = env.leds[70];
    env.moveLeds["92"] = 21; env.leds[92] = 21;
    env.moveLeds["70"] = 3;  env.leds[70] = 3;
    const sent = env.ledPackets;
    ticks(1);
    check(env.leds[92] === ours92 && env.leds[70] === ours70, `repainted in the next frame: 92=${env.leds[92]} 70=${env.leds[70]}`);
    check(env.ledPackets - sent === 2, "only the two pads Move drew: " + (env.ledPackets - sent));
    /* Move draws pad 92 again in the SAME colour: invisible to the cache, so
     * the periodic repaint is what takes it back. */
    env.leds[92] = 21;
    ticks(LED_REFRESH_TICKS);
    check(env.leds[92] === ours92, "a same-colour repaint by Move is undone within one refresh period");
}
env.dropLeds = true; store.seq.steps[3] = 1; ticks(4);
env.dropLeds = false; ticks(1);
check(env.leds[95] === LED.WHITE, "a packet the host refused is retried, not cached");

/* ---- buttons: + - < > Undo, and CLEAR ----------------------------------------- */
store.writes.length = 0;
press(55);
check(eq(store.writes, [["transpose", "5"]]) && store.seq.transpose === 12 && eq(env.overlays.at(-1), ["Transpose", "1 oct"]),
      "+ is one octave up: " + JSON.stringify(store.writes));
check(ui.state.controller.state.values.transpose === "5", "and the grid's Transpose cell follows");
press(54); press(54);
check(store.seq.transpose === -12, "- is one octave down");
for (let i = 0; i < 9; i++) press(54);
check(store.seq.transpose === -48, "clamped at -4 octaves");
for (let i = 0; i < 4; i++) press(55);
seqWrites().length = 0;
press(56);
check(eq(seqWrites(), [["undo", "1"]]), "Undo");
seqWrites().length = 0;
press(119);
check(seqWrites().length === 0 && eq(env.overlays.at(-1), ["Clear?", "X again"]), "one X asks");
env.clock += CLEAR_CONFIRM_MS + 50;
press(119);
check(seqWrites().length === 0, "a second X after the prompt is gone asks again");
press(119);
check(eq(seqWrites(), [["clear", "1"]]) && eq(env.overlays.at(-1), ["Pattern", "cleared"]), "X X clears");
/* < > only with more than 16 steps */
press(63); check(ui.state.seq.stepView === 0, "16 steps: no second window");
store.seq.set("length", 32); ticks(VIEW_EVERY_TICKS + 1);
check(ui.state.controller.state.values.length === "32", "a length the DSP changed by itself reaches the grid");
press(63); check(ui.state.seq.stepView === 1, "> shows steps 17-32");
seqWrites().length = 0;
pad(92);
check(seqWrites()[0][1].startsWith("16:"), "and the pads edit that window: " + JSON.stringify(seqWrites()));
show("Steps 17-32");
press(63); check(ui.state.seq.stepView === 1, "capped");
store.seq.set("length", 16); ticks(VIEW_EVERY_TICKS + 1);
check(ui.state.seq.stepView === 0, "shrinking the pattern brings the window back");

/* ---- a knob-written value is not fought by a poll one write behind ---------- */
goTo("Pattern");
store.viewAnswer = store.seq.view();                 /* the DSP's answer, frozen: "a write behind" */
midi(0x90, 6, 127); knob(6, 8); ticks(1);
const shownLen = ui.state.controller.state.values.length;
ticks(6);
check(ui.state.controller.state.values.length === shownLen, "the grid keeps what the knob set: " + shownLen);
midi(0x80, 6, 0);
store.viewAnswer = undefined; ticks(4);

/* ---- a read that did not complete changes nothing ---------------------------- */
const pos = ui.state.seq.position;
store.seq.position = 7;
for (const bad of [null, "", "garbage", "120;1,0"]) { store.viewAnswer = bad; ticks(4); }
check(ui.state.seq.position === pos, "null / empty / malformed seq_view keeps the last answer");
store.viewAnswer = undefined; ticks(4);
check(ui.state.seq.position === 7, "and the next good one lands");

/* ---- the option list: hold an enum knob, click ---------------------------------- */
goTo("Pattern");
check(ui.handleBack() === false, "nothing open: Back is the host's");
midi(0x90, 4, 127);                                  /* hold ROOT */
midi(0xb0, 3, 127);
check(ui.state.picker && ui.state.picker.options.length === 12, "click on a held enum opens its options");
ticks(1); show("Root options");
store.writes.length = 0;
const stepWrites = seqWrites().length;
pad(92);
check(seqWrites().length === stepWrites, "the list owns the surface: a pad under it does nothing");
check(ui.handleBack() === true && ui.state.picker === null && store.writes.length === 0, "Back cancels, nothing written");
midi(0xb0, 3, 127);
jog(-20); jog(2);
midi(0xb0, 3, 127);
ticks(3);
check(store.values.root === "2" && ui.state.picker === null, "click sets the highlighted option: root=" + store.values.root);
midi(0x80, 4, 0);
midi(0xb0, 3, 127);
check(ui.state.picker === null, "a click with no knob held opens nothing");

/* ---- leaving: the pads go back to Move ------------------------------------------- */
env.shown = false; ticks(1);
check(env.padBlocked === false, "pads released when the display closes");
check(env.leds[68] === 5 && env.leds[99] === 7 && env.leds[80] === 0 && env.leds[92] === 21 && env.leds[70] === 3,
      "Move's own pad colours restored (the latest it drew)");
const frozen = seqWrites().length;
pad(92);
check(seqWrites().length === frozen, "a pad while hidden is Move's, not ours");
env.shown = true; ticks(2);
check(env.padBlocked === true && env.leds[68] === LED.TEAL, "taken again, and repainted, on return");

/* ====================== B. the pads are TB-3PO's ============================ */
/* TB-3PO's ui.js reads device globals, so they exist before it is imported. */
const ref = { seq: new FakeSeq(), leds: {} };
Object.assign(globalThis, {
    print: () => {}, fill_rect: () => {}, draw_rect: () => {}, draw_line: () => {}, set_pixel: () => {},
    text_width: (t) => String(t).length * 6, clear_screen: () => {},
    host_module_set_param: (k, v) => {
        if (k === "a.channel" || k === "b.channel" || k === "active_slot" || k.startsWith("b.")) return;
        ref.seq.set(k.replace(/^a\./, ""), v);
    },
    host_module_get_param: (k) => {
        const q = ref.seq;
        switch (k) {
            case "a.position": return String(q.position);
            case "a.pattern": return q.pattern();
            case "a.current_bank": return String(q.bank);
            case "a.pending_recall": return String(q.pending);
            case "a.bank_filled": return q.filled();
            case "running": return String(q.running);
            case "sync_source": return "EXT";
            default: return "";
        }
    },
    shadow_get_param: () => "",
    shadow_send_midi_to_dsp: () => {},
    move_midi_internal_send: (pkt) => { if (pkt[1] === 0x90) ref.leds[pkt[2]] = pkt[3]; return true; },
});
await load(path.join(here, "../vendor/tb3po/ui/ui.js"));
const refMidi = (...d) => globalThis.onMidiMessageInternal(Uint8Array.from(d));

const mine = makeStore();
const menv = { clock: 5000, shift: false, shown: true, padBlocked: false, leds: {}, ledPackets: 0, overlays: [], dropLeds: false,
               moveLeds: {} };
const m = makeUi(mine, menv).ui;
globalThis.init();
m.init();

/* TB-3PO's writes are compared as TB-3PO spells them; this module's
 * `transpose` rides the wire as an option index, converted back in the store. */
const bothTicks = (n, compare) => {
    for (let i = 0; i < n; i++) {
        menv.clock += 17;
        globalThis.tick();
        m.tick();
        if (compare && i >= n - 9) sameLeds(compare + " (tick -" + (n - 1 - i) + ")");
    }
};
const CH_PADS = new Set([71, 72]);                    /* TB-3PO's Ch- / Ch+: not carried over */
function sameLeds(what) {
    const diff = [];
    for (let n = 68; n <= 99; n++)
        if (!CH_PADS.has(n) && ref.leds[n] !== menv.leds[n]) diff.push(`${n}: tb3po ${ref.leds[n]} / here ${menv.leds[n]}`);
    check(diff.length === 0, `pad LEDs differ after ${what}: ${diff.join(", ")}`);
}
function sameSeq(what) {
    check(eq(ref.seq.writes, mine.seq.writes),
          `sequencer writes differ after ${what}:\n   tb3po ${JSON.stringify(ref.seq.writes.slice(-4))}\n   here  ${JSON.stringify(mine.seq.writes.slice(-4))}`);
    check(eq(ref.seq.steps, mine.seq.steps) && ref.seq.length === mine.seq.length && ref.seq.bank === mine.seq.bank,
          "sequencer state differs after " + what);
}
const bothPad = (n) => { refMidi(0x90, n, 100); refMidi(0x80, n, 0); m.onMidi([0x90, n, 100]); m.onMidi([0x80, n, 0]); };
const bothCc = (cc) => { refMidi(0xb0, cc, 127); refMidi(0xb0, cc, 0); m.onMidi([0xb0, cc, 127]); m.onMidi([0xb0, cc, 0]); };
const bothShift = (on) => { refMidi(0xb0, 49, on ? 127 : 0); menv.shift = on; };   /* a CC to the tool, shared memory to a chain UI */
const bothWorld = (fn) => { fn(ref.seq); fn(mine.seq); };

bothTicks(40, "start");
sameSeq("start");
let script = 0;
const step = (what, fn) => { fn(); bothTicks(40, what); sameSeq(what); script++; };

/* every step pad of both rows, around the whole cycle */
for (let round = 0; round < 4; round++)
    for (const n of [92, 93, 94, 95, 96, 97, 98, 99, 84, 85, 86, 87, 88, 89, 90, 91])
        step(`step pad ${n} round ${round}`, () => bothPad(n));
/* the action row */
step("NEW", () => bothPad(68));
step("MUT", () => bothPad(69));
for (let i = 0; i < 5; i++) step("DIR " + i, () => bothPad(70));
step("unused action pads", () => { bothPad(73); bothPad(74); bothPad(75); });
/* banks: save, recall stopped, recall queued while playing, the bar line */
step("Shift held", () => bothShift(true));
step("save bank 1", () => bothPad(76));
step("save bank 5", () => bothPad(80));
step("Shift released", () => bothShift(false));
step("NEW again", () => bothPad(68));
step("recall bank 1, stopped", () => bothPad(76));
step("recall an empty bank", () => bothPad(79));
step("Play", () => bothWorld((q) => { q.running = 1; q.position = 3; }));
step("playhead moves", () => bothWorld((q) => { q.position = 9; }));
step("recall bank 5, playing: queued", () => bothPad(80));
step("the bar line", () => bothWorld((q) => { q.barLine(); q.position = 0; }));
step("Shift+save while playing", () => { bothShift(true); bothPad(83); bothShift(false); });
step("Stop", () => bothWorld((q) => { q.running = 0; }));
/* buttons */
step("+", () => bothCc(55));
step("- -", () => { bothCc(54); bothCc(54); });
for (let i = 0; i < 6; i++) step("- to the floor " + i, () => bothCc(54));
step("Undo", () => bothCc(56));
/* a 32-step pattern: < > move the window the pads edit and light */
step("length 32", () => bothWorld((q) => { q.length = 32; }));
step(">", () => bothCc(63));
step("pads in window 2", () => { bothPad(92); bothPad(99); bothPad(91); });
step("> again (capped)", () => bothCc(63));
step("playhead in window 2", () => bothWorld((q) => { q.running = 1; q.position = 20; }));
step("<", () => bothCc(62));
step("< again (capped)", () => bothCc(62));
step("length 24: the last pad row is half dark", () => bothWorld((q) => { q.length = 24; q.running = 0; }));
step("> at 24", () => bothCc(63));
step("back to 16", () => bothWorld((q) => { q.length = 16; }));
check(script > 100, "the comparison ran");
check(ref.seq.writes.length > 80, "and exercised the sequencer: " + ref.seq.writes.length + " writes");

/* TB-3PO's two channel pads: its own writes for them never reach this module's sequencer. */
const n0 = mine.seq.writes.length;
m.onMidi([0x90, 71, 100]); m.onMidi([0x80, 71, 0]); m.onMidi([0x90, 72, 100]); m.onMidi([0x80, 72, 0]);
check(mine.seq.writes.length === n0 && padColor(makeSeq(), 71) === LED.OFF && padColor(makeSeq(), 72) === LED.OFF,
      "Ch- / Ch+ are dark and inert here");

if (failures) { console.error(`${failures} UI check(s) failed`); process.exit(1); }
console.log(`PASS: ui (${script} gestures matched TB-3PO's ui.js, ${ref.seq.writes.length} sequencer writes)`);
