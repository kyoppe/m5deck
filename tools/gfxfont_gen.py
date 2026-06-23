#!/usr/bin/env python3
"""TTF -> LovyanGFX(Adafruit GFX互換) フォントヘッダ生成。

無印 駅の時計の数字に近い幾何学サンセリフ(Jost*)から、必要な数字グリフだけを
1bit ビットマップに変換し、lgfx::GFXfont 形式のヘッダを出力する。

ビット詰め仕様(Adafruit fontconvert 互換):
  - 各グリフは width*height ビットを行ごとに MSB ファーストで連続詰め
  - グリフ末尾でバイト境界までゼロパディング(=グリフ毎に ceil(w*h/8) バイト)
"""
import sys
import freetype

FONT_PATH = sys.argv[1] if len(sys.argv) > 1 else "Jost.ttf"
OUT_PATH = sys.argv[2] if len(sys.argv) > 2 else "../include/MujiNum.h"
PIXEL_SIZE = int(sys.argv[3]) if len(sys.argv) > 3 else 46
WEIGHT = int(sys.argv[4]) if len(sys.argv) > 4 else 400
NAME = "MujiNum"

FIRST, LAST = 0x30, 0x39  # '0'..'9'

face = freetype.Face(FONT_PATH)
# 可変フォントの太さ(wght)を指定
try:
    face.set_var_design_coords([WEIGHT])
except Exception as e:
    print(f"# variable axis set skipped: {e}", file=sys.stderr)
face.set_pixel_sizes(0, PIXEL_SIZE)

bitmaps = bytearray()
glyphs = []

for code in range(FIRST, LAST + 1):
    face.load_char(chr(code), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
    g = face.glyph
    bm = g.bitmap
    w, h, pitch, buf = bm.width, bm.rows, bm.pitch, bm.buffer

    offset = len(bitmaps)
    acc = 0
    nbits = 0
    for y in range(h):
        for x in range(w):
            byte = buf[y * pitch + (x >> 3)]
            bit = 1 if (byte & (0x80 >> (x & 7))) else 0
            acc = (acc << 1) | bit
            nbits += 1
            if nbits == 8:
                bitmaps.append(acc)
                acc = 0
                nbits = 0
    if nbits:  # グリフ末尾をバイト境界までパディング
        acc <<= (8 - nbits)
        bitmaps.append(acc)

    xadv = g.advance.x >> 6
    glyphs.append((offset, w, h, xadv, g.bitmap_left, -g.bitmap_top, chr(code)))

y_advance = face.size.height >> 6

lines = []
lines.append("#pragma once")
lines.append("// 自動生成 (tools/gfxfont_gen.py) - 無印 駅の時計用 数字フォント")
lines.append(f"// source: Jost* (SIL OFL), pixel_size={PIXEL_SIZE}, wght={WEIGHT}")
lines.append("#include <M5Unified.h>")
lines.append("")
lines.append(f"static const uint8_t {NAME}Bitmaps[] = {{")
row = []
for i, b in enumerate(bitmaps):
    row.append(f"0x{b:02X},")
    if len(row) == 16:
        lines.append("  " + "".join(row))
        row = []
if row:
    lines.append("  " + "".join(row))
lines.append("};")
lines.append("")
lines.append(f"static const lgfx::GFXglyph {NAME}Glyphs[] = {{")
lines.append("// bitmapOffset, width, height, xAdvance, xOffset, yOffset")
for (off, w, h, xadv, xoff, yoff, ch) in glyphs:
    lines.append(f"  {{ {off:5d}, {w:3d}, {h:3d}, {xadv:3d}, {xoff:4d}, {yoff:4d} }}, // '{ch}'")
lines.append("};")
lines.append("")
lines.append(f"static const lgfx::GFXfont {NAME}(")
lines.append(f"  (uint8_t*){NAME}Bitmaps, (lgfx::GFXglyph*){NAME}Glyphs,")
lines.append(f"  0x{FIRST:02X}, 0x{LAST:02X}, {y_advance});")
lines.append("")

with open(OUT_PATH, "w") as f:
    f.write("\n".join(lines))

print(f"wrote {OUT_PATH}: {len(bitmaps)} bitmap bytes, yAdvance={y_advance}")
for (off, w, h, xadv, xoff, yoff, ch) in glyphs:
    print(f"  '{ch}': {w}x{h} adv={xadv} off=({xoff},{yoff})")
