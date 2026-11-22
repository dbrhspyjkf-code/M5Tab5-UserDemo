#!/usr/bin/env python3
"""生成 GBK→Unicode 映射表 (u16 LE, 邮件直连 GBK/GB2312 正文与 RFC2047 头解码用)。

布局: 126 首字节 (0x81..0xFE) × 190 尾字节 (0x40..0xFE 跳 0x7F) = 23940 项,
每项 2 字节小端 UTF-16 码元, 0=无效码位 (运行时替换 U+FFFD)。
索引公式与固件 mail_mime.cpp gbk_index() 严格一致:
    idx = (lead-0x81)*190 + (trail-0x40 if trail<0x7F else trail-0x41)

输出: app/assets/fonts/gbk_tbl.bin (入仓与 pinyin_tbl.bin 同例, 由
pack_assets.py 打进 res 分区资产 "gbk_tbl", ~47KB)。
用法: python3 tools/gen_gbk_tbl.py
"""
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # platforms/tab5
OUT = os.path.join(ROOT, "../../app/assets/fonts/gbk_tbl.bin")

LEADS = range(0x81, 0xFF)                    # 0x81..0xFE
TRAILS = [t for t in range(0x40, 0xFF) if t != 0x7F]   # 0x40..0xFE 跳 0x7F


def main():
    entries = []
    invalid = 0
    for lead in LEADS:
        for trail in TRAILS:
            b = bytes([lead, trail])
            try:
                ch = b.decode("gbk")
                entries.append(ord(ch))
            except (UnicodeDecodeError, ValueError):
                entries.append(0)
                invalid += 1
    assert len(entries) == 126 * 190 == 23940, len(entries)
    data = struct.pack("<%dH" % len(entries), *entries)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"gbk_tbl.bin: {len(entries)} entries, {len(data)} bytes "
          f"({len(data)/1024:.1f} KB), {invalid} invalid -> {OUT}")


if __name__ == "__main__":
    main()
