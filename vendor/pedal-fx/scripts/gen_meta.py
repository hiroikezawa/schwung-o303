#!/usr/bin/env python3
"""Generate a pedal's module.json and C metadata from src/<pedal>/pedal.json.

    gen_meta.py <pedal-dir> <out-dir>

pedal.json is the single source of truth for the knob list, defaults and
factory presets, so the manifest the host reads and the tables the DSP uses
cannot drift apart. Writes:

    <out-dir>/module.json     the Schwung manifest (chain_params + ui_hierarchy)
    <out-dir>/pedal_meta.h    keys, defaults, presets, and both JSON strings
"""
import json
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
spec = json.loads((src / "pedal.json").read_text())

params = spec["params"]
keys = [p["key"] for p in params]
assert len(keys) == 8, "a pedal has exactly eight knobs (one knob page)"
assert len(set(keys)) == 8, "duplicate key"
for k in keys:
    assert re.fullmatch(r"[a-z][a-z0-9_]*", k), k
    assert k not in ("state", "preset", "preset_count", "preset_name", "name",
                     "chain_params", "ui_hierarchy"), f"reserved key {k}"
for p in params:
    assert 0.0 <= p["default"] <= 1.0, p
# Optional two-or-more-way SWITCHES beyond the eight knobs (e.g. RAT's op-amp
# model). They live on their own page after the knobs and are enums on the
# wire: an integer index, or the option name.
switches = spec.get("switches", [])
for sw in switches:
    assert re.fullmatch(r"[a-z][a-z0-9_]*", sw["key"]) and sw["key"] not in keys, sw
    assert 2 <= len(sw["options"]) and 0 <= sw["default"] < len(sw["options"]), sw
    assert all(len(o) <= 4 and o.isascii() for o in sw["options"]), "enum options must fit the ~4-char square"
all_keys = keys + [sw["key"] for sw in switches]
presets = spec["presets"]
assert len(presets) >= 5, "at least five factory presets"
for pr in presets:
    for sw in switches:  # a preset may leave a switch at its default
        pr.setdefault(sw["key"], sw["default"])
        assert pr[sw["key"]] in range(len(sw["options"])), (pr["name"], sw["key"])
    assert set(pr) == {"name", *all_keys}, (pr["name"], set(pr) ^ {"name", *all_keys})
    assert all(0.0 <= pr[k] <= 1.0 for k in keys), pr["name"]
    assert len(pr["name"]) <= 20 and '"' not in pr["name"], pr["name"]

help_doc = json.loads((src / "help.json").read_text())
def _walk(node):
    for line in node.get("lines", []):
        assert len(line) <= 21 and line.isascii(), f"help line too long / non-ASCII: {line!r}"
    for child in node.get("children", []):
        _walk(child)
_walk(help_doc)
assert help_doc["children"], "help.json needs topics (the Module Help row needs children)"

# Level-type knobs draw as faders; everything else as a plain dial. Declared
# rather than left to the grid's name-based guess (which took BD-2's "Gain",
# a drive control, for a fader).
FADERS = {"volume", "level", "input", "output"}
chain_params = [{
    "key": p["key"], "name": p["name"], "type": "float",
    "min": 0, "max": 1, "step": 0.01, "default": p["default"],
    "unit": "%", "display_format": "%.0f",
    "viz": {"kind": "fader"} if p["key"] in FADERS else False,
} for p in params] + [{
    "key": sw["key"], "name": sw["name"], "type": "enum",
    "options": sw["options"], "default": sw["default"],
} for sw in switches]

switch_links = [{"level": sw["key"], "label": sw["name"]} for sw in switches]
ui_hierarchy = {"levels": {
    "root": {
        "name": spec["name"],
        "knobs": keys,
        "params": [{"key": k} for k in keys] + switch_links + [{"level": "presets", "label": "Presets"}],
    },
    **{sw["key"]: {"label": sw["name"], "knobs": [sw["key"]], "params": [{"key": sw["key"]}]}
       for sw in switches},
    "presets": {
        "label": "Presets",
        "list_param": "preset", "count_param": "preset_count", "name_param": "preset_name",
    },
}}

module = {
    "id": spec["id"], "name": spec["name"], "abbrev": spec["abbrev"],
    "version": spec["version"], "description": spec["description"],
    "author": spec["author"], "license": spec["license"],
    "dsp": spec["id"] + ".so", "api_version": 2, "component_type": "audio_fx",
    "capabilities": {
        "chainable": True, "component_type": "audio_fx",
        "audio_in": True, "audio_out": True, "midi_in": False, "midi_out": False,
        "chain_params": chain_params, "ui_hierarchy": ui_hierarchy,
    },
}
(out / "module.json").write_text(json.dumps(module, indent=2) + "\n")


def c_str(s):
    return json.dumps(s, ensure_ascii=True)


def compact(o):
    return c_str(json.dumps(o, separators=(",", ":")))


lines = [
    "/* Generated from pedal.json by scripts/gen_meta.py. Do not edit. */",
    "#pragma once",
    f"#define PFX_ID {c_str(spec['id'])}",
    f"#define PFX_NAME {c_str(spec['name'])}",
    f"#define PFX_NPARAM {len(all_keys)}",
    f"#define PFX_NSWITCH {len(switches)}",
    f"#define PFX_PRESET_COUNT {len(presets)}",
    "enum {",
]
lines += [f"    PK_{k.upper()} = {i}," for i, k in enumerate(all_keys)]
lines += ["};",
          "static const char *const pfx_keys[PFX_NPARAM] = {" + ", ".join(c_str(k) for k in all_keys) + "};",
          "static const float pfx_defaults[PFX_NPARAM] = {" + ", ".join(
              [f"{p['default']:.4f}f" for p in params] + [f"{sw['default']}.0f" for sw in switches]) + "};",
          "/* knobs: 1 (0..1 continuous); switches: option count - 1 (integer index) */",
          "static const int pfx_param_steps[PFX_NPARAM] = {" + ", ".join(
              ["0"] * len(keys) + [str(len(sw["options"]) - 1) for sw in switches]) + "};",
          "/* NULL-terminated option names per parameter (NULL for knobs) */"]
for sw in switches:
    lines.append(f"static const char *const pfx_opts_{sw['key']}[] = {{" + ", ".join(c_str(o) for o in sw["options"]) + ", 0};")
lines += ["static const char *const *const pfx_param_options[PFX_NPARAM] = {" + ", ".join(
              ["0"] * len(keys) + [f"pfx_opts_{sw['key']}" for sw in switches]) + "};",
          "typedef struct { const char *name; float v[PFX_NPARAM]; } pfx_preset;",
          "static const pfx_preset pfx_presets[PFX_PRESET_COUNT] = {"]
for pr in presets:
    lines.append(f"    {{{c_str(pr['name'])}, {{" + ", ".join(f"{float(pr[k]):.4f}f" for k in all_keys) + "}},")
lines += ["};",
          f"static const char pfx_chain_params[] = {compact(chain_params)};",
          f"static const char pfx_ui_hierarchy[] = {compact(ui_hierarchy)};", ""]
(out / "pedal_meta.h").write_text("\n".join(lines))
