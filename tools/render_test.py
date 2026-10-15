#!/usr/bin/env python3
# render_test.py - preview kaomoji rendering with per-character font fallback
import os
from PIL import Image, ImageDraw, ImageFont

FONTS = [
    r"C:\Windows\Fonts\msgothic.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\seguisym.ttf",
]
SIZE = 30
_cache = {}

def get_font(path):
    if path not in _cache:
        _cache[path] = ImageFont.truetype(path, SIZE)
    return _cache[path]

def has_glyph(font, ch):
    """True if the font actually contains ch (compares against a private-use
    char that is essentially guaranteed missing -> .notdef render)."""
    a = bytes(font.getmask(ch))
    b = bytes(font.getmask("\uE123"))
    if not a and not b:
        return False
    return a != b

def pick_font(ch):
    for p in FONTS:
        f = get_font(p)
        if has_glyph(f, ch):
            return f
    return None

def text_width(s):
    w = 0
    for ch in s:
        f = pick_font(ch)
        if f is None:
            w += SIZE // 2
            continue
        w += f.getlength(ch)
    return w

def draw_text(draw, xy, s, fill):
    x, y = xy
    for ch in s:
        f = pick_font(ch)
        if f is None:
            x += SIZE // 2
            continue
        draw.text((x, y), ch, font=f, fill=fill)
        x += f.getlength(ch)

KAOMOJI = [
    "(=^･ω･^=)", "(｡• ᵕ •｡)", "(－_－)", "(>_<)", "(´･ω･`)",
    "(¬‿¬)", "(・_・?)", "(๑¯ω¯๑)", "(￣^￣)", "(x_x)",
    "(♥ω♥*)", "(＾▽＾)", "(－‸ლ)", "(๑•̀ㅂ•́)ง", "(╹◡╹)♥",
    "ᶘ ᵒᴥᵒᶅ", "(oT-T)尸", "(・。・;)", "(￣ω￣;)", "(¬､¬)",
    "(๑>ᴗ<๑)", "(＃￣0￣)", "(ﾉ◕ヮ◕)ﾉ", "(⊙_⊙)", "(￣ー￣)",
    "(＾□＾;)", "(屮゜Д゜)屮", "(╬☉д⊙)", "(ﾟﾛﾟ)", "(＃＞＜)",
]

W, ROW = 620, 46
img = Image.new("RGBA", (W, ROW * (len(KAOMOJI) + 1)), (24, 26, 34, 255))
d = ImageDraw.Draw(img)
label_font = ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf", 16)
d.text((10, 8), "kaomoji render preview (dark bg, white text):", font=label_font, fill=(160, 160, 170))
missing_chars = set()
for i, k in enumerate(KAOMOJI):
    y = ROW * (i + 1)
    d.text((10, y + 10), f"{i:2d}", font=label_font, fill=(120, 120, 130))
    for ch in k:
        if pick_font(ch) is None:
            missing_chars.add(ch)
    draw_text(d, (46, y + 8), k, (255, 255, 255, 255))

if missing_chars:
    print("MISSING GLYPHS:", " ".join(f"U+{ord(c):04X}({c})" for c in sorted(missing_chars)))
else:
    print("all glyphs covered")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kaomoji_preview.png")
img.save(out)
print("saved:", out, f"({W}x{img.height})")
