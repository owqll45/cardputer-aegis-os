# -*- coding: utf-8 -*-
"""
gen_font.py — 从 SimSun 的 12px 内嵌点阵直接导出：
  1) GB2312 一级汉字 3755 字，12x12，每字 24 字节（12 行，每行 2 字节，高位在左）
  2) ASCII 0x20..0x7E，每字 12 行 x 1 字节（6px 宽）
输出：main/cn12.bin（汉字点阵）、main/ascii12.h（ASCII 表）
索引公式（与 C 侧一致）：idx = (gb_hi - 0xB0) * 94 + (gb_lo - 0xA1)
"""
import os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = r"C:\Windows\Fonts\simsun.ttc"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main")
os.makedirs(OUT_DIR, exist_ok=True)

font = ImageFont.truetype(FONT_PATH, 12)


def render(ch, cw, ch_h=12, thr=128):
    """渲染到 cw x ch_h 单元格，返回二维 0/1"""
    img = Image.new("L", (cw, ch_h), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
    px = img.load()
    return [[1 if px[x, y] >= thr else 0 for x in range(cw)] for y in range(ch_h)]


# ---------------- 汉字 ----------------
CN_COUNT = 3755
cn = bytearray()
missing = []
for idx in range(CN_COUNT):
    hi = 0xB0 + idx // 94
    lo = 0xA1 + idx % 94
    try:
        ch = bytes([hi, lo]).decode("gb2312")
    except UnicodeDecodeError:
        ch = None
    if not ch:
        cn += bytes(24)
        missing.append((hi, lo))
        continue
    m = render(ch, 12)
    # 若整格为空（缺字形），记录下来
    if not any(any(r) for r in m):
        missing.append((hi, lo, ch))
    for row in m:
        v = 0
        for x in range(12):
            v = (v << 1) | row[x]
        cn += bytes([(v >> 8) & 0xFF, v & 0xFF])

with open(os.path.join(OUT_DIR, "cn12.bin"), "wb") as f:
    f.write(cn)
print("cn12.bin bytes =", len(cn), "expect", CN_COUNT * 24, "missing:", len(missing))
if missing:
    print("missing sample:", missing[:10])

# ---------------- 常用中文标点（GB2312 一级汉字区不含，追加在 3755 之后） ----------------
PUNCT = "，。、：；！？（）《》“”‘’…—·【】「」～"
extra = []          # (codepoint, glyph_index)
for ch in PUNCT:
    m = render(ch, 12)
    if not any(any(r) for r in m):
        print("punct empty, skipped:", ch)
        continue
    for row in m:
        v = 0
        for x in range(12):
            v = (v << 1) | row[x]
        cn += bytes([(v >> 8) & 0xFF, v & 0xFF])
    extra.append((ord(ch), CN_COUNT + len(extra)))

with open(os.path.join(OUT_DIR, "cn12.bin"), "wb") as f:
    f.write(cn)
print("cn12.bin bytes =", len(cn), "hanzi", CN_COUNT, "punct", len(extra))

# ---------------- ASCII ----------------
# 汉字是 12px 点阵，ASCII 用 Consolas 12px（等宽 7px）二值化，基线/行高与汉字对齐
afont = ImageFont.truetype(r"C:\Windows\Fonts\consola.ttf", 12)
AW, ATHR = 7, 100


def render_ascii(ch):
    img = Image.new("L", (AW, 12), 0)
    ImageDraw.Draw(img).text((0, 0), ch, font=afont, fill=255)
    px = img.load()
    return [[1 if px[x, y] >= ATHR else 0 for x in range(AW)] for y in range(12)]


ascii_rows = []
for code in range(0x20, 0x7F):
    ch = chr(code)
    m = render_ascii(ch)
    for row in m:
        v = 0
        for x in range(AW):
            v = (v << 1) | row[x]
        ascii_rows.append(v << 1)   # 高 7 位有效

with open(os.path.join(OUT_DIR, "ascii12.h"), "w", encoding="utf-8") as f:
    f.write("// 自动生成：Consolas 12px 二值化，ASCII 0x20..0x7E，12 行 x 7px，每行 1 字节(高 7 位)\n")
    f.write("#pragma once\n#include <stdint.h>\n")
    f.write("#define ASCII12_W 7\n#define ASCII12_H 12\n")
    f.write("static const uint8_t ascii12[95 * 12] = {\n")
    for i in range(0, len(ascii_rows), 12):
        chunk = ascii_rows[i:i + 12]
        f.write("    " + ", ".join("0x%02X" % v for v in chunk) + ",  // " + repr(chr(0x20 + i // 12)) + "\n")
    f.write("};\n")
print("ascii12.h rows =", len(ascii_rows))

# ---------------- Unicode -> 汉字字库索引（按 codepoint 排序，供二分查找） ----------------
pairs = []
for idx in range(CN_COUNT):
    hi = 0xB0 + idx // 94
    lo = 0xA1 + idx % 94
    try:
        ch = bytes([hi, lo]).decode("gb2312")
    except UnicodeDecodeError:
        continue
    pairs.append((ord(ch), idx))
pairs += extra
pairs.sort()
with open(os.path.join(OUT_DIR, "cn_index.h"), "w", encoding="utf-8") as f:
    f.write("// 自动生成：GB2312 一级字库 Unicode->索引（按 codepoint 升序）\n")
    f.write("#pragma once\n#include <stdint.h>\n")
    f.write("#define CN_COUNT %d\n" % CN_COUNT)
    f.write("static const uint16_t cn_cp[%d] = {\n" % len(pairs))
    for i in range(0, len(pairs), 10):
        f.write("    " + ", ".join("0x%04X" % c for c, _ in pairs[i:i + 10]) + ",\n")
    f.write("};\n")
    f.write("static const uint16_t cn_idx[%d] = {\n" % len(pairs))
    for i in range(0, len(pairs), 10):
        f.write("    " + ", ".join("%d" % x for _, x in pairs[i:i + 10]) + ",\n")
    f.write("};\n")
print("cn_index.h pairs =", len(pairs))

# ---------------- 自检预览 ----------------
def preview(ch, cw=12):
    m = render(ch, cw)
    print("--", ch)
    for row in m:
        print("".join("#" if v else "." for v in row) + "|")

for ch in "收到你好中":
    preview(ch)
for ch in "，。、：！？":
    preview(ch)
for ch in "A9g":
    preview(ch, 6)
