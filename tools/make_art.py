#!/usr/bin/env python3
"""Draws the installer bitmaps (installer/welcome.bmp, installer/header.bmp) from assets/icon.svg's palette.

    python3 tools/make_art.py            # needs Pillow and the Inter font (/usr/share/fonts/opentype/inter)

The bitmaps are committed, so a normal build does not need this script; it is here so the artwork can be redrawn the same way.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "installer")
FONT_DIRS = ["/usr/share/fonts/opentype/inter", "/usr/share/fonts/truetype/inter", os.path.join(ROOT, "assets")]

BG_TOP = (0x2A, 0x2B, 0x29)
BG_BOTTOM = (0x14, 0x15, 0x15)
BOLT_A = (0xF0, 0xBF, 0xA6)
BOLT_B = (0xD9, 0x8F, 0x70)
TEXT = (0xF2, 0xEF, 0xEA)
MUTED = (0x9A, 0x97, 0x92)
BOLT = [(146, 34), (74, 138), (116, 138), (102, 222), (182, 108), (138, 108)]   # the polygon of assets/icon.svg, 256-unit grid


def font(name, size):
    for d in FONT_DIRS:
        for ext in (".otf", ".ttf"):
            p = os.path.join(d, name + ext)
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
    sys.exit("font %s not found (looked in %s)" % (name, ", ".join(FONT_DIRS)))


def gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h), top)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def bolt_layer(size):
    """The lightning bolt of the icon as an RGBA layer with the peach gradient, `size` pixels square."""
    ss = 4                                           # supersample for smooth edges
    n = size * ss
    mask = Image.new("L", (n, n), 0)
    pts = [(x / 256 * n, y / 256 * n) for x, y in BOLT]
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    grad = Image.new("RGB", (n, n))
    gp = grad.load()
    for y in range(n):
        for x in range(n):
            t = (x + y) / (2 * n)
            gp[x, y] = tuple(int(BOLT_A[i] + (BOLT_B[i] - BOLT_A[i]) * t) for i in range(3))
    layer = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    layer.paste(grad, (0, 0), mask)
    return layer.resize((size, size), Image.LANCZOS)


def welcome():
    w, h = 164, 314
    img = gradient(w, h, BG_TOP, BG_BOTTOM).convert("RGBA")
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((-30, 20, w + 30, 190), fill=(0xE2, 0xA3, 0x87, 70))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(34)))
    bolt = bolt_layer(110)
    img.alpha_composite(bolt, ((w - 110) // 2, 52))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((18, 196, w - 18, 199), radius=1, fill=(0xE2, 0xA3, 0x87, 150))
    d.text((18, 216), "Control Center", font=font("Inter-SemiBold", 18), fill=TEXT)
    d.text((18, 244), "ADVANCED", font=font("Inter-Medium", 11), fill=BOLT_B)
    d.text((18, 284), "GM5TG7 control centre", font=font("Inter-Regular", 10), fill=MUTED)
    img.convert("RGB").save(os.path.join(OUT, "welcome.bmp"))


def header():
    w, h = 150, 57
    img = Image.new("RGB", (w, h), (255, 255, 255))
    badge = gradient(46, 46, BG_TOP, BG_BOTTOM).convert("RGBA")
    mask = Image.new("L", (46, 46), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 45, 45), radius=12, fill=255)
    badge.putalpha(mask)
    badge.alpha_composite(bolt_layer(34), (6, 6))
    img.paste(badge, (w - 46 - 6, 5), badge)
    img.save(os.path.join(OUT, "header.bmp"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    welcome()
    header()
    print("wrote", os.path.join(OUT, "welcome.bmp"), "and", os.path.join(OUT, "header.bmp"))
