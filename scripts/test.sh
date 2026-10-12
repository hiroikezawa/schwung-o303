#!/usr/bin/env bash
# Native build + every test, then the Move build (check_abi.py rejects a libc /
# libstdc++ symbol newer than the Move's).
#   1. engine   dsp/tb_engine.c against the stand-alone TB-3PO plugin (ASan/UBSan)
#   2. vco      dsp/open303_sync.h: bit-identical to Open303 at its defaults, sync works
#   3. smoke    the real dsp.so through the v2 ABI, as the chain host loads it
#   4. plugin   the same dsp.so built with ASan/UBSan, hammered with random input
#   5. ui       ui_core.mjs against Schwung's real page engine, and its pad layer
#               against TB-3PO's real ui.js
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
V="$MODULE/vendor"
SAN=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
bash "$MODULE/scripts/build.sh" native
OUT="$MODULE/build/native"

"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O1 -Wall -Wextra -Wno-unused-parameter -Werror "${SAN[@]}" \
  -DTB3PO_STATE_DIR="\"$OUT\"" -DTB3PO_STATE_PATH_FOR_TEST="\"$OUT/tb3po.bin\"" \
  -I"$V" -I"$MODULE/dsp" "$MODULE/tests/test_engine.c" "$MODULE/tests/ref_tb3po.c" "$MODULE/dsp/tb_engine.c" \
  -o "$OUT/test-engine" -lpthread
"$OUT/test-engine"

# The second VCO against the unmodified Open303, built from this run's objects.
"${CXX:-g++}" -std=c++17 -U_GNU_SOURCE -D_DEFAULT_SOURCE -O2 -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter \
  -I"$V" -I"$V/schwung-303" -I"$MODULE/dsp" "$MODULE/tests/test_vco.cpp" \
  "$MODULE"/build/obj-native-native/open303_*.o -o "$OUT/test-vco"
"$OUT/test-vco"

python3 "$MODULE/tests/smoke.py" "$OUT/dsp.so"

TR_OUT="$MODULE/build/asan" TR_EXTRA_FLAGS="${SAN[*]}" bash "$MODULE/scripts/build.sh" native >/dev/null
"${CC:-gcc}" -std=c11 -D_DEFAULT_SOURCE -O1 -Wall -Wextra -Werror "${SAN[@]}" \
  -I"$MODULE/third_party/schwung" "$MODULE/tests/test_plugin.c" -o "$OUT/test-plugin" -ldl
"$OUT/test-plugin" "$MODULE/build/asan/dsp.so"

# The UI test drives Schwung's own page engine and draw harness: it needs a
# Schwung checkout (SCHWUNG_ROOT, else ../schwung beside this repository).
SCHWUNG="${SCHWUNG_ROOT:-$(cd "$MODULE/.." && pwd)/schwung}"
if [[ ! -f "$SCHWUNG/src/shared/param_pages/page_controller.mjs" ]]; then
  echo "SKIP: UI test needs a Schwung checkout (set SCHWUNG_ROOT)" >&2
elif command -v node >/dev/null 2>&1; then
  SCHWUNG_ROOT="$SCHWUNG" node "$MODULE/tests/test_ui.mjs"
elif command -v node.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
  SCHWUNG_ROOT="$SCHWUNG" WSLENV="SCHWUNG_ROOT/p${WSLENV:+:$WSLENV}" node.exe "$(wslpath -w "$MODULE/tests/test_ui.mjs")"
else
  echo "Node.js is required for the UI test" >&2; exit 1
fi
if command -v "${CROSS_PREFIX:-aarch64-linux-gnu-}gcc" >/dev/null 2>&1; then
  bash "$MODULE/scripts/build.sh" arm64
else
  echo "SKIP: no aarch64 cross compiler; ABI not checked" >&2
fi
