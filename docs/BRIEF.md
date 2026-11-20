# Looper 606: brief

A 6-track live looper that loads **inside a Movy track** as a Schwung `sound_generator` (plugin api v2, `audio_in`). It takes ideas from the Boss RC-505 (faders and Rec Action), from Loopex (arm with threshold, hold to clear) and from Mark. The audio engine is Mark's.

- Module id: `looper-606`
- Display name: `Looper 606`
- Abbreviation: `L606`

## Base and license
- Engine: `mark_core.c/.h` from https://github.com/timncox/schwung-mark (MIT, © Tim Cox). From phase 1.1 we OWN a fork (MIT): keep `LICENSE-mark` and the (c) Tim Cox credit, marked "modified for Looper 606". Changes are tagged `L606` in `src/mark_core.c`: native `MARK_TRACKS=6` (no sed patch), undoable CLEAR, per-track varispeed, 120 s default per track.
- Reference shim already written: `~/movy-twister-patch/schwung-mark-movy/src/mark_synth.c` (module `mark-loop`). It is the starting point.
- **Loopex: ideas only, never its code.** It has no license.

## Pads (Movy drum layout, 4×8, pad 1 = bottom left)
Row 1 is the bottom row. Notes are declared as drum voices in the hierarchy (`pad_layout:"drums"`, fixed `note` per voice), so they do not depend on Movy's scale.

| | col 1 (MASTER) | col 2–7 (tracks 1–6) | col 8 (REC SETTINGS) |
|---|---|---|---|
| row 4 | UNDO/REDO | FX on/off | Count-in on/off |
| row 3 | ARM auto-rec (toggle) | MUTE (keeps running silently, re-enters in place) | Rec Action: REC→PLAY / REC→DUB |
| row 2 | STOP ALL | STOP (tap = stop; tap on a stopped track = CLEAR, UNDO restores) | Fixed length: free/1/2/4/8 bars (cycles) |
| row 1 | ALL START/STOP | REC→PLAY→DUB→PLAY… (per Rec Action) | Quantize on/off |

- **REVERSE:** tap the MASTER pad in row 3 (ARM) and, within 1 s, a track's row-1 pad (ARM is restored). All gestures are note-on only.
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
- Package `build/looper-606-module.tar.gz` (folder `looper-606/` with `dsp.so` and `module.json`).
- Install path on the Move: `/data/UserData/schwung/modules/sound_generators/looper-606` (owner `ableton:users`).

## Entrada de audio en Movy standalone (phase 1.1 findings)

- Movy on the device runs as the standalone `movy-host` (`/data/UserData/schwung/modules/tools/movy/movy-host`), not under the Schwung shim. Its vtable exposes the SPI page (`schwung-movy/host/vtable.c:59,65`, `audio.c:157`); a chain module reads `mapped_memory + audio_in_offset` exactly like the stock `linein` module (`schwung/src/modules/sound_generators/linein/linein.c:1016`). The chain host forwards that vtable unchanged (`chain_host.c:108`), so there is no other input mechanism for a sound_generator inside a chain.
- Movy's own docs only *assume* audio-in is live in standalone (`schwung-movy/plans/2026-10-09-standalone-migration.md:586`, `docs/standalone/inventory.md:79`); nothing in `host/` reads or routes it. Schwung documents that input routing "depends on the last selected input in the stock Move interface" (`schwung/docs/API.md:696`) and that the XMOS routing is set by Move firmware SysEx (`docs/SPI_PROTOCOL.md`, `37 12`). With the Move firmware not running, nobody configures it.
- To discriminate: load the stock `linein` in a Movy track; if it is silent too, the fix belongs in `movy-host` (send the XMOS input-select SysEx at start-up), not in this module.
- This module now logs the mechanism and the peak after 1 s, and still plays loops with no input.
- Movy's idle gate (`chain_idle.rs`: SLEEP_AFTER 344 blocks, probe every 172) parks a silent synth and ignores a synth's `requires_continuous_processing`; the module keeps itself awake with a -74 dBFS dither while tracks are in use.

## v0.4.0: step buttons, clip start, reverse playhead, persistence

- **Reverse playhead:** `mark_track_wave` maps the playhead through the same buffer index the render reads (`play_index`: reverse mirror, then clip start), so the display runs right-to-left on a reversed track and follows varispeed and a moved start.
- **Step buttons = length:** `rec_len` is now a number of bars 0..16 (0 = free). Column-8 pad cycles 1/2/4/8/free from any value; step N sets N bars (again = free); `step_leds` shows it.
- **Clip start:** per-track `off` (frames) in the core. Reads and dub writes use `(pos + off) mod len`; undo capture indexes the buffer frame; CLEAR/UNDO parks and restores `off`; trim/len16 re-normalise it. Refused in REC/DUB/swap. The shim turns `loop_knob "N:delta[:fine]"` into a grid move (1/16, fine 1/64 note).
- **Persistence:** see README "Persistencia de los loops". Core: `mark_persist_sig/save/load` (WAV + `loops.json`, no `io_busy` on save, never on the audio thread); shim: one worker thread, loop id in the `state` blob. Mark's `session_dir` is no longer set (its `state` getter would `fopen()` session slots on Movy's audio callback).
