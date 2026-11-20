#!/usr/bin/env bash
# Cross-compile Movy Looper (sound_generator) for the Ableton Move.
# Output: build/movy-looper-module.tar.gz containing movy-looper/{module.json,dsp.so}
# Uses a native aarch64-linux-gnu-gcc if present (brew), otherwise Docker.
#
# Install on the Move (by hand, not done by this script):
#   tar -xzf movy-looper-module.tar.gz
#   scp -r movy-looper ableton@move.local:/data/UserData/schwung/modules/sound_generators/
#   (owner ableton:users; then restart Schwung / rescan modules)
# Per-track memory: MARK_SECONDS=30 (default) .. 60.
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/patch-core.sh
SECS="${MARK_SECONDS:-30}"
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Iinclude -Isrc -DMARK_TRACKS=6 -DMARK_MAX_SECONDS=$SECS"
SRCS="build/mark_core6.c src/looper_synth.c"
D=build/modules/sound_generators/movy-looper
rm -rf "$D"; mkdir -p "$D"
cp module.json "$D/"
if command -v aarch64-linux-gnu-gcc &>/dev/null; then
    aarch64-linux-gnu-gcc $CFLAGS $SRCS -o "$D/dsp.so" -lm -lpthread -ldl
    COPYFILE_DISABLE=1 tar -czf build/movy-looper-module.tar.gz -C build/modules/sound_generators movy-looper
elif command -v docker &>/dev/null && docker info &>/dev/null; then
    IMAGE=smack-build
    if ! docker image inspect "$IMAGE" &>/dev/null; then
        docker build -t "$IMAGE" - <<'DOCKER'
FROM debian:bookworm
RUN apt-get update && apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu file && rm -rf /var/lib/apt/lists/*
DOCKER
    fi
    docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c "
        set -e
        aarch64-linux-gnu-gcc $CFLAGS $SRCS -o $D/dsp.so -lm -lpthread -ldl
        tar --owner=0 --group=0 -czf build/movy-looper-module.tar.gz -C build/modules/sound_generators movy-looper"
else
    echo "No aarch64-linux-gnu-gcc and no running Docker: cannot cross-compile." >&2; exit 1
fi
file "$D/dsp.so" || true
tar -tzf build/movy-looper-module.tar.gz
md5 build/movy-looper-module.tar.gz 2>/dev/null || md5sum build/movy-looper-module.tar.gz
