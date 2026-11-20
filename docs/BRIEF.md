# Movy Looper: brief

A 6-track live looper that loads **inside a Movy track** as a Schwung `sound_generator` (plugin api v2, `audio_in`). It takes ideas from the Boss RC-505 (faders and Rec Action), from Loopex (arm with threshold, hold to clear) and from Mark. The audio engine is Mark's.

- Module id: `movy-looper`
- Display name: `Looper`
- Abbreviation: `LOOP`

## Base and license
- Engine: `mark_core.c/.h` from https://github.com/timncox/schwung-mark (MIT, © Tim Cox). Copy it into `src/` **without modifying it**, and keep `LICENSE-mark` and the credits.
  - Build it with `-DMARK_TRACKS=6`.
- Reference shim already written: `~/movy-twister-patch/schwung-mark-movy/src/mark_synth.c` (module `mark-loop`). It is the starting point.
- **Loopex: ideas only, never its code.** It has no license.

## Pads (Movy drum layout, 4×8, pad 1 = bottom left)
Row 1 is the bottom row. Notes are declared as drum voices in the hierarchy (`pad_layout:"drums"`, fixed `note` per voice), so they do not depend on Movy's scale.

| | col 1 (MASTER) | col 2–7 (tracks 1–6) | col 8 (REC SETTINGS) |
|---|---|---|---|
| row 4 | UNDO/REDO | FX on/off | Count-in on/off |
| row 3 | ARM auto-rec (toggle) | MUTE (keeps running silently, re-enters in place) | Rec Action: REC→PLAY / REC→DUB |
| row 2 | STOP ALL | STOP (hold 1 s = CLEAR, UNDO restores) | Fixed length: free/1/2/4/8 bars (cycles) |
| row 1 | ALL START/STOP | REC→PLAY→DUB→PLAY… (per Rec Action) | Quantize on/off |

- **REVERSE:** hold the MASTER pad in row 3 (ARM) and tap a track's row-1 pad. That pad works as a modifier: a short tap without another pad still toggles ARM.
- **Pad ↔ note table:** a single array in the code, so it is easy to change. Verify Movy's pad → voice order (`schwung-movy/src/keyboard/drum-grid.ts`, banks 4×4: v01–16 on the left, v17–32 on the right).

## Auto-rec (in the shim, not in the core)
- With ARM on, tapping REC on an empty track leaves it **ARMED**.
- When the input peak crosses `threshold`, the shim sends the core `tN_btn` (with quantize on, the core does its usual quantization).
- Configurable threshold, default 0.08 (Loopex's value).

## Knobs (hierarchy pages; also in `capabilities.ui_hierarchy` of module.json)
The cause of today's "0 params" is that Movy reads params from module.json. Duplicate the hierarchy there and keep `get_param("ui_hierarchy")`. Working example: `schwung-movy/browser-test/fixtures/module-contracts/sound_generator--smack-in.json`.

1. **Root page = mixer** (what shows without touching anything): K1–K6 volume of tracks 1–6 · K7 input/monitor volume · K8 master.
2. **Track page** (track selected with the last pad touched): FX type · FX amount · pan · reverse · speed (½ / 1 / 2 if the core allows it, otherwise leave it out) · one-shot.
3. **Master page:** threshold · quantize · fixed length · Rec Action · count-in · BPM override · monitor on/off.
4. **Read-only state:** `tN_state` enums (Empty/Armed/Rec/Play/Dub/Stop/Mute), so the screen shows the state of each track. A sound_generator cannot light pads in Movy, so the screen is the only feedback.

## Out of scope for this phase
- Pad LEDs: needs a separate patch to Movy (a per-tick `pad_leds` param). Phase 2.
- Loopex-style drift heads and punch-in FX. Phase 3.

## Risks to verify
- MIDI clock / BPM reaching a sound_generator inside Movy (`on_midi` F8/FA/FC or `host->get_bpm`). Add a log line on the first clock received.
- RAM: 6 tracks × 60 s stereo ≈ 74 MB. Make `MARK_MAX_SECONDS` configurable; default 30 s.

## Deliverables
- `src/looper_synth.c` (shim), `module.json`, `Makefile` (host test build and ARM build, copying the Mark mechanism: `scripts/build-synth.sh`).
- Host tests: smoke test (load, process with a sine input, arm + threshold → REC, cycle, mute, reverse, clear/undo, params).
- `README.md` in Spanish for the user: pad layout (ASCII), knobs, installation.
- Package `build/movy-looper-module.tar.gz` (folder `movy-looper/` with `dsp.so` and `module.json`).
- Install path on the Move: `/data/UserData/schwung/modules/sound_generators/movy-looper` (owner `ableton:users`).
