/*
 * cjk_font — font_zh_24 cbin 字体的终端位图混成实现。
 * 字体解析(cbin_font_create)与位图格式(LVGL fmt_txt A4, 高半字节在前)
 * 均为 LVGL 标准管线; 本文件只做 glyph→RGB565 的居中 blit + 混色。
 * 调用方(会话任务渲染)持有 lvglLock, lv_font_* 缓存访问在此锁内安全。
 */
#include "cjk_font.h"

#ifndef PLATFORM_BUILD_DESKTOP
#include <lvgl.h>
#include <cbin_font.h>
#include "../utils/assets/assets_partition.h"

namespace CjkFont {
namespace {

lv_font_t* zh24()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}

// 16 级 alpha 混 fg 到 dst(RGB565), a=15 全 fg
inline void blend(uint16_t& dst, uint16_t fg, uint8_t a)
{
    if (a >= 15) { dst = fg; return; }
    if (!a) return;
    const int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    const int fr = (fg >> 11) & 0x1F, fg6 = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    const int r = (fr * a + dr * (15 - a) + 7) / 15;
    const int g = (fg6 * a + dg * (15 - a) + 7) / 15;
    const int b = (fb * a + db * (15 - a) + 7) / 15;
    dst = (uint16_t)((r << 11) | (g << 5) | b);
}

}  // namespace

bool ready() { return zh24() != nullptr; }

bool hasGlyph(uint32_t cp)
{
    lv_font_t* f = zh24();
    if (!f) return false;
    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(f, &dsc, cp, 0)) return false;
    return !dsc.is_placeholder && dsc.box_w > 0 && dsc.box_h > 0;
}

void drawGlyphRgb565(uint16_t* buf, int bufW, int bufH, uint32_t cp,
                     int x, int y, int boxW, int boxH, uint16_t fg)
{
    lv_font_t* f = zh24();
    if (!f || !buf) return;
    lv_font_glyph_dsc_t dsc;
    if (!lv_font_get_glyph_dsc(f, &dsc, cp, 0)) return;
    if (dsc.is_placeholder || !dsc.box_w || !dsc.box_h) return;
    const uint8_t* bm = (const uint8_t*)lv_font_get_glyph_bitmap(&dsc, nullptr);
    if (!bm) return;

    // 字形盒在 (boxW x boxH) 内居中
    const int ox = x + (boxW - (int)dsc.box_w) / 2;
    const int oy = y + (boxH - (int)dsc.box_h) / 2;

    switch (dsc.format) {
    case LV_FONT_GLYPH_FORMAT_A4: {
        const int stride = ((int)dsc.box_w + 1) / 2;
        for (uint16_t gy = 0; gy < dsc.box_h; ++gy) {
            const int dy = oy + gy;
            if (dy < 0 || dy >= bufH) continue;
            const uint8_t* row = bm + (size_t)gy * stride;
            uint16_t* line = buf + (size_t)dy * bufW;
            for (uint16_t gx = 0; gx < dsc.box_w; ++gx) {
                const uint8_t a = (gx & 1) ? (row[gx >> 1] & 0xF) : (row[gx >> 1] >> 4);
                if (!a) continue;
                const int dx = ox + gx;
                if (dx >= 0 && dx < bufW) blend(line[dx], fg, a);
            }
        }
        break;
    }
    case LV_FONT_GLYPH_FORMAT_A1: {
        const int stride = ((int)dsc.box_w + 7) / 8;
        for (uint16_t gy = 0; gy < dsc.box_h; ++gy) {
            const int dy = oy + gy;
            if (dy < 0 || dy >= bufH) continue;
            const uint8_t* row = bm + (size_t)gy * stride;
            uint16_t* line = buf + (size_t)dy * bufW;
            for (uint16_t gx = 0; gx < dsc.box_w; ++gx) {
                if (!((row[gx >> 3] >> (7 - (gx & 7))) & 1)) continue;
                const int dx = ox + gx;
                if (dx >= 0 && dx < bufW) line[dx] = fg;
            }
        }
        break;
    }
    default:   // A2/其他: 本字体恒为 A4; 未知格式按实心块兜底避免丢字
        for (uint16_t gy = 0; gy < dsc.box_h; ++gy) {
            for (uint16_t gx = 0; gx < dsc.box_w; ++gx) {
                const int dx = ox + gx, dy = oy + gy;
                if (dx >= 0 && dx < bufW && dy >= 0 && dy < bufH)
                    buf[(size_t)dy * bufW + dx] = fg;
            }
        }
        break;
    }
}

}  // namespace CjkFont

#else  // 桌面: 无 res 分区

namespace CjkFont {
bool ready() { return false; }
bool hasGlyph(uint32_t) { return false; }
void drawGlyphRgb565(uint16_t*, int, int, uint32_t, int, int, int, int, uint16_t) {}
}  // namespace CjkFont

#endif
