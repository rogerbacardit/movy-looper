#!/usr/bin/env bash
# Cross-compile Looper 606 (sound_generator) for the Ableton Move.
# Output: build/looper-606-module.tar.gz containing looper-606/{module.json,dsp.so}
# Uses a native aarch64-linux-gnu-gcc if present (brew), otherwise Docker.
#
# Install on the Move (by hand, not done by this script):
#   tar -xzf looper-606-module.tar.gz
#   scp -r looper-606 ableton@move.local:/data/UserData/schwung/modules/sound_generators/
#   (owner ableton:users; then restart Schwung / rescan modules)
# Per-track memory: MARK_SECONDS=120 (default) .. 30.
set -euo pipefail
cd "$(dirname "$0")/.."
SECS="${MARK_SECONDS:-120}"
CFLAGS="-O3 -g -shared -fPIC -Wall -Wextra -Iinclude -Isrc -DMARK_TRACKS=6 -DMARK_MAX_SECONDS=$SECS"
SRCS="src/mark_core.c src/looper_synth.c src/gfx1.c src/wire3d.c src/perform.c src/perform_ui.c"
D=build/modules/sound_generators/looper-606
rm -rf "$D"; mkdir -p "$D"
cp module.json "$D/"
if command -v aarch64-linux-gnu-gcc &>/dev/null; then
    aarch64-linux-gnu-gcc $CFLAGS $SRCS -o "$D/dsp.so" -lm -lpthread -ldl
    COPYFILE_DISABLE=1 tar -czf build/looper-606-module.tar.gz -C build/modules/sound_generators looper-606
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
        tar --owner=0 --group=0 -czf build/looper-606-module.tar.gz -C build/modules/sound_generators looper-606"
else
    echo "No aarch64-linux-gnu-gcc and no running Docker: cannot cross-compile." >&2; exit 1
fi
file "$D/dsp.so" || true
tar -tzf build/looper-606-module.tar.gz
md5 build/looper-606-module.tar.gz 2>/dev/null || md5sum build/looper-606-module.tar.gz
