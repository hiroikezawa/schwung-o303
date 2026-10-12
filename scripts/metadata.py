#!/usr/bin/env python3
"""Embed module.json's chain_params in the DSP so get_param never reads a file.

Also cross-checks the manifest against the plugin's own parameter table, so the
knob metadata the host shows can never drift from what the DSP does, and
against the page hierarchy the plugin serves (every key on a page is declared,
every declared key is on a page).
"""
import json
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
params = json.loads((root / "module.json").read_text())["capabilities"]["chain_params"]
plugin = (root / "dsp" / "tr_plugin.c").read_text()

table = plugin.split("tr_params[P_COUNT] = {", 1)[1].split("\n};", 1)[0]
rows = re.findall(r'\{"([a-z0-9_]+)", "[a-z0-9_]*", D_[A-Z0-9]+, K_([A-Z]+), '
                  r'([-0-9.]+)f, ([-0-9.]+)f, ([-0-9.]+)f, (\w+), \w+\}', table)
assert [p["key"] for p in params] == [r[0] for r in rows], ("chain_params order != tr_params", rows)
counts = {name: len(re.findall(r'"[^"]*"', body))
          for name, body in re.findall(r"static const char \*const (o_\w+)\[\] = \{(.*?)\};", plugin, re.S)}
for p, (key, kind, lo, hi, default, opts) in zip(params, rows):
    lo, hi, default = float(lo), float(hi), float(default)
    assert float(p["default"]) == default, (key, "default drift")
    if p["type"] == "enum":
        assert kind == ("TEXT" if p.get("options_as_string") else "ENUM"), (key, "enum wire format drift")
        assert lo == 0 and hi == len(p["options"]) - 1 == counts[opts] - 1, (key, "option count drift")
    else:
        assert kind == p["type"].upper() and opts == "0", (key, "type drift")
        assert float(p["min"]) == lo and float(p["max"]) == hi, (key, "range drift")

hierarchy = json.loads(json.loads('"' + "".join(re.findall(
    r'^\s+"(.*)"$', plugin.split("tr_hierarchy[] =", 1)[1].split(";\n", 1)[0], re.M)) + '"'))
paged = [k for level in hierarchy["levels"].values() for k in level["knobs"]]
assert sorted(paged) == sorted(p["key"] for p in params), set(paged) ^ {p["key"] for p in params}
assert all(len(level["knobs"]) <= 8 for level in hierarchy["levels"].values())
assert hierarchy["levels"]["root"]["knobs"][0] == "volume", "knob 1 is VOLUME"

out = pathlib.Path(sys.argv[1])
out.write_text("/* Generated from module.json by scripts/metadata.py. Do not edit. */\n"
               "static const char tr_chain_params[] = "
               + json.dumps(json.dumps(params, separators=(",", ":")), ensure_ascii=True) + ";\n")
