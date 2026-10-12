# Third-party code in o303

o303 is Copyright (C) 2026 Hiro Ikezawa and is distributed under
**GPL-3.0-only** (see `LICENSE`), because it is a combined work of the GPL
components below. Everything under `vendor/` is an
unmodified copy (the Rat Drive files carry their author name as Hiro Ikezawa,
as later pedal-fx releases do -- no code differs); `vendor/SOURCES.txt` records the revisions and
`vendor/SHA256SUMS` the checksums (`scripts/vendor.sh --verify`).

| Part | Where | From | Licence |
| --- | --- | --- | --- |
| TB-3PO sequencer DSP (`tb3po.c`) and step-lane drawing (`ui/lane.mjs`) | `vendor/tb3po/` | [charlesvestal/schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po), a port of the `TB_3PO` applet of djphazer's [O_C-Phazerville](https://github.com/djphazer/O_C-Phazerville) Hemisphere Suite | GPL-3.0 |
| TB-3PO UI (`ui/ui.js`, `params.mjs`, `pages.mjs`, `pad_map.mjs`) — test reference only, not shipped | `vendor/tb3po/ui/` | same | GPL-3.0 |
| 303 synth plugin (`plugin.cpp`, `drive.h`) | `vendor/schwung-303/` | [charlesvestal/schwung-303](https://github.com/charlesvestal/schwung-303) (after midilab/jc303; drive model after davemollen/dm-Rat) | GPL-3.0 |
| Open303 engine | `vendor/schwung-303/open303/` | Robin Schmidt (rosic), via jc303 | MIT (`open303/LICENSE`) |
| Rat Drive pedal DSP (`rat.c`) | `vendor/pedal-fx/rat/` | schwung-pedal-fx, derived from schwung-303's `drive::rat` and [davemollen/dm-Rat](https://github.com/davemollen/dm-Rat) | GPL-3.0-only |
| Pedal support headers and `gen_meta.py` | `vendor/pedal-fx/common/`, `scripts/` | schwung-pedal-fx | MIT |
| Schwung plugin API header (`host/plugin_api_v1.h`, from Schwung 1.7.3), unmodified | `third_party/schwung/` | [charlesvestal/schwung](https://github.com/charlesvestal/schwung) | MIT (`third_party/schwung/LICENSE`) |

`dsp/open303_sync.h` contains a modified copy of `rosic::Open303::getSample()`
from Open303 (MIT, Robin Schmidt; notice in `vendor/schwung-303/open303/LICENSE`).
Its VCO2 band-limits edges the way Mutable Instruments' Plaits oscillators do
(two-sample PolyBLEP, one sample of delay); no Plaits code is included.

This module's own files (`dsp/`, `*.mjs`, `ui_chain.js`, `scripts/`, `tests/`)
are Copyright (C) 2026 Hiro Ikezawa, GPL-3.0-only. `seq_pads.mjs` and `pages.mjs` are derived from TB-3PO's
`ui.js`, `pages.mjs` and `pad_map.mjs`.

Unofficial emulations: not affiliated with or endorsed by Roland, ProCo or
their successors. "TB-303" and "RAT" are used descriptively.
