#!/usr/bin/env python3
"""生成拼音输入法映射表 (app_ssh IME, res 分区 pinyin_tbl 资产)。

数据源:
  字频  — techkang/chinese_character_frequence zi.csv (维基语料, 频率降序)
  读音  — pypinyin (heteronym 全部读音; 多音字归多个音节)
  覆盖  — 只保留 font_zh_24 cbin 字体里有的字(候选条不出现豆腐)

格式 (小端):
  [u32 magic 'PYN1'][u16 nsyll][u16 reserved]
  [nsyll × 12B: pinyin[8] (a-z/v, NUL 填充, 升序) | u16 blob_off | u16 count]
  [UTF-8 汉字 blob, 每音节内按字频降序]

用法: python3 gen_pinyin_tbl.py [zi.csv] [out.bin]
"""
import re
import struct
import sys

from pypinyin import pinyin

MAGIC = 0x314E5950  # 'PYN1'
MAX_PER_SYLLABLE = 60


def font_cmap_chars(path):
    """解析 cbin 字体的全部码点 (LARGE=1 布局, 26 张 cmap)。"""
    d = open(path, 'rb').read()
    dsc_off = struct.unpack_from('<I', d, 24)[0]
    cm_off = struct.unpack_from('<I', d, dsc_off + 8)[0]
    cmap_num = struct.unpack_from('<H', d, dsc_off + 18)[0] & 0x1FF
    base = dsc_off + cm_off
    cps = set()
    for i in range(cmap_num):
        e = base + i * 20
        rs, rl, gs = struct.unpack_from('<IHH', d, e)
        ul, go, ll = struct.unpack_from('<IIH', d, e + 8)
        ty = d[e + 18]
        if ty in (0, 1):                      # FORMAT0_FULL/TINY
            for k in range(rl):
                cps.add(rs + k)
        elif ty in (2, 3):                    # SPARSE_FULL/TINY: u16 delta
            for k in range(ll):
                cps.add(rs + struct.unpack_from('<H', d, base + ul + k * 2)[0])
    return cps


def main():
    freq_csv = sys.argv[1] if len(sys.argv) > 1 else '/tmp/zi.csv'
    root = __file__.rsplit('/', 2)[0]
    font_bin = root + '/../../app/assets/fonts/font_puhui_common_24_4.bin'
    out = sys.argv[2] if len(sys.argv) > 2 else root + '/../../app/assets/fonts/pinyin_tbl.bin'

    cps = font_cmap_chars(font_bin)
    print(f"font coverage: {len(cps)} codepoints")

    # 聊天场景人工加权: 语料频率会让"尼"排在"你"前, 聊天里显然该你优先
    BOOST = {'你': 3e6, '好': 6e6, '吗': 8e5, '嗯': 5e5, '谢': 3e5,
             '喂': 3e5, '哎': 2e5, '吧': 5e5, '呢': 5e5, '哈': 3e5}

    syl = {}   # syllable -> {char: freq}
    n_in = 0
    for line in open(freq_csv, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('zi,'):
            continue
        parts = line.split(',')
        if len(parts) < 2:
            continue
        ch, freq = parts[0], float(parts[1])
        if len(ch) != 1 or ord(ch) not in cps:
            continue
        n_in += 1
        for reading in pinyin(ch, style=pypinyin.Style.NORMAL, heteronym=True):
            s = reading[0].replace('ü', 'v')
            if not re.fullmatch(r'[a-z]+', s):
                continue
            syl.setdefault(s, {})
            # 首次遇见的频率即排名(字典序稳定); 聊天加权字提频
            syl[s].setdefault(ch, freq + BOOST.get(ch, 0))

    keys = sorted(syl)
    entries, blob, off = [], bytearray(), 0
    for s in keys:
        chars = sorted(syl[s], key=lambda c: -syl[s][c])[:MAX_PER_SYLLABLE]
        utf8 = ''.join(chars).encode()
        entries.append((s, off, len(chars)))
        blob += utf8
        off += len(utf8)

    out_b = bytearray(struct.pack('<IHH', MAGIC, len(entries), 0))
    for s, o, n in entries:
        out_b += struct.pack('<8sHH', s.encode(), o, n)
    out_b += blob
    open(out, 'wb').write(out_b)
    print(f"{len(entries)} syllables, {sum(n for _, _, n in entries)} chars, "
          f"{len(out_b)} bytes -> {out}")

    # 自检: 打几个高频音节的首候选
    bys = {s: ''.join(chr(b) for b in blob[o:o + n * 3][:9])
           for s, o, n in [(s2, o2, n2) for s2, o2, n2 in entries]}
    for probe in ('ni', 'hao', 'zhong', 'de', 'shi'):
        print(f"  {probe}: {bys.get(probe, 'N/A')}")


if __name__ == '__main__':
    import pypinyin
    main()
