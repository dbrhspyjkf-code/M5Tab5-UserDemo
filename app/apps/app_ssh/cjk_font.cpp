/*
 * cjk_font — res 分区点阵字库加载与 blit。
 */
#include "cjk_font.h"

#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"

namespace CjkFont {
namespace {
struct Header {
    bool ok = false;
    uint16_t w = 0, h = 0, rowBytes = 0, count = 0;
    const uint32_t* cps = nullptr;
    const uint8_t* bits = nullptr;
};

const Header& load()
{
    static Header s_h = []() -> Header {
        Header h;
        size_t sz = 0;
        const uint8_t* p = assets_part::find("cjk_font", &sz);
        if (!p || sz < 12) return h;
        if (p[0] != 'C' || p[1] != 'J' || p[2] != 'K' || p[3] != '1') return h;
        h.w        = p[4] | (p[5] << 8);
        h.h        = p[6] | (p[7] << 8);
        h.rowBytes = p[8] | (p[9] << 8);
        h.count    = p[10] | (p[11] << 8);
        const size_t cps_off = 12;
        const size_t bits_off = cps_off + (size_t)h.count * 4;
        if (sz < bits_off + (size_t)h.count * h.h * h.rowBytes) return h;
        h.cps  = reinterpret_cast<const uint32_t*>(p + cps_off);
        h.bits = p + bits_off;
        h.ok   = h.count > 0 && h.w > 0 && h.h > 0 && h.rowBytes > 0;
        return h;
    }();
    return s_h;
}

int findGlyph(const Header& h, uint32_t cp)
{
    int lo = 0, hi = (int)h.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (h.cps[mid] == cp) return mid;
        if (h.cps[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}
}  // namespace

bool ready() { return load().ok; }

bool hasGlyph(uint32_t cp)
{
    const Header& h = load();
    return h.ok && findGlyph(h, cp) >= 0;
}

void drawGlyphRgb565(uint16_t* buf, int bufW, int bufH,
                     uint32_t cp, int x, int y, uint16_t fg)
{
    const Header& h = load();
    if (!h.ok) return;
    int idx = findGlyph(h, cp);
    if (idx < 0) return;
    const uint8_t* g = h.bits + (size_t)idx * h.h * h.rowBytes;
    for (int gy = 0; gy < h.h; ++gy) {
        int dy = y + gy;
        if (dy < 0 || dy >= bufH) continue;
        const uint8_t* row = g + gy * h.rowBytes;
        uint16_t* line = buf + (size_t)dy * bufW;
        for (int gx = 0; gx < h.w; ++gx) {
            if (!((row[gx >> 3] >> (7 - (gx & 7))) & 1)) continue;
            int dx = x + gx;
            if (dx >= 0 && dx < bufW) line[dx] = fg;
        }
    }
}

}  // namespace CjkFont

#else  // 桌面: 无 res 分区

namespace CjkFont {
bool ready() { return false; }
bool hasGlyph(uint32_t) { return false; }
void drawGlyphRgb565(uint16_t*, int, int, uint32_t, int, int, uint16_t) {}
}  // namespace CjkFont

#endif
