#pragma once
/*
 * cjk_font — 终端 CJK 字形, 复用 xiaozhi 的 font_zh_24 cbin 字体
 * (res 分区既有资产, app_radio 同款读取路径; 经 LVGL fmt_txt glyph
 * API 取 4bpp 位图, 自行混成到 RGB565 canvas)。
 *
 * 不再使用自产 GB2312 点阵(gen_cjk_font.py/cjk_gb2312_24.bin 留仓库
 * 备查): 那条路栽在打包奇数偏移上 uint32 码点表非对齐读。
 *
 * x/y 为 (boxW x boxH) 盒左上角, 字形在盒内居中; 前景对已铺的背景
 * 像素做 4bpp 抗锯齿混色。仅真机(桌面无 res 分区 → 恒无字形)。
 */
#include <stdint.h>

namespace CjkFont {

bool ready();
bool hasGlyph(uint32_t cp);

void drawGlyphRgb565(uint16_t* buf, int bufW, int bufH, uint32_t cp,
                     int x, int y, int boxW, int boxH, uint16_t fg);

}  // namespace CjkFont
