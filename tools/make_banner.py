#!/usr/bin/env python3
"""Draws the README banners (docs/images/banner.png and docs/images/banner.tr.png) from the program's palette and two of its screenshots.

    python3 tools/make_banner.py       # needs Pillow and the Inter font; reads docs/images/hero-custom.png and overview.png (tr/ for Turkish)

The pictures in the banner are screenshots of the built-in simulator (the top bar of each says SIMULATION), and the banner says so too.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_art import ACCENT, BG_BOTTOM, BG_TOP, BOLT_B, MUTED, TEXT, bolt_layer, font, gradient  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "docs", "images")
W, H = 1600, 580
PILL_TEXT = (0xFF, 0xC9, 0xA3)       # the accent, lightened, for text on the translucent pills
NOTE = (0x7A, 0x86, 0x99)
LINE = (0x2E, 0x38, 0x46)

TEXTS = {
    "en": {
        "tag": ["A light, native control centre for the", "Monster Tulpar T5 / XMG Neo 15 M21"],
        "pills": ["Live values", "Confirm first", "GPU power to 150 W"],
        "note": "Pictured: the built-in simulator, not a real laptop.",
        "shots": "",
    },
    "tr": {
        "tag": ["Monster Tulpar T5 / XMG Neo 15 M21 için", "hafif, yerel bir denetim merkezi"],
        "pills": ["Canlı değerler", "Önce onay", "150 W'a GPU gücü"],
        "note": "Görülen: yerleşik simülatör, gerçek bir dizüstü değil.",
        "shots": "tr",
    },
}


def spaced(draw, xy, text, fnt, fill, gap):
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=fnt, fill=fill)
        x += draw.textlength(ch, font=fnt) + gap


def card(path, width):
    """A screenshot as a rounded card with a hairline border, scaled to `width`."""
    im = Image.open(path).convert("RGB")
    h = round(im.height * width / im.width)
    im = im.resize((width, h), Image.LANCZOS)
    r = 22
    mask = Image.new("L", (width, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, width - 1, h - 1), radius=r, fill=255)
    out = Image.new("RGBA", (width, h), (0, 0, 0, 0))
    out.paste(im, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle((0, 0, width - 1, h - 1), radius=r, outline=LINE + (255,), width=2)
    return out


def shadow(size, radius=22, blur=34, alpha=190):
    w, h = size
    pad = blur * 3
    s = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(s).rounded_rectangle((pad, pad, pad + w, pad + h), radius=radius, fill=(0, 0, 0, alpha))
    return s.filter(ImageFilter.GaussianBlur(blur)), pad


def banner(lang):
    t = TEXTS[lang]
    shots = os.path.join(IMG, t["shots"]) if t["shots"] else IMG
    img = gradient(W, H, BG_TOP, BG_BOTTOM).convert("RGBA")

    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((760, -140, 1760, 560), fill=ACCENT + (62,))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(120)))

    dots = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dots)
    for y in range(24, H, 44):
        for x in range(24, W, 44):
            dd.ellipse((x - 1, y - 1, x + 1, y + 1), fill=(255, 255, 255, 16))
    img = Image.alpha_composite(img, dots)

    # the two screenshots, the second one in front, both running off the bottom edge
    for name, x, y, w in (("hero-custom.png", 830, 64, 800), ("overview.png", 700, 214, 720)):
        c = card(os.path.join(shots, name), w)
        sh, pad = shadow(c.size)
        img.alpha_composite(sh, (x - pad, y - pad + 18))
        img.alpha_composite(c, (x, y))

    # the left column
    d = ImageDraw.Draw(img)
    badge = gradient(138, 138, BG_TOP, BG_BOTTOM).convert("RGBA")
    m = Image.new("L", (138, 138), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, 137, 137), radius=38, fill=255)
    badge.putalpha(m)
    ImageDraw.Draw(badge).rounded_rectangle((1, 1, 136, 136), radius=37, outline=ACCENT + (110,), width=3)
    badge.alpha_composite(bolt_layer(104), (17, 17))
    img.alpha_composite(badge, (96, 74))

    d = ImageDraw.Draw(img)
    # "Control Center" is the big line and "ADVANCED" the spaced label under it, as in the sidebar of the program; 80 px keeps the
    # name clear of the first screenshot, which starts at x = 700
    d.text((92, 236), "Control Center", font=font("Inter-Bold", 80), fill=TEXT)
    spaced(d, (98, 340), "ADVANCED", font("Inter-SemiBold", 30), BOLT_B, 11)
    y = 404
    for line in t["tag"]:
        d.text((96, y), line, font=font("Inter-Regular", 30), fill=MUTED)
        y += 42
    # The pills are drawn on a transparent layer and composited: ImageDraw on an RGBA picture replaces pixels instead of blending, so a
    # translucent fill drawn straight onto the banner would come out as solid orange under light text.
    x = 96
    f = font("Inter-Medium", 21)
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    placed = []
    for text in t["pills"]:
        w = int(d.textlength(text, font=f)) + 36
        ld.rounded_rectangle((x, 500, x + w, 540), radius=20, outline=ACCENT + (150,), fill=ACCENT + (28,), width=2)
        placed.append((x, text))
        x += w + 12
    img = Image.alpha_composite(img, layer)
    d = ImageDraw.Draw(img)
    for px, text in placed:
        d.text((px + 18, 508), text, font=f, fill=PILL_TEXT)
    d.text((96, 552), t["note"], font=font("Inter-Regular", 15), fill=NOTE)
    return img.convert("RGB")


if __name__ == "__main__":
    for lang, out in (("en", "banner.png"), ("tr", "banner.tr.png")):
        path = os.path.join(IMG, out)
        banner(lang).save(path, optimize=True)
        print("wrote", path)
