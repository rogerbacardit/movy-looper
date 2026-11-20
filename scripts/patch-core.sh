#!/usr/bin/env bash
# Derive build/mark_core6.c from the untouched src/mark_core.c.
# The core's per-track key router hard-codes tracks '1'..'5' (two places);
# this widens it to MARK_TRACKS so t6_* keys reach track 6.
# src/mark_core.c itself is never modified.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
sed "s/key\[1\] <= '5'/key[1] <= '0' + MARK_TRACKS/" src/mark_core.c > build/mark_core6.c
n=$(grep -c "key\[1\] <= '0' + MARK_TRACKS" build/mark_core6.c)
[ "$n" = 2 ] || { echo "patch-core: expected 2 replacements, got $n" >&2; exit 1; }
