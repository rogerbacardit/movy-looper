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
| row 4 | SHIFT (hold) | track selector (PISTA view) | Count-in on/off |
| row 3 | ARM auto-rec (toggle) | MUTE (keeps running silently, re-enters in place) | Rec Action: REC→PLAY / REC→DUB |
| row 2 | STOP ALL | STOP (tap = stop; tap on a stopped track = CLEAR, UNDO restores) | Fixed length: free/1/2/4/8 bars (cycles) |
| row 1 | ALL START/STOP | REC→PLAY→DUB→PLAY… (per Rec Action) | Quantize on/off |

- **REVERSE:** SHIFT + a track's row-1 pad (v0.6.0); ARM held + pad stays as an alias. All gestures are note-on only.
- **Pad ↔ note table:** a single array in the code, so it is easy to change. Verify Movy's pad → voice order (`schwung-movy/src/keyboard/drum-grid.ts`, banks 4×4: v01–16 on the left, v17–32 on the right).

## Auto-rec (in the shim, not in the core)
- With ARM on, tapping REC on an empty track leaves it **ARMED**.
- When the input peak crosses `threshold`, the shim sends the core `tN_btn` (with quantize on, the core does its usual quantization).
- Configurable threshold, -70..-6 dBFS, default -42 dB (~260/32768; device line-in noise floor peaks ~-66 dB, quiet playing ~-40..-30 dB).

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
- **Clip start (v0.4.1, real trim):** per-track `off` (buffer frame) is the start of the clip like an Ableton clip: the loop plays buffer frames `[off, len)` only and jumps back to `off`; audio left of `off` never sounds. `len` stays the END point, effective loop length is `len - off` (`reg_len`); `pos` runs 0..len-off, `play_index` = `off + (rev ? mirror : ip)` inside the region. Dub writes/undo capture use the same region (undo saves `undo_roff/rlen`). `off` is clamped to `len - min_region` (1/16 note, 256 frames if the grid is unknown); setlen/trim/undo re-clamp it and the playhead. Knob moves clamp at 0 and at the end (no wrap). Saved `off` keeps its meaning, so old saves load. (Before 0.4.1 `off` rotated the loop.) CLEAR/UNDO parks and restores `off`. Refused in REC/DUB/swap. The shim turns `loop_knob "N:delta[:fine]"` into a grid move (1/16, fine 1/64 note).
- **Persistence:** see README "Persistencia de los loops". Core: `mark_persist_sig/save/load` (WAV + `loops.json`, no `io_busy` on save, never on the audio thread); shim: one worker thread, loop id in the `state` blob. Mark's `session_dir` is no longer set (its `state` getter would `fopen()` session slots on Movy's audio callback).

## v0.6.0: selector de pista, vista PISTA, SHIFT

- Row 4 cols 2-7 = track selector (tap = select and knobs become that track's; tap again = MIX). While selected, `knob_capture` = 1 and `knob_delta "N:d"` drives: K1 IN (start), K2 OUT (end), K3 vol, K4 pan, K5 FX type (OFF first, 3 clicks/step), K6 FX amount, K7 speed, K8 reverse. Toast `T3 IN +8/16`, header `PISTA 3 <last readout>`. `cur_track` is locked to the selection while PISTA is active; pedals 3/4 move both.
- Col 1 row 4 = looper SHIFT (hold). SHIFT + K1/K2 fine; + row 4 = FX on/off; + row 1 = reverse; + row 3 = SOLO (restores previous mutes); + col 8 row 4 = count-in 1/2 bars (not persisted), + col 8 row 3 = threshold cycle -54..-30 dB. The MODE pad and its START/END/FX/PAN modes are gone. ARM + pad = reverse stays as an alias.
- Selection, view and solo are not persisted.

## v0.6.1: 7 velocidades, zoom de onda

- **Speed:** core `spd` is now an index 0..6 = 1/4, 1/2, 3/4, 1, 1.5, 2, 4x (`MK_SPD_1X` = 3, default 1x; `SPD_RATE` table). Render/pos/wrap/one-shot/playhead already worked on a fractional rate (linear interpolation, wrap at region end, reverse mirrors the head), so only the table changed; no dub when `spd != 1x` or reverse. Pending-action sync still fires on the base track's region boundaries (a non-1x base is only the fallback when no 1x track plays). `trk_spd` enum = 7 options, default 3 (ui_hierarchy + module.json updated). **State:** core session JSON keeps `w<N>` (old 0/1/2 = nearest of 1/2x,1x,2x, written for downgrade) and adds `q<N>` (the index, wins on load; old files without `q` map w 0/1/2 -> 1/3/5). Shim state: new `l6sq` CSV of indices; old `l6sp` (0/1/2) is read as 1/3/5 only when `l6sq` is absent.
- **Zoom (build_display):** `view_window()` picks the buffer window [lo,hi): while IN/OUT is adjusted (+2.5 s, `zoom_until`) 2 bars (1/2 bar for fine) centred on `off`/`len`, clamped to [0, max(len, full)), whole buffer when the clip is not longer than the window; else PISTA = whole buffer with IN/OUT markers, MIX = [off, len). Peaks come from `mark_track_wave_win` (UI-thread `get_param display` only, cached on window/track/state/dirty/timer, never on the audio thread); markers and bar/beat/phrase ticks are computed in window coordinates (ticks counted from IN); header gets ` ZOOM`. `seq` bumps when the window, peaks or ticks change. The playhead has no "hidden" value in the display format, so outside the window it is pinned to the left/right edge it left through. REC/empty keep the old whole-clip view.

## v0.12.0: DUB rojo, Delete+selector solo borra audio, paginas de FX, automatizacion de knobs

- **DUB rojo:** the row-1 pad of a track in DUB was `L_PURPLE` by design (v0.5 colour rules); it is now `L_RED` like REC (REC = solid red, waiting on the grid = red blink).
- **Delete + selector (fila 4):** was `reset_track` (audio + level/pan/FX/mute/SPEED/REV + its PERFORM effects), which felt like "delete everything". Now it is `clear_track` (audio only, undoable) like Delete + any pad of the column. The full RESET moved to **SHIFT(pad) + Delete + selector**. There is no clear-all function.
- **Paginas de FX:** `PF_NFX` = 17 (new effect 17 = SIDECHAIN, chain slot between GATE and KILL: RATE / DEPT / CURV), `PF_BANK` = 16 per page, max 2 pages (32-bit masks). `<` / `>` flip the page **only while the PERFORM page is showing** (SHIFT held or PERFORM mode): module param `step_bank` (hierarchy key `step_bank_param`): get = page, or -1 when not on the PERFORM page, set = +1/-1. Movy (`seq/router.ts`) asks first and falls back to its own bar navigation on -1, so nothing is lost outside PERFORM (conflict: Movy used `<` `>` for bar navigation / nudging held steps; that stays everywhere except the PERFORM page). A page change shows `FX PAGE n/2`; step LEDs and the grid screen follow the page; a step released after a page flip releases the effect that was pressed (`down_fx`). Needs the host UI `ui-module-leds-v11.js` (module-leds branch, schwung-movy commit "step_bank_param"); without it the module works, just no page flip.
- **Automatizacion de knobs (K1-K3 de un FX PERFORM):** per effect and knob, `PF_AU_N` = 64 steps over the base loop region (`au[fx][knob][64]`, 255 = empty), step = playhead / loop length of the base playing track. It records while **a track is in REC/DUB or AUTO-REC is on**, and only the knob being turned (touch window ~0.4 s): every step passed since the last write is filled with the current value. A knob not being turned plays the stored curve back each audio block (pct is overwritten, so the screen shows it moving). Only effects that are on. No loop playing = no automation. Not persisted. Engine change (minimal, tagged L606): `perf_block_begin` gets `loop_len` and a "recording" flag from `mark_core.c`.
  - **AUTO-REC on/off:** SHIFT + ARM pad (ARM pad blinks red while on). Status `AUTO-REC ON/OFF`.
  - **Borrar:** Delete + knob (Movy sends `knob_delta` fine=2) or ARM held + knob wipes that knob's curve (`AUTO CLR LPF CUT`). No clear-all gesture yet (`perf_auto_clear_all` exists in code).
- **Columna 8 (REC SETTINGS) en blanco:** count-in, Rec Action, fixed length, quantize: bright white = on / not default (count-in on, Rec>Dub, fixed length set, quantize on), dim white = off.
- **Pista 5 (columna 6):** was off-white (122/123/124), which clashed with the white column 8 and the REC red; now Light Magenta (palette 26, dim 115, dark 116), clearly apart from the violet of track 4 and from red/white.
