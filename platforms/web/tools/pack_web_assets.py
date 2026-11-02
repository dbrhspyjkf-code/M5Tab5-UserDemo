#!/usr/bin/env python3
"""打包 web 版资产镜像 (res 分区同格式, assets_partition.cpp 的 PLATFORM_BUILD_WEB
分支加载)。

与 pack_assets.py 的区别: 不含 home_bg (web 用不上桌面壁纸 C 数组路径),
但把 30px cbin 也打进来 —— 真机 30px 是固件 EMBED_FILES 单独嵌入的,
web 无固件分区, 三档字体统一走本镜像:
  font_zh_20 / font_zh_24 / font_zh_30 / pinyin_tbl

用法: python3 platforms/web/tools/pack_web_assets.py [输出.img]
默认输出 build/web/bin/assets.img (build.sh 会 --preload-file 进 MEMFS)
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))  # 仓库根
TAB5 = os.path.join(ROOT, "platforms", "tab5")

MAGIC = 0x42545341  # 'ASTB'
VERSION = 1
NAME_LEN = 32
ENTRY_SIZE = 40

ASSETS = [
    ("font_zh_20", os.path.join(TAB5, "managed_components/78__xiaozhi-fonts/cbin/"
                                "font_puhui_common_20_4.bin")),
    ("font_zh_24", os.path.join(ROOT, "app/assets/fonts/font_puhui_common_24_4.bin")),
    ("font_zh_30", os.path.join(TAB5, "managed_components/78__xiaozhi-fonts/cbin/"
                                 "font_puhui_common_30_4.bin")),
    ("pinyin_tbl", os.path.join(ROOT, "app/assets/fonts/pinyin_tbl.bin")),
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "build", "web", "bin", "assets.img")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    blobs = []
    for name, path in ASSETS:
        with open(path, "rb") as f:
            blobs.append((name, f.read()))

    header_size = 16 + len(blobs) * ENTRY_SIZE
    entries = b""
    offset = header_size
    body = b""
    for name, data in blobs:
        assert len(data) > 0, name
        entries += name.encode().ljust(NAME_LEN, b"\0") + struct.pack("<II", offset, len(data))
        pad = (-len(body)) % 4
        body += b"\0" * pad
        offset += len(data)
        body += data

    img = struct.pack("<IIII", MAGIC, VERSION, len(blobs), 0) + entries + body
    with open(out, "wb") as f:
        f.write(img)
    print(f"packed {len(blobs)} assets -> {out} ({len(img)} bytes)")
    for name, data in blobs:
        print(f"  {name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
