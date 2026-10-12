#!/usr/bin/env python3
"""Reject a dsp.so the Move cannot load, or one that could collide with a
neighbour: AArch64, glibc <= 2.34, a libstdc++ no newer than the one that
shipped beside glibc 2.34 (GCC 11: GLIBCXX_3.4.29, CXXABI_1.3.13), one export."""
import re
import subprocess
import sys

path = sys.argv[1]
header = subprocess.check_output(["readelf", "-h", path], text=True)
assert "AArch64" in header, "not an AArch64 build"

versions = subprocess.check_output(["readelf", "--version-info", path], text=True)
glibc = {tuple(map(int, s.split("."))) for s in re.findall(r"GLIBC_([0-9.]+)", versions)}
assert glibc and max(glibc) <= (2, 34), f"Move-incompatible GLIBC: {sorted(glibc)}"
def newest(prefix):
    found = {tuple(map(int, s.split("."))) for s in re.findall(prefix + r"_([0-9.]+)", versions)}
    return max(found) if found else (0,)
assert newest("GLIBCXX") <= (3, 4, 29), f"libstdc++ too new for the Move: GLIBCXX {newest('GLIBCXX')}"
assert newest("CXXABI") <= (1, 3, 13), f"libstdc++ too new for the Move: CXXABI {newest('CXXABI')}"

symbols = subprocess.check_output(["readelf", "-W", "--dyn-syms", path], text=True)
exported = []
for line in symbols.splitlines():
    f = line.split()
    # Num: Value Size Type Bind Vis Ndx Name
    if len(f) >= 8 and f[4] in ("GLOBAL", "WEAK") and f[6] != "UND":
        exported.append(f[7])
assert exported == ["move_plugin_init_v2"], f"unexpected exports: {exported}"
print("PASS: AArch64, only move_plugin_init_v2 exported, max GLIBC", ".".join(map(str, max(glibc))),
      "GLIBCXX", ".".join(map(str, newest("GLIBCXX"))), "CXXABI", ".".join(map(str, newest("CXXABI"))))
