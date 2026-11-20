#!/usr/bin/env python3
"""PBM (P4) -> PNG, scaled up, for looking at the OLED frames. usage: frames2png.py <dir> [scale]"""
import sys, glob, os
from PIL import Image
d = sys.argv[1]; k = int(sys.argv[2]) if len(sys.argv) > 2 else 6
for p in sorted(glob.glob(os.path.join(d, "*.pbm"))):
    raw = open(p, "rb").read()
    body = raw.split(b"\n", 2)[2]
    im = Image.new("1", (128, 64))
    for y in range(64):
        for x in range(128):
            if (body[y * 16 + (x >> 3)] >> (7 - (x & 7))) & 1: im.putpixel((x, y), 1)
    im = im.convert("RGB").resize((128 * k, 64 * k), Image.NEAREST)
    # OLED look: lit pixels white on black with a thin gap grid
    im.save(p[:-4] + ".png")
