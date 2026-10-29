#!/usr/bin/env python3
"""打包 assets 分区镜像（与 app/apps/utils/assets/assets_partition.{h,cpp} 配套）。

格式（全部小端、4 字节成员、无对齐填充）:
  [16B 表头: magic 'ASTB' | version=1 | count | reserved]
  [count × 40B 条目: name[32] (0 填充) | offset | size]   offset 相对镜像起始
  [资产数据，起始位置 4 字节对齐]

资产来源:
  home_bg    — 从 app/assets/images/home_bg.c 解析出的 RGB565 位图（单一数据源，
               桌面模拟器继续链接该 .c；真机镜像从这里提取，不维护第二份 .bin）
  font_zh_20 — managed_components/78__xiaozhi-fonts/cbin/font_puhui_common_20_4.bin

用法: python3 tools/pack_assets.py [输出.img]   （默认 build/assets.img）
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # platforms/tab5

MAGIC = 0x42545341  # 'ASTB'
VERSION = 1
NAME_LEN = 32
ENTRY_SIZE = 40


def parse_c_array(c_path, sym):
    """提取 C 源文件里 `sym[] = { 0x..,... };` 的全部字节。"""
    text = open(c_path, encoding="utf-8", errors="replace").read()
    m = re.search(re.escape(sym) + r"\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise RuntimeError(f"symbol {sym} not found in {c_path}")
    data = bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]+", m.group(1)))
    return data


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "assets.img")

    bg = parse_c_array(
        os.path.join(ROOT, "../../app/assets/images/home_bg.c"), "home_bg_map")
    if len(bg) != 1280 * 720 * 2:
        raise RuntimeError(f"home_bg_map: expect {1280*720*2} bytes, got {len(bg)}")

    font20 = open(
        os.path.join(ROOT, "managed_components/78__xiaozhi-fonts/cbin/"
                      "font_puhui_common_20_4.bin"), "rb").read()
    # 24px 中间号: 自产 (gen_cbin_font.py 从 puhei-common.ttf 生成, 存仓库)
    font24 = open(
        os.path.join(ROOT, "../../app/assets/fonts/"
                      "font_puhui_common_24_4.bin"), "rb").read()

    # 注: app_ssh 终端 CJK 用 font_zh_24(cbin), 自产 GB2312 点阵方案已弃
    # (gen_cjk_font.py 留档; 若复活须解决资产间 4 字节对齐)

    pinyin_tbl = open(
        os.path.join(ROOT, "../../app/assets/fonts/pinyin_tbl.bin"), "rb").read()

    assets = [("home_bg", bg), ("font_zh_20", font20), ("font_zh_24", font24),
              ("pinyin_tbl", pinyin_tbl)]

    header_size = 16 + ENTRY_SIZE * len(assets)
    off = (header_size + 3) & ~3  # 数据区 4 字节对齐
    entries, blob = [], bytearray(b"\xff" * off)  # 空洞填 0xff（flash 擦除态）
    for name, data in assets:
        entries.append((name.encode(), off, len(data)))
        blob += data
        off += len(data)

    img = bytearray()
    img += struct.pack("<IIII", MAGIC, VERSION, len(assets), 0)
    for name, eoff, esize in entries:
        nb = name[: NAME_LEN - 1].ljust(NAME_LEN, b"\0")
        img += struct.pack("<32sII", nb, eoff, esize)
    assert len(img) == header_size
    img += blob[header_size:]

    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(img)
    for name, eoff, esize in entries:
        print(f"{name.decode():<12} off=0x{eoff:06x} size={esize} ({esize/1024:.0f} KB)")
    print(f"total image: {len(img)} bytes ({len(img)/1024/1024:.2f} MB) -> {out}")


if __name__ == "__main__":
    main()
