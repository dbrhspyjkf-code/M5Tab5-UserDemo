#!/usr/bin/env python3
"""gen_lottery_icon.py — 双色球工具页 tile 图标转换器 (2026-11-01)。

用户指定图标: icons8-彩票-100.png (透明底 RGBA, 100x100, 彩色票券+红蓝球)。
转 ARGB8888 lv_image_dsc_t C 数组 (uint32 0xAARRGGBB, 与 canvas_draw 同
字节序, app_stocks 走势图 canvas 已实测该格式渲染正确)。保留原色不 recolor
——红/蓝球是彩票语义色。

用法: python3 gen_lottery_icon.py <输入.png>   (输出 lottery_icon.c 于同目录)
"""
import sys
from pathlib import Path

from PIL import Image


def main() -> None:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "icons8-彩票-100.png")
    img = Image.open(src).convert("RGBA")
    w, h = img.size
    px = img.load()

    out = Path(__file__).parent / "lottery_icon.c"
    with out.open("w") as f:
        f.write("/* 由 gen_lottery_icon.py 生成, 勿手改。源: %s (%dx%d) */\n" % (src.name, w, h))
        f.write('#include "lottery_icon.h"\n\n')
        f.write("static const uint32_t lottery_icon_map[] = {\n")
        for y in range(h):
            f.write("    ")
            f.write(",".join("0x%02X%02X%02X%02X" % (px[x, y][3], px[x, y][0], px[x, y][1], px[x, y][2])
                             for x in range(w)))
            f.write(",\n")
        f.write("};\n\n")
        f.write("const lv_image_dsc_t lottery_icon = {\n")
        f.write("  .header.cf = LV_COLOR_FORMAT_ARGB8888,\n")
        f.write("  .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        f.write("  .header.w = %d,\n" % w)
        f.write("  .header.h = %d,\n" % h)
        f.write("  .data_size = %d,\n" % (w * h * 4))
        f.write("  .data = (const uint8_t*)lottery_icon_map,\n")
        f.write("};\n")
    print(f"written {out} ({w}x{h}, {w*h*4} bytes)")


if __name__ == "__main__":
    main()
