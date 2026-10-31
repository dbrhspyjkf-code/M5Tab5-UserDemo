#pragma once
/*
 * canvas_draw.h — ARGB8888 画布像素级绘制助手 (2026-10-31)。
 *
 * 从 app_stocks.cpp 匿名 namespace 提取参数化 (源头保持不动, 两处并存:
 * stocks 已验收勿扰; 新图从这里引)。用法: 调用方已在 UI/LVGL 上下文,
 * 缓冲为 lv_canvas_set_buffer 挂好的 ARGB8888 行主序内存。
 * 颜色传 0x00RRGGBB, 写入时自动 OR 不透明 alpha。
 */
#include <cstdint>
#include <cstdlib>

namespace canvas_draw {

inline void px_fill(void* pix, int W, int H, uint32_t color)
{
    uint32_t* p = (uint32_t*)pix;
    for (int i = 0; i < W * H; i++) p[i] = color | 0xFF000000;
}

inline void px_rect(void* pix, int W, int H, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++) {
        uint32_t* row = (uint32_t*)pix + y * W;
        for (int x = x0; x <= x1; x++) row[x] = color | 0xFF000000;
    }
}

inline void px_line(void* pix, int W, int H, int x0, int y0, int x1, int y1, uint32_t color)
{
    // Bresenham
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H)
            ((uint32_t*)pix)[y0 * W + x0] = color | 0xFF000000;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// 两色线性混合 (t=0 → a, t=1 → b)。历史柱向底色调暗用。
inline uint32_t px_blend(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    const int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    const int r = ar + (int)((br - ar) * t);
    const int g = ag + (int)((bg - ag) * t);
    const int bl = ab + (int)((bb - ab) * t);
    return 0xFF000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)bl;
}

}  // namespace canvas_draw
