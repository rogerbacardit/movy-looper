CC ?= cc
# Per-track capacity target in seconds (6 tracks x stereo int16 + undo buffer).
MARK_SECONDS ?= 30
CFLAGS = -O2 -g -Wall -Wextra -Iinclude -Isrc -DMARK_TRACKS=6 -DMARK_MAX_SECONDS=$(MARK_SECONDS)

ifeq ($(shell uname -s),Darwin)
SHARED_FLAGS = -dynamiclib
else
SHARED_FLAGS = -shared
endif

.PHONY: test-synth arm-synth module-json clean

build/mark_core6.c: src/mark_core.c scripts/patch-core.sh
	./scripts/patch-core.sh

test-synth: build/mark_core6.c
	@mkdir -p build
	$(CC) $(CFLAGS) -fPIC $(SHARED_FLAGS) build/mark_core6.c src/looper_synth.c -o build/movy-looper-host.so -lm -lpthread -ldl
	$(CC) $(CFLAGS) test/synth_smoke.c -o build/synth_smoke -ldl -lm
	./build/synth_smoke build/movy-looper-host.so build/hierarchy.json
	python3 scripts/check-hierarchy.py build/hierarchy.json module.json

module-json:
	python3 scripts/check-hierarchy.py build/hierarchy.json module.json --write

arm-synth:
	MARK_SECONDS=$(MARK_SECONDS) ./scripts/build-synth.sh

clean:
	rm -rf build
