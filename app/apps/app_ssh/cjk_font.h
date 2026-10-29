#pragma once
/*
 * cjk_font — GB2312 一级字库 24x24 点阵 (res 分区 cjk_font 资产,
 * gen_cjk_font.py 生成, STHeiti Light 渲染阈值 110)。终端宽字符渲染用:
 * 码点二分查找, 字形等长无偏移表。仅真机(桌面无 res 分区 → 恒无字形)。
 *
 * 二进制格式见 gen_cjk_font.py 头注释 ('CJK1' magic)。
 */
#include <stdint.h>

namespace CjkFont {

// 首次调用时从 res 分区加载(mmap 指针, 零拷贝); 失败恒返回 false
bool ready();
bool hasGlyph(uint32_t cp);

// 画 24x24 字形到 RGB565 缓冲, (x,y) 为字形左上角, 外框留白由调用方管
void drawGlyphRgb565(uint16_t* buf, int bufW, int bufH,
                     uint32_t cp, int x, int y, uint16_t fg);

}  // namespace CjkFont
