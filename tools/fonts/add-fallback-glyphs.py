#!/usr/bin/env python3
"""Adds the geometric "missing glyph" boxes titles expect in the system font
to Voland's shared font (Noto Sans, SIL OFL 1.1 - modification allowed).

Nintendo's system font has U+25A1 WHITE SQUARE; Super Smash Bros. Ultimate's
text engine asks for it as its fallback glyph and dereferences the result,
so a font without it crashes the title. Noto Sans (Latin) has no geometric
shapes, so this draws them: U+25A1 (outline square) and U+25A0 (filled).

    tools/fonts/add-fallback-glyphs.py platform/web/public/fonts/NotoSans-Regular.ttf

Idempotent: glyphs already present are left alone. Needs fontTools.
"""
import sys

from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

ADVANCE = 800
LEFT, RIGHT, BOTTOM, TOP = 90, 710, -40, 580
STROKE = 64


def square(pen, left, bottom, right, top, clockwise):
    points = [(left, bottom), (left, top), (right, top), (right, bottom)]
    if not clockwise:
        points.reverse()
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def glyph(filled, glyf):
    pen = TTGlyphPen(glyf)
    square(pen, LEFT, BOTTOM, RIGHT, TOP, clockwise=True)  # TrueType outer contours run clockwise
    if not filled:
        square(pen, LEFT + STROKE, BOTTOM + STROKE, RIGHT - STROKE, TOP - STROKE, clockwise=False)
    return pen.glyph()


def main(path):
    font = TTFont(path)
    glyf, hmtx = font["glyf"], font["hmtx"]
    cmap = font.getBestCmap()
    wanted = [(c, n, f) for c, n, f in ((0x25A1, "uni25A1", False), (0x25A0, "uni25A0", True)) if c not in cmap]
    if wanted:
        order = list(font.getGlyphOrder()) + [name for _, name, _ in wanted]
        font.setGlyphOrder(order)
        glyf.setGlyphOrder(order)
    added = []
    for codepoint, name, filled in wanted:
        g = glyph(filled, glyf)
        glyf[name] = g
        g.recalcBounds(glyf)
        hmtx[name] = (ADVANCE, g.xMin)
        for table in font["cmap"].tables:
            if table.isUnicode():
                table.cmap[codepoint] = name
        added.append(name)
    if added:
        font.save(path)
    print(f"{path}: added {', '.join(added) if added else 'nothing (already present)'}")


if __name__ == "__main__":
    main(sys.argv[1])
