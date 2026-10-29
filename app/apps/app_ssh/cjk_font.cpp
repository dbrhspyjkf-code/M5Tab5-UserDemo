/*
 * cjk_font — font_zh_24 cbin 字体的终端位图混成实现。
 *
 * 字形获取: lv_font_get_glyph_dsc(cmap 查找+元数据) 后, 绕开
 * lv_font_get_glyph_bitmap —— LVGL9.2 该 API 语义是"解到调用方
 * draw_buf->data"(传 nullptr 即空指针崩溃, 2026-10-29 实锤
 * Load access fault @ lv_font_get_bitmap_fmt_txt), 改为直读
 * fmt_txt 内部结构: 字体由 gen_cbin_font.py --no-compress 生成
 * (PLAIN 格式, 4bpp 高半字节在前), glyph_bitmap 指向 res 分区
 * mmap, 单字节读取无对齐要求, 零拷贝。
 * 调用方(会话任务渲染)持有 lvglLock, 字体缓存访问在锁内安全。
 */
#include "cjk_font.h"

#ifndef PLATFORM_BUILD_DESKTOP
#include <lvgl.h>
#include <cbin_font.h>
#include <font/lv_font_fmt_txt.h>
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
    if (dsc.is_placeholder || !dsc.gid.index) return;

    const lv_font_t* font = dsc.resolved_font;
    if (!font || !font->dsc) return;
    const auto* fdsc = (const lv_font_fmt_txt_dsc_t*)font->dsc;
    if (fdsc->bitmap_format != LV_FONT_FMT_TXT_PLAIN || fdsc->bpp != 4) return;
    if (!dsc.box_w || !dsc.box_h) return;

    const uint32_t gid = dsc.gid.index;
    const auto& gd = fdsc->glyph_dsc[gid];
    const uint8_t* bm = fdsc->glyph_bitmap + gd.bitmap_index;
    const int bw = gd.box_w, bh = gd.box_h;
    const int stride = (bw + 1) / 2;          // 4bpp, 高半字节在前

    // 字形盒在 (boxW x boxH) 内居中
    const int ox = x + (boxW - bw) / 2;
    const int oy = y + (boxH - bh) / 2;

    for (int gy = 0; gy < bh; ++gy) {
        const int dy = oy + gy;
        if (dy < 0 || dy >= bufH) continue;
        const uint8_t* row = bm + (size_t)gy * stride;
        uint16_t* line = buf + (size_t)dy * bufW;
        for (int gx = 0; gx < bw; ++gx) {
            const uint8_t a = (gx & 1) ? (row[gx >> 1] & 0xF) : (row[gx >> 1] >> 4);
            if (!a) continue;
            const int dx = ox + gx;
            if (dx >= 0 && dx < bufW) blend(line[dx], fg, a);
        }
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
