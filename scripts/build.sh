#!/usr/bin/env bash
# Build the o303 module.
#   scripts/build.sh native   desktop dsp.so for the tests (build/native)
#   scripts/build.sh arm64    Move dsp.so + install tarball (dist/)
set -euo pipefail
ID=o303
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
# Schwung's plugin API header ships in third_party/schwung (MIT, see its
# LICENSE), unmodified, from the Schwung release the Move runs (1.7.3).
API="$MODULE/third_party/schwung"

ARCH="${1:-arm64}"
case "$ARCH" in
  native) CC="${CC:-gcc}"; CXX="${CXX:-g++}"; OUT="$MODULE/build/native"; TUNE=() ;;
  arm64) CC="${CROSS_PREFIX:-aarch64-linux-gnu-}gcc"; CXX="${CROSS_PREFIX:-aarch64-linux-gnu-}g++"
         OUT="$MODULE/dist/$ID"; TUNE=(-march=armv8-a -mtune=cortex-a72) ;;
  *) echo "Usage: $0 [native|arm64]" >&2; exit 2 ;;
esac
# TR_EXTRA_FLAGS / TR_OUT: scripts/test.sh's sanitizer build.
read -r -a EXTRA <<< "${TR_EXTRA_FLAGS:-}"
OUT="${TR_OUT:-$OUT}"
V="$MODULE/vendor"
GEN="$MODULE/build/generated"
OBJ="$MODULE/build/obj-$(basename "$OUT")-$ARCH"
rm -rf "$OBJ"
[[ "$ARCH" == arm64 ]] && rm -rf "$OUT"
mkdir -p "$OUT" "$GEN/rat" "$OBJ"

# vendor/ is three upstream projects, byte for byte; refuse to build an edit.
bash "$MODULE/scripts/vendor.sh" --verify
python3 "$MODULE/scripts/metadata.py" "$GEN/tr_metadata.h"
# The pedal's own generator: knob indices and defaults from its pedal.json.
python3 "$V/pedal-fx/scripts/gen_meta.py" "$V/pedal-fx/rat" "$GEN/rat"
RAT_OS=$(python3 -c "import json; print(json.load(open('$V/pedal-fx/rat/pedal.json'))['oversampling'])")

# Shared: no -ffast-math (the isfinite() guards must stay real); -fno-math-errno
# keeps sqrtf & co. inline on ARM instead of binding a newer glibc's symbol.
BASE=(-fPIC -fno-math-errno -fvisibility=hidden "${TUNE[@]}" "${EXTRA[@]}")
# -std=c11 / -U_GNU_SOURCE matter: under GNU C23 / C++ the headers bind strtol
# to __isoc23_strtol@GLIBC_2.38, and Move has glibc 2.34.
C11=(-std=c11 -D_DEFAULT_SOURCE)
objs=()
cc() {  # cc <object name> <source> <flags...>
  local obj="$OBJ/$1.o" src=$2; shift 2
  "$CC" "${BASE[@]}" "$@" -c "$src" -o "$obj"
  objs+=("$obj")
}
cxx() {
  local obj="$OBJ/$1.o" src=$2; shift 2
  "$CXX" "${BASE[@]}" -std=c++17 -U_GNU_SOURCE -D_DEFAULT_SOURCE -fvisibility-inlines-hidden "$@" -c "$src" -o "$obj"
  objs+=("$obj")
}

# 1. TB-3PO: tb3po.c inside dsp/tb_engine.c, with TB-3PO's own warning flags.
cc tb_engine "$MODULE/dsp/tb_engine.c" "${C11[@]}" -O2 -Wall -Wextra -Wno-unused-parameter -Werror -I"$V" -I"$MODULE/dsp"
# 2. The 303: plugin.cpp inside dsp/voice303.cpp, plus Open303 itself, with
#    the 303 module's flags. -fno-strict-aliasing: Open303 type-puns a double
#    (EXPOFDBL) to pick its mip-map level.
#    -fno-aggressive-loop-optimizations: upstream's initPrototypeTable() zeroes
#    four doubles past prototypeTable[2048] (into tableSet, which is zeroed
#    next anyway). Harmless as written, undefined to the optimizer -- so it is
#    told not to reason from it, and the bounds sanitizer is kept off these
#    files only. Reported in README.md; vendor/ is never edited.
S303=(-O3 -fno-strict-aliasing -fno-aggressive-loop-optimizations -fno-sanitize=bounds
      -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare
      -I"$V" -I"$V/schwung-303" -I"$V/schwung-303/open303" -I"$MODULE/dsp")
cxx voice303 "$MODULE/dsp/voice303.cpp" "${S303[@]}"
for f in "$V"/schwung-303/open303/*.cpp; do cxx "open303_$(basename "$f" .cpp)" "$f" "${S303[@]}"; done
for f in "$V"/schwung-303/open303/*.c; do cc "open303_$(basename "$f" .c)" "$f" -std=c99 -O3; done
# 3. The RAT: rat.c inside dsp/rat_stage.c, with the pedal project's flags.
cc rat_stage "$MODULE/dsp/rat_stage.c" "${C11[@]}" -O2 -ffp-contract=fast -Wall -Wextra -Werror -Wshadow \
   -Wno-unused-function -DPFX_OS="$RAT_OS" -I"$V" -I"$V/pedal-fx" -I"$GEN/rat" -I"$MODULE/dsp"
# 4. The module.
cc tr_plugin "$MODULE/dsp/tr_plugin.c" "${C11[@]}" -O2 -Wall -Wextra -Werror -I"$API" -I"$GEN" -I"$MODULE/dsp"

# libstdc++ stays dynamic, as in the catalog 303 module this code comes from:
# Open303 needs only std::list / std::string / new (GLIBCXX_3.4.21, GCC 5),
# while a static copy drags in arc4random and _dl_find_object, which Move's
# glibc 2.34 does not have. check_abi.py caps the versions. Hidden visibility
# plus -Bsymbolic bind every rosic:: call locally, so the catalog 303 loaded in
# the same process can never interpose on this module (or the reverse).
"$CXX" -shared "${EXTRA[@]}" -o "$OUT/dsp.so" "${objs[@]}" -Wl,-z,defs -Wl,-Bsymbolic -lm

cp "$MODULE/module.json" "$OUT/"
if [[ "$ARCH" == arm64 ]]; then
  python3 "$MODULE/scripts/check_abi.py" "$OUT/dsp.so"
  # A missed .mjs fails at IMPORT time on the device: a module that installs
  # cleanly and then does not open. So every one is listed, and required.
  for f in ui_chain.js ui_core.mjs seq_pads.mjs pages.mjs help.json README.md LICENSE THIRD_PARTY_LICENSES.md; do
    [[ -f "$MODULE/$f" ]] || { echo "ERROR: $f missing -- not packaging a module that cannot load" >&2; exit 1; }
    cp "$MODULE/$f" "$OUT/"
  done
  mkdir -p "$OUT/vendor/tb3po/ui"
  cp "$V/tb3po/ui/lane.mjs" "$OUT/vendor/tb3po/ui/"
  cp "$V/tb3po/LICENSE" "$OUT/vendor/tb3po/"
  # The MIT parts travel with their notices (Open303; the pedal's helpers).
  mkdir -p "$OUT/vendor/schwung-303/open303" "$OUT/vendor/pedal-fx"
  cp "$V/schwung-303/LICENSE" "$OUT/vendor/schwung-303/"
  cp "$V/schwung-303/open303/LICENSE" "$OUT/vendor/schwung-303/open303/"
  cp "$V/pedal-fx/LICENSE" "$OUT/vendor/pedal-fx/"
  mkdir -p "$OUT/third_party/schwung"
  cp "$API/LICENSE" "$OUT/third_party/schwung/"
  cp "$V/SOURCES.txt" "$OUT/vendor/"
  tar -czf "$MODULE/dist/$ID-module.tar.gz" -C "$MODULE/dist" "$ID"
fi
file "$OUT/dsp.so"
