#!/usr/bin/env python3
"""dlopen the real dsp.so through the v2 ABI and drive it as the chain host
does: Move's transport arrives as MIDI realtime (0xFA, 0xF8 x24 per beat, 0xFC)
before the block it falls in."""
import ctypes as c
import json
import math
import pathlib
import sys

LIB = sys.argv[1]
BLOCK = 128
SR = 44100
MODULE = pathlib.Path(__file__).resolve().parent.parent

BPMFN = c.CFUNCTYPE(c.c_float)
CLOCKFN = c.CFUNCTYPE(c.c_int)


class Host(c.Structure):
    _fields_ = [("api_version", c.c_uint32), ("sample_rate", c.c_int),
                ("frames_per_block", c.c_int), ("mapped_memory", c.c_void_p),
                ("audio_out_offset", c.c_int), ("audio_in_offset", c.c_int),
                ("log", c.c_void_p), ("midi_send_internal", c.c_void_p),
                ("midi_send_external", c.c_void_p), ("get_clock_status", CLOCKFN),
                ("mod_emit_value", c.c_void_p), ("mod_clear_source", c.c_void_p),
                ("mod_host_ctx", c.c_void_p), ("get_bpm", BPMFN),
                ("midi_inject_to_move", c.c_void_p), ("slot_recv_channel", c.c_void_p),
                ("get_beat_position", c.c_void_p), ("reserved", c.c_void_p * 8)]


assert Host.get_beat_position.offset == 112 and Host.reserved.offset == 120
STOPPED, RUNNING = 1, 2
transport = {"bpm": 120.0, "status": STOPPED}
bpm_cb = BPMFN(lambda: transport["bpm"])
clock_cb = CLOCKFN(lambda: transport["status"])


class API(c.Structure):
    _fields_ = [("version", c.c_uint32)] + [(k, c.c_void_p) for k in
               ("create", "destroy", "midi", "set", "get", "error", "render")]


lib = c.CDLL(LIB)
lib.move_plugin_init_v2.restype = c.POINTER(API)
lib.move_plugin_init_v2.argtypes = [c.c_void_p]
bad = Host(api_version=1, sample_rate=48000, frames_per_block=BLOCK)
assert not lib.move_plugin_init_v2(c.byref(bad)), "must refuse a 48 kHz host"
host = Host(api_version=1, sample_rate=SR, frames_per_block=BLOCK, get_bpm=bpm_cb,
            get_clock_status=clock_cb)
api = lib.move_plugin_init_v2(c.byref(host)).contents
assert api.version == 2

create = c.CFUNCTYPE(c.c_void_p, c.c_char_p, c.c_char_p)(api.create)
destroy = c.CFUNCTYPE(None, c.c_void_p)(api.destroy)
midi = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(c.c_uint8), c.c_int, c.c_int)(api.midi)
set_param = c.CFUNCTYPE(None, c.c_void_p, c.c_char_p, c.c_char_p)(api.set)
get_param = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_char_p, c.c_char_p, c.c_int)(api.get)
render = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(c.c_int16), c.c_int)(api.render)

buf = (c.c_int16 * (BLOCK * 2))()
p = create(b".", None)
assert p


def read(key, size=65536, inst=None):
    out = c.create_string_buffer(size)
    n = get_param(inst or p, key.encode(), out, size)
    return None if n < 0 else out.value.decode()


def write(key, value, inst=None):
    set_param(inst or p, key.encode(), str(value).encode())


def rt(status, inst=None):
    midi(inst or p, (c.c_uint8 * 1)(status), 1, 2)


class Clock:
    """Move's clock: 24 pulses per quarter, each delivered before the block
    its instant falls in (the shim dispatches MIDI ahead of the render)."""

    def __init__(self, inst=None):
        self.inst, self.playing, self.t, self.next = inst, False, 0.0, 0.0

    def play(self):
        transport["status"] = RUNNING
        rt(0xFA, self.inst)
        self.playing, self.next = True, self.t

    def stop(self):
        transport["status"] = STOPPED
        rt(0xFC, self.inst)
        self.playing = False

    def block(self):
        if self.playing:
            while self.next < self.t + BLOCK:
                rt(0xF8, self.inst)
                self.next += SR * 60.0 / transport["bpm"] / 24.0
        render(self.inst or p, buf, BLOCK)
        self.t += BLOCK
        return list(buf)[0::2]

    def run(self, seconds):
        out = []
        for _ in range(int(seconds * SR / BLOCK)):
            out += self.block()
        return out


def view(inst=None):
    raw = read("seq_view", inst=inst)
    bpm, rest = raw.split(";")
    head, steps = rest.split("|")
    f = head.split(",")
    return {"bpm": int(bpm), "pos": int(f[0]), "running": int(f[1]), "bank": int(f[2]),
            "pending": int(f[3]), "filled": f[4], "length": int(f[5]), "dir": int(f[6]),
            "transpose": int(f[7]), "undo": int(f[8]), "steps": steps}


def rms(x):
    return math.sqrt(sum(v * v for v in x) / max(1, len(x)))


def pattern(kinds, inst=None, degree=0, octave=0):
    """Write a whole pattern: one char per step, 0 rest 1 note 2 accent 3 slide."""
    for i, k in enumerate(kinds):
        write("set_step", f"{i}:{k}:{degree}:{octave}", inst)


clock = Clock()

# --- contract with the shadow UI -------------------------------------------
manifest = json.loads((MODULE / "module.json").read_text())
params = manifest["capabilities"]["chain_params"]
assert json.loads(read("chain_params")) == params
assert read("ui_hierarchy") == "", "empty on purpose: the shadow UI then opens ui_chain.js"
levels = json.loads(read("grid_hierarchy"))["levels"]
# tests/test_ui.mjs plans its pages from what the DSP really serves.
(pathlib.Path(LIB).parent / "grid_hierarchy.json").write_text(read("grid_hierarchy"))
assert levels["root"]["knobs"] == ["volume", "cutoff", "resonance", "env_mod", "decay",
                                   "acc_level", "rat_dist", "rat_filter"], "knob 1 is VOLUME"
assert levels["pattern"]["knobs"] == ["density", "accent", "slide", "gate", "root", "scale",
                                      "length", "octaves"], "TB-3PO's own Pattern page"
for prm in params:
    got = read(prm["key"])
    if prm["type"] == "float":
        assert abs(float(got) - prm["default"]) < 1e-3, (prm["key"], got)
    elif prm.get("options_as_string"):
        assert got == prm["options"][prm["default"]], (prm["key"], got)
    else:
        assert int(got) == prm["default"], (prm["key"], got)
assert read("no_such_key") is None
assert manifest["capabilities"]["claims_ccs"] == [54, 55, 56, 62, 63, 119]

# --- enums: an index, or the option's name; `length` speaks step counts -------
write("scale", "Phrygian"); assert read("scale") == "1"
write("scale", 4); assert read("scale") == "4"
write("scale", 0)
write("length", "32"); assert read("length") == "32" and view()["length"] == 32
write("length", 0); assert read("length") == "8"
write("length", "16")
write("transpose", "+1 oct"); assert read("transpose") == "5" and view()["transpose"] == 12
write("transpose", 4); assert view()["transpose"] == 0
write("current_bank", 5); assert read("current_bank") == "1", "a readout"
write("volume", 7); assert read("volume") == "1.0000"
write("volume", "nan"); assert read("volume") == "1.0000"
write("volume", 0.75)

# --- stopped: silent, and the slot is kept awake -------------------------------
assert view()["running"] == 0
quiet = clock.run(0.5)
assert set(quiet) == {6}, "keepalive offset while silent"
write("volume", 0)
clock.run(0.3)
assert set(clock.run(0.1)) == {0}, "VOLUME 0 is exact digital silence"
write("volume", 0.75)

# --- Play: the first clock pulse is step 1, and it sounds ----------------------
pattern("1000" * 4)
clock.play()
first = clock.block()
assert view()["pos"] == 0 and view()["running"] == 1, view()
assert max(abs(v) for v in first[64:]) > 100, "the downbeat sounds in its own block"
bar = first + clock.run(2.0 - BLOCK / SR)       # one bar at 120 BPM
step = SR * 60 / 120 / 4
for n in range(4):                               # a hit on every beat, silence between
    at = int(n * 4 * step)
    assert rms(bar[at:at + 2000]) > 20 * max(1.0, rms(bar[at + int(2.6 * step):at + int(3.9 * step)])), n
assert view()["pos"] == 15, view()

# --- tempo follows the clock, not a timer ----------------------------------------
transport["bpm"] = 150.0
clock.run(60 / 150 * 4)                          # one bar at the new tempo
assert view()["pos"] == 15, view()
transport["bpm"] = 120.0

# --- Stop: the note is released and the playhead holds --------------------------
clock.run(0.05)
clock.stop()
held = view()["pos"]
tail = clock.run(1.5)
assert set(tail[-2000:]) == {6}, "silent again after Stop"
assert view()["pos"] == held and view()["running"] == 0
clock.play()
clock.block()
assert view()["pos"] == 0, "Play restarts the pattern"
clock.stop()
clock.run(1.0)

# --- accent: TB-3PO's velocity 118 is the 303's accent --------------------------
def one_bar(kinds, **knobs):
    for k, v in knobs.items():
        write(k, v)
    pattern(kinds)
    clock.play()
    out = clock.run(2.0)
    clock.stop()
    clock.run(1.0)
    return out


write("rat_on", 0)
plain = one_bar("1000" * 4, acc_level=1.0)
accented = one_bar("2000" * 4, acc_level=1.0)
assert max(map(abs, accented)) > 1.3 * max(map(abs, plain)), (max(map(abs, plain)), max(map(abs, accented)))
flat = one_bar("2000" * 4, acc_level=0.0)
assert max(map(abs, flat)) < 1.15 * max(map(abs, plain)), "ACCENT knob at 0: an accent is a plain note"
write("acc_level", 0.5)

# --- slide: the next note-on arrives before the note-off, so the 303 glides ----
def gap(out):
    """Quietest 5 ms between the first two steps."""
    a, b = int(0.6 * step), int(1.0 * step)
    return min(rms(out[i:i + 220]) for i in range(a, b - 220, 110))


write("gate", 0.3)
for i, (deg, octv) in enumerate([(0, 0), (4, 1)]):
    write("set_step", f"{i}:1:{deg}:{octv}")
detached = one_bar("11" + "0" * 14)
slid = one_bar("31" + "0" * 14)
assert gap(slid) > 4 * max(1.0, gap(detached)), (gap(slid), gap(detached))
write("gate", 0.5)

# --- RAT: after the 303, before VOLUME -------------------------------------------
def hf(x):
    """Share of the energy in the sample-to-sample difference: brightness."""
    return rms([b - a for a, b in zip(x, x[1:])]) / max(1.0, rms(x))


write("cutoff", 0.2); write("resonance", 0.2)
clean = one_bar("1111" * 4)
dirty = one_bar("1111" * 4, rat_on=1, rat_dist=0.9, rat_filter=0.0)
assert hf(dirty) > 2.0 * hf(clean), (hf(clean), hf(dirty))
dark = one_bar("1111" * 4, rat_filter=1.0)
assert hf(dark) < 0.6 * hf(dirty), "RAT FILTER closes the top"
# VOLUME scales the finished sound and changes nothing else about it.
loud = one_bar("1111" * 4, rat_filter=0.5, volume=0.5)
soft = one_bar("1111" * 4, volume=0.25)
assert max(map(abs, loud)) < 0.7 * 32767, "below the limiter knee for this check"
ratio = rms(loud) / rms(soft)
assert abs(ratio - 4.0) < 0.1, ratio
assert abs(hf(loud) - hf(soft)) < 0.02 * hf(loud), "same waveform, only quieter"
# RAT LEVEL is the pedal's own volume, a different control.
half = one_bar("1111" * 4, rat_level=0.25)
assert rms(half) < 0.5 * rms(soft)
write("rat_level", 0.5)
# VOLUME 0 mutes it, exactly, even at full drive.
write("volume", 0)
clock.play(); clock.run(0.2)
assert set(clock.run(1.0)) == {0}
clock.stop()
write("volume", 0.75); write("cutoff", 0.5); write("resonance", 0.5); write("rat_dist", 0.5)

# --- VCO2: hard-synced to VCO1, before the 303 filter ------------------------------
assert levels["vco"]["knobs"] == ["vco2_level", "vco2_tune", "vco2_fine", "vco2_wave", "vco2_sync",
                                  "sync_env", "vco1_level"]
write("rat_on", 0); write("cutoff", 1.0); write("resonance", 0.0); write("env_mod", 0.0)
one = one_bar("1111" * 4)
again = one_bar("1111" * 4)
write("vco2_level", 1.0); write("vco1_level", 0.0)
assert read("vco2_level") == "1.0000" and read("vco2_tune") == "7" and read("vco2_sync") == "1"
synced7 = one_bar("1111" * 4)
write("vco2_tune", 19)
synced19 = one_bar("1111" * 4)
assert hf(synced19) > 1.3 * hf(synced7), (hf(synced7), hf(synced19))
write("vco2_fine", 50); assert read("vco2_fine") == "50"
write("vco2_fine", 500); assert read("vco2_fine") == "50", "clamped"
write("vco2_wave", "Square"); assert read("vco2_wave") == "1"
square = one_bar("1111" * 4)
assert square != synced19 and rms(square) > 100
write("sync_env", 1.0)
swept = one_bar("1111" * 4)
assert swept != square
# Back to the defaults: the 303 alone again, sounding as it did.
for k, v in (("vco2_level", 0.0), ("vco1_level", 1.0), ("vco2_tune", 7), ("vco2_fine", 0),
             ("vco2_wave", 0), ("sync_env", 0.0)):
    write(k, v)
clock.run(1.0)
back = one_bar("1111" * 4)
assert abs(rms(back) / rms(one) - 1.0) < 0.05, (rms(one), rms(back))
write("cutoff", 0.5); write("resonance", 0.5); write("env_mod", 0.5); write("rat_on", 1)

# --- extremes: bounded, finite, no DC, nothing stuck ------------------------------
for k in ("volume", "cutoff", "resonance", "env_mod", "decay", "acc_level", "rat_dist", "rat_filter",
          "osc_level", "rat_level", "tanh_shaper_drive", "normal_decay", "accent_decay"):
    write(k, 1.0)
write("devil_mod_switch", 1); write("rat_model", 1); write("waveform", 1)
write("vco2_level", 1.0); write("vco2_tune", 24); write("sync_env", 1.0); write("vco2_wave", 1)
pattern("2321" * 4, octave=2)
write("density", 1.0)
clock.play()
wild = clock.run(8.0)
clock.stop()
peak = max(map(abs, wild))
pinned = sum(1 for v in wild if abs(v) >= 32767) / len(wild)
dc = abs(sum(wild) / len(wild)) / 32767
assert peak <= 32767 and pinned < 0.001, (peak, pinned)
assert dc < 0.02, dc
for k in ("rat_dist", "rat_filter", "resonance", "cutoff", "vco2_level", "sync_env"):   # and at the other end
    write(k, 0.0)
clock.play(); clock.run(2.0); clock.stop()
# Devilfish decays are up to 3 s and every gain is at its top, so the tail is
# long -- but it is a tail: it must end, not settle into a limit cycle.
after = clock.run(12.0)
assert max(map(abs, after[int(3.0 * SR):])) < 64, "decaying, not ringing"
assert set(after[-2000:]) == {6}, "rings out to silence"

# --- state: everything, through a second instance -----------------------------------
write("set_step", "5:3:4:1")
write("store_bank", 2)
write("length", "32"); write("direction", 2); write("transpose", "-2 oct"); write("root", 3)
write("generate", 0)
write("store_bank", 6)
state = read("state")
doc = json.loads(state)
assert doc["v"] == 1 and len(state) < 2048, len(state)
q = create(b".", None)
write("state", state, q)
assert read("state", inst=q) == state
va, vb = view(), view(q)
for k in ("bank", "filled", "length", "dir", "transpose", "steps"):
    assert va[k] == vb[k], (k, va[k], vb[k])
assert vb["filled"] == "00100010" and vb["length"] == 32
for prm in params:
    assert read(prm["key"]) == read(prm["key"], inst=q), prm["key"]
# A bank brings its own length back (TB-3PO: length is part of a pattern).
write("recall_bank_now", 2, q)
assert view(q)["length"] == 16 and view(q)["steps"][5] == "3"
# A restored instance PLAYS it: two fresh instances from one state are
# sample-identical, and sound like the one the state was taken from (which
# differs only in what cannot be saved -- oscillator phase, filter history).
r = create(b".", None)
write("state", state, r)
outs = []
for inst in (p, q, r):
    write("recall_bank_now", 6, inst)
    write("direction", 0, inst)
    for k, v in (("volume", 0.6), ("rat_dist", 0.5), ("rat_filter", 0.5), ("cutoff", 0.5), ("resonance", 0.5),
                 ("vco2_level", 0.7), ("vco2_tune", 12), ("vco2_fine", -20), ("sync_env", 0.4)):
        write(k, v, inst)
    cl = Clock(inst)
    cl.run(3.0)
    cl.play(); outs.append(cl.run(4.0)); cl.stop()
    cl.run(1.0)
assert rms(outs[1]) > 300 and outs[1] == outs[2], "two instances from one state are sample-identical"
assert abs(rms(outs[0]) / rms(outs[1]) - 1.0) < 0.2, (rms(outs[0]), rms(outs[1]))
destroy(r)
state = read("state")
write("state", state, q)
# Rejected documents change nothing.
before = read("state", inst=q)
for bad_doc in ("", "{", "{}", '{"v":2}', state[:-1], state.replace('"v":1', '"v":9'),
                state.replace('"volume":0.6000', '"volume":7'),
                state[:state.index('"seq":"') + 7] + 'zz"}'):
    write("state", bad_doc, q)
    assert read("state", inst=q) == before, bad_doc[:40]
# A document from a newer build (unknown key) and one with a key missing still load.
write("state", state.replace('{"v":1', '{"v":1,"future":3'), q)
assert read("state", inst=q) == state
destroy(q)

# --- incoming notes are not the sequencer's: ignored -----------------------------------
clock.run(2.0)
midi(p, (c.c_uint8 * 3)(0x90, 60, 127), 3, 0)
assert set(clock.run(0.3)) == {6}

# --- pools: bounded, and reusable ---------------------------------------------------------
more = [create(b".", None) for _ in range(8)]
assert sum(1 for m in more if m) == 5 and all(m is None for m in more[5:]), "6 instances, then a refusal"
for m in more:
    if m:
        destroy(m)
again = create(b".", None)
assert again
destroy(again)
destroy(p)
print("PASS: smoke")
