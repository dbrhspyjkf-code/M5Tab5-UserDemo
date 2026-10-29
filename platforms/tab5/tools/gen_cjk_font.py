#!/usr/bin/env python3
"""生成 GB2312 一级字库点阵 (app_ssh 终端 CJK 字形, cjk_font.cpp 消费)。

格式 (小端, 无对齐填充):
  [12B 表头: magic 'CJK1' | u16 w | u16 h | u16 rowBytes | u16 count]
  [count × u32 码点 (Unicode 升序, 二分查找)]
  [count × (h × rowBytes) 字节位图]   (1bpp, MSB first, 每字形等长无偏移表)

字集: GB2312 一级字库 (区 16-55, 3755 字)。24x24 经典终端汉字规格,
  28px 终端行高内居中。生成 285KB, 经 pack_assets.py 进 res 分区。

用法: python3 gen_cjk_font.py [font_path] [out.bin]
  font_path 默认遍历 macOS 常见 CJK 字体 (ttc 用 index=0)。
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

W = H = 24
ROW_BYTES = (W + 7) // 8  # 3

CANDIDATES = [
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
]


def load_font(path=None):
    paths = [path] if path else CANDIDATES
    for p in paths:
        for idx in (0, 1, 2):
            try:
                return ImageFont.truetype(p, H - 4, index=idx), f"{p}#{idx}"
            except Exception:
                continue
    raise RuntimeError("no usable CJK font found")


def gb2312_level1():
    chars = []
    for qu in range(16, 56):        # 一级: 16-55 区
        for wei in range(1, 95):
            try:
                ch = bytes([0xA0 + qu, 0xA0 + wei]).decode("gb2312")
                chars.append(ch)
            except UnicodeDecodeError:
                pass
    return chars


def render_glyph(font, ch):
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text((W // 2, H // 2), ch, font=font, fill=255, anchor="mm")
    px = img.load()
    rows = bytearray(H * ROW_BYTES)
    for y in range(H):
        for x in range(W):
            if px[x, y] > 110:      # 阈值取 110: 细笔画字体不至于太瘦
                rows[y * ROW_BYTES + x // 8] |= 0x80 >> (x & 7)
    return bytes(rows)


def main():
    root = __file__.rsplit("/", 2)[0]   # platforms/tab5
    out = sys.argv[2] if len(sys.argv) > 2 else root + "/../../app/assets/fonts/cjk_gb2312_24.bin"
    font, name = load_font(sys.argv[1] if len(sys.argv) > 1 else None)
    chars = sorted(set(gb2312_level1()))
    header = struct.pack("<IHHHH", 0x314B4A43, W, H, ROW_BYTES, len(chars))
    blob = bytearray(header)
    blob += struct.pack("<%dI" % len(chars), *(ord(c) for c in chars))
    empty = bytes(H * ROW_BYTES)
    for i, c in enumerate(chars):
        blob += render_glyph(font, c) if ord(c) >= 0x4E00 else empty
        if i % 500 == 0:
            print(f"{i}/{len(chars)}", file=sys.stderr)
    open(out, "wb").write(blob)
    print(f"{name}: {len(chars)} glyphs, {len(blob)} bytes -> {out}")


if __name__ == "__main__":
    main()
