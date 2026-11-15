#!/usr/bin/env python3
"""Builds res/app.ico (the program's icon) and assets/icon-256.png from assets/icon.svg.

    python3 tools/make_icon.py        # needs Pillow and rsvg-convert (librsvg); without rsvg-convert the small sizes are scaled down from the 256 px picture

The icon file holds seven sizes. The six small ones are stored as plain 32-bit bitmaps and only the 256 px one as PNG, which is what Windows
expects: a PNG-compressed frame at every size makes the resource section of the executable look packed to tools/verify_pe.py (high entropy),
and a bitmap frame at 256 px would be a quarter of a megabyte for no gain.
"""
import io
import os
import shutil
import struct
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(ROOT, "assets", "icon.svg")
PNG256 = os.path.join(ROOT, "assets", "icon-256.png")
ICO = os.path.join(ROOT, "res", "app.ico")
SIZES = (16, 24, 32, 48, 64, 128, 256)


def render(size):
    """The icon at `size` px, as an RGBA picture (from the SVG when rsvg-convert exists, otherwise scaled from the 256 px picture)."""
    if shutil.which("rsvg-convert"):
        png = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), SVG], check=True, stdout=subprocess.PIPE).stdout
        return Image.open(io.BytesIO(png)).convert("RGBA")
    base = Image.open(PNG256).convert("RGBA")
    return base if size == 256 else base.resize((size, size), Image.LANCZOS)


def bmp_frame(im):
    """One ICO image as a BITMAPINFOHEADER + bottom-up BGRA pixels + an all-zero AND mask (the alpha channel does the masking)."""
    w, h = im.size
    px = im.load()
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            rows += bytes((b, g, r, a))
    mask_row = ((w + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(rows) + mask_row * h, 0, 0, 0, 0)
    return header + bytes(rows) + bytes(mask_row * h)


def png_frame(im):
    out = io.BytesIO()
    im.save(out, format="PNG", optimize=True)
    return out.getvalue()


def main():
    frames = []
    for s in SIZES:
        im = render(s)
        if s == 256:
            im.save(PNG256, optimize=True)
        frames.append((s, png_frame(im) if s == 256 else bmp_frame(im)))
    out = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
    offset = 6 + 16 * len(frames)
    for s, data in frames:
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in frames:
        out += data
    with open(ICO, "wb") as f:
        f.write(out)
    print("wrote", ICO, len(out), "bytes,", len(frames), "sizes")


if __name__ == "__main__":
    sys.exit(main())
