/*
 * A sequencer for the UI tests: enough of TB-3PO's DSP (the keys both UIs
 * write, the keys both UIs read) to tell whether two UIs ask it for the same
 * things. Deterministic; it plays nothing.
 */
export class FakeSeq {
    constructor() {
        this.steps = new Array(32).fill(0).map((_, i) => (i * 7 + 1) & 3);
        this.length = 16; this.direction = 0; this.transpose = 0;
        this.position = 0; this.running = 0;
        this.bank = 0; this.pending = -1;
        this.banks = new Array(8).fill(null);
        this.undo = null;
        this.gen = 0;
        this.writes = [];              /* [key, value] exactly as received */
    }
    snapshot() { this.undo = this.steps.slice(); }
    /** TB-3PO's own key names and value formats (no slot prefix). */
    set(key, value) {
        const v = String(value);
        this.writes.push([key, v]);
        switch (key) {
            case "set_step": { const [i, k] = v.split(":").map(Number); this.steps[i] = k & 3; break; }
            case "store_bank": this.banks[+v] = { steps: this.steps.slice(), length: this.length }; this.bank = +v; break;
            case "recall_bank": if (this.banks[+v]) this.pending = +v; break;
            case "recall_bank_now": if (this.banks[+v]) this.apply(+v); break;
            case "generate": this.snapshot(); this.gen++; this.steps = this.steps.map((_, i) => (i * 5 + this.gen * 3) & 3); break;
            case "mutate": this.snapshot(); this.steps = this.steps.map((s, i) => (i % 4 === 1 ? (s + 1) & 3 : s)); break;
            case "clear": this.snapshot(); this.steps.fill(0); this.steps[0] = 1; break;
            case "undo": if (this.undo) this.steps = this.undo.slice(); break;
            case "direction": this.direction = +v & 3; break;
            case "transpose": this.transpose = +v; break;      /* semitones */
            case "length": this.length = +v; break;
            default: break;
        }
    }
    apply(b) { this.steps = this.banks[b].steps.slice(); this.length = this.banks[b].length; this.bank = b; this.pending = -1; }
    /** The bar line: a queued recall lands. */
    barLine() { if (this.pending >= 0) this.apply(this.pending); }
    filled() { return this.banks.map((b) => (b ? "1" : "0")).join(""); }
    /** tr_plugin.c's `seq_view`. */
    view(bpm = 120) {
        return `${bpm};${this.position},${this.running},${this.bank},${this.pending},${this.filled()},` +
               `${this.length},${this.direction},${this.transpose},${this.undo ? 1 : 0}|` +
               this.steps.slice(0, this.length).join("");
    }
    /** tb3po.c's `pattern`: "len|steps|degs|octs". */
    pattern() {
        const z = new Array(this.length).fill(0).join(",");
        return `${this.length}|${this.steps.slice(0, this.length).join(",")}|${z}|${z}`;
    }
}
