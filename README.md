# o303

**English** | [日本語](README.ja.md)

A sound generator for Schwung on Ableton Move. Inside one chain slot it wires three
things together into a single acid bass:

```
TB-3PO sequencer → Open303 (+ VCO2 with hard sync) → RAT distortion → Volume → output
```

- **TB-3PO**: a port to Schwung of the `TB_3PO` applet from djphazer's O_C-Phazerville
  (Hemisphere Suite). A sequencer that generates 303-style patterns by probability.
- **Open303**: Robin Schmidt's Roland TB-303 emulation engine, including the Devilfish
  mod extensions.
- **RAT**: a circuit model of the ProCo RAT distortion pedal, switchable between RAT
  (LM308) and RAT2 (OP07DP).

No MIDI channel setup and no second slot are needed. Load it into a slot and it plays in
time with Move's transport.

This is an unofficial emulation, not affiliated with Roland, ProCo or anyone connected
with them. "TB-303" and "RAT" are used only for description. The licence is
GPL-3.0-only (`LICENSE`, `THIRD_PARTY_LICENSES.md`).

## Getting started

1. In a Schwung chain slot, choose **o303** as the Sound Generator.
2. Open the slot's screen and the pads turn into the sequencer's control surface.
3. Press **Play** on Move and the pattern starts.
4. Each press of the bottom-left pad (**GEN**) generates a new pattern.
5. Shape a 303 sound with knobs 2–6 (Cutoff / Resonance / Env Mod / Decay / Accent), and
   add RAT distortion and tone with knobs 7 and 8.
6. To keep a pattern you like, **hold Shift** and tap a pad on the third row from the
   bottom to save it to banks 1–8.

Closing the screen gives the pads back to Move. The sequencer keeps playing for as long as
Move is playing.

## Controls (reference)

### Pads (while this sound generator's screen is open)

| Row | Role |
| --- | --- |
| Top | Steps 1–8 (first half of the step window) |
| Second | Steps 9–16 (second half of the step window) |
| Third | Banks 1–8 |
| Bottom | GEN (generate), MUT (mutate), DIR (play direction), OUT (notes outward), IN (notes inward). Pads 6–8 are unused |

- **Step pads**: each tap cycles rest → note → accent → slide → rest…
  - White is a note, red an accent, blue a slide, off a rest.
  - The playing step lights yellow.
- **Bank pads**: tap to recall, **Shift + tap** to save the current pattern.
  - A recall switches at the start of the next bar while playing, immediately while stopped.
  - A bank holds the step sequence, the pitches and the pattern length.
  - Purple is the current bank, teal a saved one, off an empty one. A bank waiting to be
    recalled flashes yellow; while Shift is held the banks flash red and pink as save
    targets.
- **GEN**: makes a new pattern with the current Density / Accent / Slide / Octaves
  probabilities. **Only when Length is 8 steps** it switches to a generation method made
  just for acid bass that sounds good repeating over 8 steps (see "GEN at 8 steps" below).
  At 16 / 24 / 32 steps it uses TB-3PO's own method.
- **MUT**: regenerates about 25 % of the current pattern's steps.
- **DIR**: cycles the play direction forward → reverse → ping-pong → random.
- **OUT** (4th, green): picks one sounding note of the phrase at random; if more of the
  phrase's notes are below it, it moves up one scale note, and if more are above it, down
  one (on a tie, either way at random). In other words, away from the middle (the median)
  of the phrase. The median is used because with the mean a single outlying note pulls
  the mean along and that note alone keeps rising. The more you press, the wider the
  phrase's range spreads.
- **IN** (5th, blue): takes whichever of the phrase's highest or lowest note is further
  from the mean and moves it one neighbouring scale note towards the mean (if both are
  equally far, one is picked at random). If that pitch occurs in several places, only one
  of them moves. The more you press, the tighter the range gets; once every note is the
  same pitch it does nothing.
- OUT and IN only move pitch; the step type (note / accent / slide) and position stay the
  same. "The phrase" means the sounding steps within Length; rests are ignored. The amount
  moved is the result of a semitone move quantised to the current scale, i.e. the
  neighbouring scale note. They never leave the Octaves range. If the note OUT picks sits
  at the lower limit (the root of the lowest octave) or the upper limit and cannot move
  outward, it moves one step the other way. The picked note always changes, so even a
  root-heavy phrase gets a variety of notes changed each time. Both can be undone once
  with Undo.

**About pad colours Move draws**: even while this screen is open, Move keeps lighting its
own track's pads in the background (a Drum Rack's pads lighting along with a clip, for
example). Pad presses are taken away from Move, but Move's LEDs cannot be stopped. So
whenever Move rewrites a pad colour it is repainted with the sequencer's colour on the
next frame, and all pads are repainted about every 0.15 s as well. For about one frame
(a dozen or so milliseconds) after Move lights a pad, Move's colour may show.

### GEN at 8 steps

TB-3PO's own generation decides steps one at a time by probability. Over 16–32 steps the
bias evens out, but at 8 steps you hear half a bar over and over, so phrases tend to lack
shape. So when Length is 8, GEN builds a single loop the way acid bass lines are put
together. The operation is unchanged: just press GEN.

1. A **style** is picked at random each time from these five. It is not shown on screen
   and cannot be chosen.
   - **HYPNOTIC**: centred on repeating the root. Little pitch movement; accents make the
     rhythm. Few slides.
   - **INDUSTRIAL**: an aggressive phrase around the root, decorated with the semitones just
     above and below. Strong accents; slides only over short intervals.
   - **ROLLING**: more notes, a rolling phrase moving between root, fifth and octave.
     Relatively many slides.
   - **MINIMAL**: almost one note (over 80 % root). Variation from rests and accents, with
     an occasional jump up an octave.
   - **BROKEN**: many rests, an uneasy phrase decorated with semitones and tritones.
     Irregular accents, few big leaps.
2. The **rhythm** comes first. One 8-step rhythm template such as `X-XX-X-X` or
   `XX-XX-XX` is picked, varied each time (moving one hit, for instance), then hits are
   added or removed to reach the count Density asks for. Step 1 always sounds, and it is
   never all rests or overcrowded (at least 2 notes even at Density 0).
3. The **pitches** come next. From intervals that suit the style (root, fifth, minor
   third, octave, semitone, ...) only notes allowed by Root / Scale / Octaves are used.
   Notes outside the current scale (a semitone in Minor, for example) are not used.
   Repeated notes are favoured, big leaps avoided, and the last note is one that leads
   easily back to the root on step 1.
4. **Accents** go on the beats the style favours. Their number is set by the Accent
   probability, and not every note gets one.
5. **Slides** only go from a sounding step to the next sounding step (including the join
   from step 8 back to step 1 at the loop start). Semitone and same-note slides are
   preferred, and their number is set by the Slide probability. Not every note gets one,
   and they never cross a rest.
6. Eight such candidates are made and the one that hangs together best as a loop is kept
   (number of notes, share of the root, few leaps, the loop join, how accents and slides
   fall).

The resulting pattern is written as TB-3PO's own data (step type, scale degree, octave).
Editing by hand, mutating with MUT, saving to and recalling from banks, and being saved
with the set all work exactly as for any other pattern. Undo goes back to the pattern
before GEN. MUT keeps TB-3PO's own behaviour even at 8 steps.

### Buttons (while this sound generator's screen is open)

| Button | Action |
| --- | --- |
| **+ / −** | Shift the whole pattern up / down one octave (up to ±4 octaves) |
| **< / >** | Move the step window (to edit the second half of 24- and 32-step patterns) |
| **Undo** | Undo the last GEN / MUT / clear |
| **X twice** | Clear the pattern (the first press shows a confirmation; press again within about 1.5 s to do it) |
| **Turn the jog wheel** | Change page |
| **Press the jog while touching a knob that has choices** | Opens that parameter's list of choices. Pick with the jog or the knob, press the jog to confirm, Back to cancel |
| **Back** | Close the screen (the sequencer keeps playing) |

While this screen is shown these buttons do not reach Move. Closing the screen returns
them to Move.

### Knobs and pages

Change pages with the jog wheel. The bar at the top of the screen shows where the current
page is.

| Page | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Synth** | **Volume** | Cutoff | Resonance | Env Mod | Decay | Accent | RAT Dist | RAT Filter |
| Steps | Density | Accent prob. | Slide prob. | Gate | | | | |
| Pattern | Density | Accent prob. | Slide prob. | Gate | Root | Scale | Length | Octaves |
| Setup | Direction | Transpose | Bank (display only) | | | | | |
| Tone | Waveform | Tuning | 303 Level | RAT on/off | RAT / RAT2 | RAT Level | Low Cut | Hi Cut |
| VCO | VCO2 Level | Tune | Fine | Wave | Hard Sync | Sync Env | VCO1 Level | |
| Devilfish | Mod on/off | Normal Decay | Accent Decay | Feedback HPF | Soft Attack | Slide Time | Shaper | |

There are also pages with the pad map (Pads) and the button list (Keys).

- The **Steps** page shows the steps large in a lane, with the first row of the Pattern
  page (Density / Accent prob. / Slide prob. / Gate) below it.
- **Density / Accent prob. / Slide prob. / Octaves** take effect from the next GEN (or
  MUT). **Gate** is the note length and takes effect at once.
- **Root / Scale** set the scale. Scale can be Minor / Phrygian / HarmMinor / MinPent /
  Dorian / Major. **Length** is 8 / 16 / 24 / 32 steps.
- **Volume** is this sound generator's final level, after RAT. It has a square-law curve
  and 0 is complete silence. After Volume there is a soft limiter that rounds off only
  what exceeds 0.7 of full scale.
- **303 Level** is the level going from the 303 into RAT (it sets how deep the distortion
  goes); **RAT Level** is the pedal's own output level. Both are separate from Volume.
- **RAT on/off** switches with a crossfade of about 6 ms.

### VCO2 and hard sync

A second oscillator (VCO2) is added to Open303's oscillator (VCO1). VCO2 is mixed with
VCO1 before the 303 filter, so Cutoff, Env Mod, Accent and Decay act on VCO2 just the
same. On a slide both glide together.

- **VCO2 Level**: VCO2's level. Default 0 (off).
- **Tune / Fine**: VCO2's pitch relative to VCO1. Tune is ±24 semitones (default +7),
  Fine ±50 cents.
- **Wave**: VCO2's waveform, saw or square.
- **Hard Sync**: when on, VCO2's phase resets every time VCO1 completes a cycle (default
  on). Tuning VCO2 above VCO1 gives the sharp harmonics typical of sync.
- **Sync Env**: the 303's main envelope raises VCO2's pitch by up to +36 semitones. It acts
  harder on accented steps and falls with the decay, giving a "sync sweep".
- **VCO1 Level**: the level of the 303's own oscillator. Turn it down for VCO2 alone.

VCO2 uses band-limited waveforms (PolyBLEP) and runs inside Open303's internal 4×
oversampling, so aliasing stays down even with heavy sync. With VCO2 Level at 0 and VCO1
Level at 1 the output is identical to the original Open303, sample for sample.

### Differences from TB-3PO (the tool version)

| TB-3PO | o303 | Why |
| --- | --- | --- |
| Bottom-row pads 4 and 5 are MIDI channel −/+ | OUT / IN (spread the phrase's notes out / pull them in) | The 303 is built in, so there is no MIDI channel; the freed pads got a new job |
| Slots A / B switched with the Track buttons | One sequencer | The voice is monophonic. The Track buttons switch Schwung slots and cannot be used by a sound generator |
| Step buttons change page | Jog only | Pressing a Step button would also reach Move's sequencer |
| Shift + X clears | X twice | Shift + Delete is used by Schwung's snapshot recall and never reaches the sound generator |
| Back pauses | Back closes the screen | A chain-slot sound generator plays for as long as Move is playing |
| State saved to one file | Saved as the slot's state | So each set and each slot has its own |

### Saving

The playing pattern, the eight banks, the sequencer settings and every sound knob are
saved automatically by Schwung as the slot's state. They come back when the set is
reopened or Move is restarted. Loading into a new slot starts from the initial pattern.

## How it works

The three upstream sources sit **unmodified** in `vendor/` (checked with
`vendor/SHA256SUMS`) and are joined by thin wrappers around them. The author name in the
RAT files reads "Hiro Ikezawa" (comments and metadata only; the code is as imported).

| File | What it wraps | What it changes |
| --- | --- | --- |
| `dsp/tb_engine.c` | TB-3PO's `tb3po.c` | Sends MIDI to an internal function instead of another slot. Everything a chain sound generator does runs on the audio callback, so it uses no memory allocation, threads or state files |
| `dsp/voice303.cpp` | The Schwung 303 plugin's `plugin.cpp` (Open303) | Takes the float output before its own drive stage. Swaps the engine for `Open303Sync` below |
| `dsp/open303_sync.h` | Open303 (a derived class) | Replaces only the per-sample processing, adding VCO2 and hard sync |
| `dsp/rat_stage.c` | The RAT pedal's `rat.c` | Removes the stereo effect wrapper and runs it as a single mono stage |
| `dsp/acid8.c` | — | The 8-step pattern generation used by GEN when Length is 8 |
| `dsp/tr_plugin.c` | — | The sound generator itself: parameters, state saving, signal path |

**How accents and slides are passed on**: the same MIDI that TB-3PO sends to a 303 in the
neighbouring slot is handed to Open303 through a single internal function (`seq_midi()`
in `tr_plugin.c`). Velocity 118 is an accent (Open303 treats 100 and above as an accent)
and 72 a normal note. A slide sends the next note before releasing the previous one, and
Open303 joins them smoothly as legato. A rest is a note-off.

**Timing**: steps advance on Move's MIDI clock (every 6 pulses of 24 per beat), and step 1
sounds on the first pulse after Play. If no clock arrives, an internal timer drives it.
`tests/test_engine.c` runs the stand-alone TB-3PO plugin and the built-in engine side by
side and checks that the same MIDI comes out in the same audio block.

## Building and testing

```bash
scripts/test.sh                  # native build, all tests, and the Move build
scripts/build.sh arm64           # creates dist/o303-module.tar.gz
scripts/install.py http://<Move's IP>:7700   # install through Schwung Manager
scripts/vendor.sh                # refetch vendor/ from the upstream checkouts
scripts/vendor.sh --verify       # check vendor/ is unmodified (runs automatically at build)
```

- The build needs an aarch64 cross compiler (`aarch64-linux-gnu-gcc` / `g++`). The Schwung
  plugin API header is bundled in `third_party/schwung/` (from Schwung 1.7.3).
- The UI tests use Schwung's page engine and drawing harness, so they need a Schwung
  checkout (`SCHWUNG_ROOT`, or else `../schwung` next to this repository). If none is
  found only the UI tests are skipped. Point `TR_SHARED` at a copy of the device's
  `shared/` to check against the same page engine as the device as well.

| Test | What it checks |
| --- | --- |
| `tests/test_engine.c` | The built-in sequencer emits the same MIDI at the same timing as the stand-alone TB-3PO (including GEN at 16 / 24 / 32 steps and MUT at 8 steps). OUT / IN move exactly one note, in the right direction, to the neighbouring scale note; Undo reverts them; they stay within Octaves. GEN at 8 steps writes with the new method, GEN while playing leaves no hanging notes, saving to / recalling from the eight banks, state saving. Corrupt state text does not crash (with ASan / UBSan) |
| `tests/test_acid8.c` | The 8-step generation keeps every promise across 40,000 settings (step 1 is the root, no notes outside the scale, no slides into rests, not every note accented or slid, ...). Density / Accent / Slide / Octaves each have an effect, and the five styles show their character |
| `tests/test_vco.cpp` | VCO2 at its defaults is identical to the original Open303 sample for sample, the level calibration, sync locking to VCO1's period, the Sync Env sweep, no runaway values in any setting |
| `tests/smoke.py` | Play / stop with the real `dsp.so`, tempo following, accents, slides, RAT's position, Volume, extreme settings, state save and restore. GEN at 8 steps and the banks, and that state saved by v0.3.0 (`tests/fixtures/state_v030.json`) still loads |
| `tests/test_plugin.c` | Runs the whole sound generator for a long time with random operations, with no memory errors or abnormal values (with ASan / UBSan) |
| `tests/test_ui.mjs` | The behaviour of the screen, knobs, pads and buttons, and that pad operations give the same results as the real TB-3PO `ui.js` |

## Limitations and notes

- External note input is ignored. Only the built-in sequencer plays the 303.
- Loaded while playing, it starts running from the next 1/16 note. The next Play lines up
  the start.
- The LEDs of the buttons used while the screen is shown (+ − < > Undo X) are not lit.
- This sound generator's screen has no Schwung "My Presets" page. Saving named presets or
  taking a pattern to another slot is not possible.
- Notes on the upstream code (`vendor/` is not modified):
  - Open303's `MipMappedWaveTable::initPrototypeTable()` writes 4 zeros past the end of an
    array. It does no harm, but it is built with `-fno-aggressive-loop-optimizations` so
    that optimisation cannot turn it into a problem.
  - Open303 allocates memory at creation and for its list of held notes.
