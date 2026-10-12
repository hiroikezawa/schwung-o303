#!/usr/bin/env bash
# Copy the three upstream sources this module is built from into vendor/,
# UNMODIFIED, and record their checksums.
#
#   scripts/vendor.sh            refresh vendor/ from the checkouts below
#   scripts/vendor.sh --verify   check vendor/ against vendor/SHA256SUMS (no
#                                checkout needed) -- the build runs this
#
# Nothing under vendor/ is ever edited: the module adapts the files from the
# OUTSIDE (dsp/tb_engine.c, dsp/voice303.cpp, dsp/rat_stage.c), so an upstream
# fix is a re-run of this script.
set -euo pipefail
MODULE="$(cd "$(dirname "$0")/.." && pwd)"
V="$MODULE/vendor"
if [[ "${1:-}" == "--verify" ]]; then
  (cd "$V" && sha256sum --quiet -c SHA256SUMS)
  echo "vendor/: verified"
  exit 0
fi
TB3PO="${TB3PO_SRC:-$HOME/ext/schwung-tb3po}"      # charlesvestal/schwung-tb3po
S303="${S303_SRC:-$HOME/ext/schwung-303}"          # charlesvestal/schwung-303
# The pedal FX (Rat Drive). NOTE: it has moved past the 0.3.0 RAT vendored
# here (see vendor/SOURCES.txt); refreshing pulls the newer RAT DSP, which
# changes o303's sound -- do that on purpose, never by accident.
PFX="${PFX_SRC:-$HOME/schwung-pedal-fx}"
for d in "$TB3PO/src/dsp/tb3po.c" "$S303/src/dsp/plugin.cpp" "$PFX/src/rat/rat.c"; do
  [[ -f "$d" ]] || { echo "missing upstream: $d" >&2; exit 1; }
done
rm -rf "$V/tb3po" "$V/schwung-303" "$V/pedal-fx"
mkdir -p "$V/tb3po/include" "$V/schwung-303/open303" "$V/pedal-fx/rat" "$V/pedal-fx/common" "$V/pedal-fx/scripts"
cp "$TB3PO/src/dsp/tb3po.c" "$V/tb3po/"
cp "$TB3PO/src/dsp/include/plugin_api_v1.h" "$V/tb3po/include/"
cp "$TB3PO/LICENSE" "$V/tb3po/"
# TB-3PO's UI. lane.mjs (the step lane drawing) is SHIPPED and used as is; the
# rest is the reference tests/test_ui.mjs runs the pad layer against.
mkdir -p "$V/tb3po/ui"
for f in ui.js params.mjs pages.mjs pad_map.mjs lane.mjs; do cp "$TB3PO/src/$f" "$V/tb3po/ui/"; done
cp "$S303/src/dsp/plugin.cpp" "$S303/src/dsp/drive.h" "$S303/LICENSE" "$V/schwung-303/"
cp "$S303"/src/dsp/open303/* "$V/schwung-303/open303/"
cp "$PFX/src/rat/rat.c" "$PFX/src/rat/pedal.json" "$PFX/src/rat/help.json" "$V/pedal-fx/rat/"
for h in smoothing filters oversampling nonlinear tapers plugin_v2; do cp "$PFX/src/common/$h.h" "$V/pedal-fx/common/"; done
cp "$PFX/scripts/gen_meta.py" "$V/pedal-fx/scripts/"
cp "$PFX/LICENSE-GPL-3.0" "$PFX/LICENSE" "$V/pedal-fx/"
rev() { git -C "$1" rev-parse --short HEAD 2>/dev/null || echo "uncommitted"; }
cat > "$V/SOURCES.txt" <<TXT
tb3po        charlesvestal/schwung-tb3po @ $(rev "$TB3PO")  (module.json $(python3 -c "import json;print(json.load(open('$TB3PO/src/module.json'))['version'])"))
schwung-303  charlesvestal/schwung-303   @ $(rev "$S303")  (module.json $(python3 -c "import json;print(json.load(open('$S303/src/module.json'))['version'])"))
pedal-fx     schwung-pedal-fx (local)    @ $(rev "$PFX")  (Rat Drive $(python3 -c "import json;print(json.load(open('$PFX/src/rat/pedal.json'))['version'])"))
TXT
(cd "$V" && find tb3po schwung-303 pedal-fx -type f | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
echo "vendor/: refreshed"; cat "$V/SOURCES.txt"
