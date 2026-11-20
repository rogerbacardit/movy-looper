#!/usr/bin/env bash
# PERFORM graphics must never run from the audio thread: gfx1 / perform_ui are only
# reachable from get_param. Fail if the engine files or the render path mention them.
set -euo pipefail
cd "$(dirname "$0")/.."
bad=0
if grep -n "gfx_\|perf_ui\|gfx1\|perform_ui" src/perform.c src/mark_core.c src/mark_core.h src/perform.h; then bad=1; fi
# the render path of the shim: from tick_block to the end of gen_render
if awk '/^static void tick_block/,/^static void gen_on_midi/' src/looper_synth.c | grep -n "gfx_\|perf_ui"; then bad=1; fi
if [ "$bad" = 1 ]; then echo "FAIL: drawing is reachable from process()" >&2; exit 1; fi
echo "no drawing in process(): ok"
