/*
 * o303 - page composition: header, bank bar, body, footer.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Hiro Ikezawa
 *
 * TB-3PO's screen (vendor/tb3po/ui/pages.mjs + pad_map.mjs), with the page
 * set of this module. The arrangement is TB-3PO's and so are the numbers:
 *
 *   - Schwung's page controller draws the BODY of a knob page (every widget,
 *     label, enum square and graphic) into a rect, with its own chrome off;
 *   - this file draws the header (which becomes "<param>  <value>" while a
 *     knob is held), the bank bar counting ALL the pages, and the footer;
 *   - a knob page's footer carries the step lane, following the playhead;
 *   - Steps is the big lane, edited by the pads, over one knob row.
 *
 * The lanes themselves are TB-3PO's lane.mjs, unmodified.
 *
 * Every drawing dependency is injected (ui_chain.js binds the device's shared
 * library, the tests bind this checkout's), so nothing here names a path.
 */
import { drawBigLane, drawMiniLane, windowFor } from "./vendor/tb3po/ui/lane.mjs";

export const TITLE = "o303";

/*
 * The pages, in jog order. `first` names the first knob of the controller
 * page that backs a page -- the controller plans its own list from the
 * hierarchy the DSP serves, and matching on a key rather than a position
 * means a re-ordered hierarchy moves the page instead of silently swapping
 * what the knobs do.
 *
 * Steps borrows Pattern's first row (Density / Accent / Slide / Gate), the
 * same four in the same positions, exactly as TB-3PO's PERFORM page does.
 */
export const PAGES = Object.freeze([
    { name: "Synth",     kind: "knobs",   first: "volume" },
    { name: "Steps",     kind: "perform", first: "density" },
    { name: "Pattern",   kind: "knobs",   first: "density" },
    { name: "Setup",     kind: "knobs",   first: "direction" },
    { name: "Tone",      kind: "knobs",   first: "waveform" },
    { name: "VCO",       kind: "knobs",   first: "vco2_level" },
    { name: "Devilfish", kind: "knobs",   first: "devil_mod_switch" },
    { name: "Pads",      kind: "pads" },
    { name: "Keys",      kind: "keys" },
]);

/* Steps' live knobs: Pattern's row 0. */
export const PERFORM_KNOBS = 4;

/* Where the controller may draw, and what it may draw there: all three of
 * its chrome bands are off. y: 8 puts the two knob rows at [9,24] and
 * [33,48] -- see TB-3PO's pages.mjs for why it is not 9. */
export const BANDS = Object.freeze({ header: false, bank: false, footer: false });
export const BODY = Object.freeze({ x: 0, y: 8, w: 128, h: 49 });

const FOOTER_BAND = { y: 57, h: 7 };

/* ---- the pad map (TB-3PO's pad_map.mjs geometry) --------------------------- */
const MAP_TOP = 13, CELL_W = 16, BAND_H = 9, ACT_H = 13;
const ACT_Y = MAP_TOP + 3 * BAND_H;
/* A cell is 16px and its divider owns one column, so a label has 14: "GEN"
 * measures 14 where "NEW" measures 15. Pads 4 and 5 are TB-3PO's Ch- / Ch+,
 * unused here (seq_pads.mjs). */
export const ACTION_LABELS = ["GEN", "MUT", "DIR", "", "", "", "", ""];

export const KEY_ROWS = [
    [["+ -", "octave"],     ["< >", "step pg"]],
    [["x x", "clear"],      ["undo", "last op"]],
    [["shift+pad", "save bank 1-8"]],
    [["jog", "pages"],      ["play", "start"]],
];
const KEY_COL_X = [2, 66], KEY_W = 26, KEY_WIDE_W = 42;

export function createPages(deps) {
    const { drawKnobRow, drawHeader, drawBankBar, drawFooter, movyHeaderFor, isDivable,
            createAnimState, fontPrint4x5, fontWidth4x5 } = deps;
    const anim = createAnimState();
    const caps = (s) => String(s).toUpperCase();
    const bandY = (row) => MAP_TOP + row * BAND_H;

    /** One row-wide label, knocked out of the grid it sits on. */
    function band(ctx, row, text, inverted) {
        const y = bandY(row);
        const t = caps(text), tw = fontWidth4x5(t);
        const tx = Math.floor((128 - tw) / 2), ty = y + 2;
        if (inverted) ctx.fillRect(0, y + 1, 128, BAND_H - 2, 1);
        ctx.fillRect(tx - 3, ty - 1, tw + 6, 7, inverted ? 1 : 0);
        fontPrint4x5(ctx, tx, ty, t, inverted ? 0 : 1);
    }

    function drawPadsPage(ctx, view) {
        for (let r = 0; r < 3; r++) {
            const y = bandY(r);
            ctx.fillRect(0, y, 128, 1, 1);
            for (let c = 0; c <= 8; c++) ctx.fillRect(Math.min(c * CELL_W, 127), y, 1, BAND_H, 1);
        }
        ctx.fillRect(0, ACT_Y, 128, 1, 1);
        ctx.fillRect(0, ACT_Y + ACT_H, 128, 1, 1);
        for (let c = 0; c <= 8; c++) ctx.fillRect(Math.min(c * CELL_W, 127), ACT_Y, 1, ACT_H + 1, 1);
        /* The map names the window the top two rows are editing, and redraws
         * while Shift is held: live state cannot drift from what the pads do. */
        const base = view.stepView * 16;
        band(ctx, 0, "steps " + (base + 1) + "-" + (base + 8), false);
        band(ctx, 1, "steps " + (base + 9) + "-" + (base + 16), false);
        band(ctx, 2, view.shiftHeld ? "save to bank 1-8" : "banks 1-8", !!view.shiftHeld);
        ACTION_LABELS.forEach((text, col) => {
            if (!text) return;
            const t = caps(text), tw = fontWidth4x5(t);
            fontPrint4x5(ctx, col * CELL_W + 1 + Math.floor((CELL_W - 1 - tw) / 2), ACT_Y + 4, t, 1);
        });
    }

    function drawKeysPage(ctx) {
        KEY_ROWS.forEach((row, i) => {
            const y = 15 + i * 10;
            row.forEach((pair, c) => {
                fontPrint4x5(ctx, KEY_COL_X[c], y, caps(pair[0]), 1);
                fontPrint4x5(ctx, KEY_COL_X[c] + (row.length === 1 ? KEY_WIDE_W : KEY_W), y, caps(pair[1]), 1);
            });
        });
    }

    /** The lane in the footer band of a knob page. */
    function drawFooterLane(ctx, view, gridPage) {
        /* A cell the jog click would open is announced in the footer while
         * its knob is held, and that costs the lane: they take turns rather
         * than fight over 82 pixels. Derived from the page that dispatches the
         * click, so the footer cannot promise a door the click will not open. */
        const meta = view.ctl && view.ctl.metaIndex;
        const heldKey = (view.touched >= 0 && gridPage && gridPage.keys) ? gridPage.keys[view.touched] : null;
        if (heldKey && meta && isDivable(meta.getOrGuess(heldKey))) {
            drawFooter(ctx, [["JOG", "PAGE"], ["CLK", "OPEN"]]);
            return;
        }
        drawFooter(ctx, [["JOG", "PAGE"]]);
        ctx.fillRect(46, 56, 82, 8, 0);
        /* The window the MUSIC is in: nothing here is editable, and there is
         * no room to say which window it is. */
        drawMiniLane(null, ctx, { steps: view.steps, position: view.position, stepBase: windowFor(view.position) },
                     { x: 48, y: FOOTER_BAND.y, w: 80, h: FOOTER_BAND.h });
    }

    /**
     * @param {object} view
     *   pageIndex, steps, position, stepView, bpm,
     *   shiftHeld, touched (held knob 0-7 or -1), ctl, nowMs
     */
    function drawPage(ctx, view) {
        const page = PAGES[view.pageIndex];
        const ctl = view.ctl;
        const planned = (ctl && ctl.page) || null;
        const gridPage = page.kind === "perform"
            ? { name: page.name, keys: planned && planned.keys ? planned.keys.slice(0, PERFORM_KNOBS) : [] }
            : page.kind === "knobs" ? planned : null;
        /* Steps names the window: "Steps 1-16", "Steps 17-32". */
        const pageName = page.kind === "perform"
            ? page.name + " " + (view.stepView * 16 + 1) + "-" + (view.stepView * 16 + 16)
            : page.name;
        /* The tempo rides on the pages with room for it. */
        const title = page.kind === "knobs" ? TITLE : TITLE + " - " + (view.bpm | 0);
        const header = movyHeaderFor({
            page: gridPage,
            metaIndex: ctl && ctl.metaIndex,
            values: (ctl && ctl.state.values) || {},
            touched: view.touched,
            title,
            /* The planner calls the root page "Main" whatever is declared;
             * this module's own name is the only one that reaches the screen. */
            pageLabel: pageName,
        });
        drawHeader(ctx, header.left, header.right, header.inverted);
        drawBankBar(ctx, view.pageIndex, PAGES.length, null);

        if (page.kind === "knobs") {
            if (ctl) ctl.render(ctx, { title: TITLE, rect: BODY, bands: BANDS });
            drawFooterLane(ctx, view, gridPage);
            return;
        }
        if (page.kind === "perform") {
            /* The window the USER chose, because the pads edit what is shown. */
            drawBigLane(null, ctx, { steps: view.steps, position: view.position, stepBase: view.stepView * 16 },
                        { x: 0, y: 12, w: 128, h: 22 });
            /* A genuine grid row, from the controller's own Pattern page: the
             * same call the grid makes for its row 0, same metadata, same
             * values, so holding a knob here gives the same touch strip. */
            if (ctl && ctl.metaIndex) {
                drawKnobRow(ctx, {
                    page: gridPage,
                    metaIndex: ctl.metaIndex,
                    values: ctl.state.values || {},
                    anim, nowMs: view.nowMs,
                    touched: view.touched,
                    touchedSlots: view.touched >= 0 ? [view.touched] : [],
                    modulated: () => false,
                }, 0, 33, 48);
            }
            drawFooter(ctx, [["PAD", "STEP"], ["JOG", "PAGE"]]);
            return;
        }
        if (page.kind === "pads") {
            drawPadsPage(ctx, view);
            drawFooter(ctx, view.shiftHeld ? [["SHIFT", "HELD"], ["PAD", "SAVE"]]
                                           : [["TAP", "REST>NOTE>ACC>SLIDE"]]);
            return;
        }
        drawKeysPage(ctx);
        drawFooter(ctx, [["JOG", "PAGE"], ["BACK", "EXIT"]]);
    }

    return { drawPage };
}
