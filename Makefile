CC ?= cc
# Per-track capacity target in seconds (6 tracks x stereo int16 + undo buffer); 120 s = ~21 MB/track, ~148 MB total.
MARK_SECONDS ?= 120
CFLAGS = -O2 -g -Wall -Wextra -Iinclude -Isrc -DMARK_TRACKS=6 -DMARK_MAX_SECONDS=$(MARK_SECONDS)

ifeq ($(shell uname -s),Darwin)
SHARED_FLAGS = -dynamiclib
else
SHARED_FLAGS = -shared
endif

.PHONY: test-synth arm-synth module-json clean

test-synth:
	@mkdir -p build
	$(CC) $(CFLAGS) -fPIC $(SHARED_FLAGS) src/mark_core.c src/looper_synth.c -o build/looper-606-host.so -lm -lpthread -ldl
	$(CC) $(CFLAGS) test/synth_smoke.c -o build/synth_smoke -ldl -lm
	./build/synth_smoke build/looper-606-host.so build/hierarchy.json
	python3 scripts/check-hierarchy.py build/hierarchy.json module.json

module-json:
	python3 scripts/check-hierarchy.py build/hierarchy.json module.json --write

arm-synth:
	MARK_SECONDS=$(MARK_SECONDS) ./scripts/build-synth.sh

clean:
	rm -rf build
