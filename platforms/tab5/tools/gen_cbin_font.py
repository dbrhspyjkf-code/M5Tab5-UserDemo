#!/usr/bin/env python3
"""lv_font_conv C 数组 → xiaozhi cbin 平铺字体 .bin 生成器。

配套解析器: managed_components/78__xiaozhi-fonts/src/cbin_font.c
布局 (riscv32 / little-endian / LV_FONT_FMT_TXT_LARGE=1, LVGL 9.2):
  [0]    lv_font_t (36B): 指针清零; line_height@12, base_line@16, dsc@24=36
  [36]   lv_font_fmt_txt_dsc_t (20B): glyph_bitmap/glyph_dsc/cmaps =
         相对 dsc 起始的偏移; kern_dsc=0 (--no-kern); 位域 u16 =
         cmap_num:9 | bpp:4<<9 | kern_classes:1<<13 | bitmap_format:2<<14
  [56..] glyph_bitmap → glyph_dsc 数组 (LARGE 16B/项) → cmap 紧凑数组
         (20B/项) → unicode_list(u16)/glyph_id_ofs(u8) 数组
         (cmap 内 u32 字段 = 相对 cmap 数组起始的偏移, 0 = NULL)

用法:
  npx lv_font_conv --font xxx.ttf --size 24 --bpp 4 --format lvgl \
      --no-compress --no-kern -r 0x20-0xFFFF -o /tmp/f.c
  python3 tools/gen_cbin_font.py /tmp/f.c out.bin [--check 剩,元,G,9]
"""
import re
import struct
import sys

LV_FONT_T_SIZE = 36
DSC_SIZE = 20
GLYPH_DSC_SIZE = 16   # LARGE: u32,u32,u16,u16,i16,i16
CMAP_PACKED_SIZE = 20
CMAP_TYPES = ["FORMAT0_FULL", "SPARSE_FULL", "FORMAT0_TINY", "SPARSE_TINY"]


def _array(text, decl):
    m = re.search(decl + r"\s*\[\s*\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise RuntimeError("array not found: " + decl)
    return m.group(1)


def parse_c(src):
    text = open(src, encoding="utf-8", errors="replace").read()

    bitmap = bytes(int(t, 16) for t in
                   re.findall(r"0x[0-9a-fA-F]{1,2}\b", _array(text, r"glyph_bitmap")))

    glyph_dsc = []
    for ent in re.findall(r"\{(.*?)\}", _array(text, r"static const lv_font_fmt_txt_glyph_dsc_t\s+glyph_dsc"), re.S):
        f = dict(re.findall(r"\.(\w+)\s*=\s*(-?\d+)", ent))
        glyph_dsc.append((int(f["bitmap_index"]), int(f["adv_w"]), int(f["box_w"]),
                          int(f["box_h"]), int(f["ofs_x"]), int(f["ofs_y"])))

    arrays = {}   # name -> (width_bits, [values])  值可能十进制或 0x 十六进制
    for m in re.finditer(r"static const uint(8|16)_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\};", text, re.S):
        arrays[m.group(2)] = (int(m.group(1)),
                              [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", m.group(3))])

    cmaps = []
    for ent in re.findall(r"\{(.*?)\}", _array(text, r"static const lv_font_fmt_txt_cmap_t\s+cmaps"), re.S):
        f = dict(re.findall(r"\.(\w+)\s*=\s*([-\w]+)", ent))
        tname = f["type"].replace("LV_FONT_FMT_TXT_CMAP_", "")
        cmaps.append({
            "range_start": int(f["range_start"]),
            "range_length": int(f["range_length"]),
            "glyph_id_start": int(f["glyph_id_start"]),
            "unicode_list": None if f["unicode_list"] == "NULL" else f["unicode_list"],
            "glyph_id_ofs_list": None if f["glyph_id_ofs_list"] == "NULL" else f["glyph_id_ofs_list"],
            "list_length": int(f["list_length"]),
            "type": CMAP_TYPES.index(tname),
        })

    fnt = re.search(r"\.line_height\s*=\s*(\d+).*?\.base_line\s*=\s*(\d+)", text, re.S)
    bpp = int(re.search(r"\.bpp\s*=\s*(\d+)", text).group(1))
    return {"bitmap": bitmap, "glyph_dsc": glyph_dsc, "cmaps": cmaps, "arrays": arrays,
            "line_height": int(fnt.group(1)), "base_line": int(fnt.group(2)), "bpp": bpp}


def build_bin(f):
    off_bitmap = DSC_SIZE
    off_glyph_dsc = off_bitmap + len(f["bitmap"])
    off_cmaps = off_glyph_dsc + len(f["glyph_dsc"]) * GLYPH_DSC_SIZE
    cmaps_addr = LV_FONT_T_SIZE + off_cmaps

    # extra 数组紧随 cmap 数组 (偏移相对 cmaps_addr)
    extras = {}   # name -> rel_off
    cur = len(f["cmaps"]) * CMAP_PACKED_SIZE
    for cm in f["cmaps"]:
        for key in ("unicode_list", "glyph_id_ofs_list"):
            name = cm[key]
            if name and name not in extras:
                w, vals = f["arrays"][name]
                extras[name] = cur
                cur += len(vals) * (w // 8)
    total = cmaps_addr + cur

    out = bytearray(total)
    struct.pack_into("<ii4xI", out, 12, f["line_height"], f["base_line"], LV_FONT_T_SIZE)
    struct.pack_into("<IIIIHH", out, LV_FONT_T_SIZE,
                     off_bitmap, off_glyph_dsc, off_cmaps, 0, 0,
                     len(f["cmaps"]) | (f["bpp"] << 9))

    out[LV_FONT_T_SIZE + off_bitmap: LV_FONT_T_SIZE + off_bitmap + len(f["bitmap"])] = f["bitmap"]
    p = LV_FONT_T_SIZE + off_glyph_dsc
    for g in f["glyph_dsc"]:
        struct.pack_into("<IIHHhh", out, p, *g)
        p += GLYPH_DSC_SIZE
    p = cmaps_addr
    for cm in f["cmaps"]:
        struct.pack_into("<IHHIIHBx", out, p,
                         cm["range_start"], cm["range_length"], cm["glyph_id_start"],
                         extras.get(cm["unicode_list"], 0), extras.get(cm["glyph_id_ofs_list"], 0),
                         cm["list_length"], cm["type"])
        p += CMAP_PACKED_SIZE
    for name, rel in extras.items():
        w, vals = f["arrays"][name]
        for i, v in enumerate(vals):
            if w == 8:
                struct.pack_into("<B", out, cmaps_addr + rel + i, v)
            else:
                struct.pack_into("<H", out, cmaps_addr + rel + i * 2, v)
    return bytes(out)


# ── 自检: 镜像 cbin_font.c + LVGL lv_font_get_glyph_dsc_fmt_txt 的查找 ──────
def decode_and_render(blob, chars):
    lh, bl = struct.unpack_from("<ii", blob, 12); dsc_off = struct.unpack_from("<I", blob, 24)[0]
    gb_off, gd_off, cm_off, _kern, _ks, bits = struct.unpack_from("<IIIIHH", blob, dsc_off)
    cmap_num, bpp = bits & 0x1FF, (bits >> 9) & 0xF
    cmaps_addr = dsc_off + cm_off
    print(f"decode: line_height={lh} base_line={bl} cmaps={cmap_num} bpp={bpp} "
          f"glyphs={(cm_off - gd_off)//GLYPH_DSC_SIZE}")

    def u16(a):
        return struct.unpack_from("<H", blob, a)[0]

    def glyph_id(letter):
        for i in range(cmap_num):
            e = cmaps_addr + i * CMAP_PACKED_SIZE
            rs, rl, gs, ul, go, ll, ty = struct.unpack_from("<IHHIIHB", blob, e)
            rcp = letter - rs
            if rcp < 0 or rcp > rl - 1:
                continue
            if ty in (2, 0):     # FORMAT0: unicode_list == NULL
                if go == 0:
                    return gs + rcp
                # FORMAT0_FULL: ofs 为 u8 数组
                return gs + blob[cmaps_addr + go + rcp]
            # SPARSE: unicode_list 二分 (与 LVGL 相同)
            lo, hi = 0, ll - 1
            while lo <= hi:
                mid = (lo + hi) // 2
                v = u16(cmaps_addr + ul + mid * 2)
                if v == rcp:
                    if ty == 3:   # SPARSE_TINY
                        return gs + mid
                    return gs + u16(cmaps_addr + go + mid * 2)  # SPARSE_FULL: u16
                if v < rcp:
                    lo = mid + 1
                else:
                    hi = mid - 1
        return None

    for ch in chars:
        gid = glyph_id(ord(ch))
        if gid is None:
            print(f"  '{ch}' U+{ord(ch):04X}: NOT FOUND")
            continue
        bi, adv, bw, bh, ox, oy = struct.unpack_from("<IIHHhh", blob, dsc_off + gd_off + gid * GLYPH_DSC_SIZE)
        print(f"  '{ch}' gid={gid} box={bw}x{bh} adv={adv/16:.1f}")
        stride = (bw * bpp + 7) // 8
        for y in range(bh):
            row = ""
            for x in range(bw):
                b = blob[dsc_off + gb_off + bi + y * stride + x // 2]
                nib = (b >> 4) if x % 2 == 0 else b & 0xF
                row += " .:*#"[min(4, (nib * 4 + 14) // 15)]
            print("    " + row)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    check = [c for a in sys.argv[3:] for c in a.split(",") if c] if len(sys.argv) > 3 else []
    f = parse_c(src)
    print(f"parsed: {len(f['bitmap'])}B bitmap, {len(f['glyph_dsc'])} glyphs, "
          f"{len(f['cmaps'])} cmaps, bpp={f['bpp']}, arrays={len(f['arrays'])}")
    blob = build_bin(f)
    open(dst, "wb").write(blob)
    print(f"written {len(blob)} bytes ({len(blob)/1024:.0f} KB) -> {dst}")
    decode_and_render(blob, check or "元G9")


if __name__ == "__main__":
    main()
