# SPDX-License-Identifier: GPL-3.0-or-later
"""Растеризует TTF в 1-битные шрифты для прошивки (fonts.h).

Латиница+кириллица берутся из Roboto Condensed, иврит — из Noto Sans Hebrew.
Запуск:  python gen_fonts.py  ->  ../libraries/FridgeCommon/src/fonts.h
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).parent
OUT = HERE.parent / "libraries" / "FridgeCommon" / "src" / "fonts.h"
LATCYR = HERE / "fonts" / "RobotoCondensed.ttf"
HEBREW = HERE / "fonts" / "NotoSansHebrew.ttf"

CHARSET = (
    list(range(0x20, 0x7F))
    + [0x401, 0x451] + list(range(0x410, 0x450))          # кириллица
    + list(range(0x5D0, 0x5EB))                             # иврит
    + [0xAB, 0xBB, 0x2014, 0x2013, 0x2116, 0xB0, 0x2022,   # « » — – № ° •
       0x2026, 0x2018, 0x2019, 0x201C, 0x201D, 0x20AA, 0x20AC]  # … ‘ ’ “ ” ₪ €
)

# имя, пиксельный размер, вес
FONTS = [
    ("F_SMALL", 15, 500),   # шапка
    ("F_UNIT", 13, 500),    # «дней»
    ("F_ITEM", 20, 400),    # названия продуктов
    ("F_HEAD", 22, 700),    # заголовки секций
    ("F_NUM", 26, 700),     # числа в списках
    ("F_BIG", 40, 700),     # число в блоке «сегодня»
]
THRESHOLD = 110  # ниже 128 -> штрихи чуть жирнее, лучше для e-ink


def load(path, size, weight, hebrew=False):
    f = ImageFont.truetype(str(path), size)
    vals = []
    for a in f.get_variation_axes():
        n = a["name"]
        n = n.decode() if isinstance(n, bytes) else n
        if n.lower() == "weight":
            vals.append(weight)
        elif n.lower() == "width":
            vals.append(87.5 if hebrew else 100)
        else:
            vals.append(a["default"])
    f.set_variation_by_axes(vals)
    return f


def glyph(font, ch):
    ascent, _ = font.getmetrics()
    adv = round(font.getlength(ch))
    bbox = font.getbbox(ch, anchor="ls")
    if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
        return adv, 0, 0, 0, 0, b""
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-bbox[0], -bbox[1]), ch, font=font, fill=255, anchor="ls")
    bits, acc, n = bytearray(), 0, 0
    for p in img.getdata():
        acc = (acc << 1) | (1 if p >= THRESHOLD else 0)
        n += 1
        if n == 8:
            bits.append(acc); acc = n = 0
    if n:
        bits.append(acc << (8 - n))
    return adv, w, h, bbox[0], bbox[1], bytes(bits)  # x/y-offset от базовой линии


def main():
    out = [
        "// Сгенерировано tools/gen_fonts.py — не редактировать вручную.",
        "// Glyphs: Roboto Condensed (Apache-2.0, (c) 2011 Google Inc.) and Noto Sans Hebrew",
        "// (OFL-1.1, (c) 2024 The Noto Project Authors). See THIRD_PARTY_NOTICES.md.",
        "#pragma once",
        '#include "efont.h"',
        "",
    ]
    for name, size, weight in FONTS:
        lc = load(LATCYR, size, weight)
        he = load(HEBREW, size, weight, hebrew=True)
        bitmap, glyphs = bytearray(), []
        for cp in sorted(set(CHARSET)):
            f = he if 0x590 <= cp <= 0x5FF else lc
            adv, w, h, xo, yo, bits = glyph(f, chr(cp))
            glyphs.append((cp, len(bitmap), w, h, xo, yo, adv))
            bitmap += bits
        asc, desc = lc.getmetrics()
        out.append(f"static const uint8_t {name}_bits[] PROGMEM = {{")
        for i in range(0, len(bitmap), 24):
            out.append("  " + ",".join(f"0x{b:02X}" for b in bitmap[i:i + 24]) + ",")
        out.append("};")
        out.append(f"static const EGlyph {name}_glyphs[] PROGMEM = {{")
        for g in glyphs:
            out.append("  {0x%04X,%d,%d,%d,%d,%d,%d}," % g)
        out.append("};")
        out.append(f"static const EFont {name} = {{{name}_bits, {name}_glyphs, {len(glyphs)}, {asc}, {desc}}};")
        out.append("")
        print(f"{name}: {size}px w{weight}, {len(glyphs)} glyphs, {len(bitmap)} bytes")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out), encoding="utf-8")
    print("->", OUT)


if __name__ == "__main__":
    main()
