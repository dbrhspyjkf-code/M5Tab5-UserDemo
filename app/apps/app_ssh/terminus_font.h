#pragma once
/*
 * terminus_font — Terminus 点阵字数据 (移植自 Tab5_SSH_Client 的
 * TerminusBitmap, 弃 M5Canvas/PROGMEM, 自带 RGB565 直写 blit)。
 * 4 档字号 (8x16/10x20/14x28/16x32→18x36), 252 码点: ASCII + 制表符
 * (U+2500 块) + Powerline。Terminus 字体版权 Dimitar Toshkov Zhekov,
 * SIL OFL 1.1 (许可全文见 THIRD_PARTY_LICENSES, 上游 Tab5_SSH_Client)。
 * CJK 无字形: 渲染层画豆腐框。
 */
#include <stdint.h>
#include <stddef.h>

namespace TerminusFont {

struct Font {
    uint8_t srcW;
    uint8_t srcH;
    uint8_t targetW;
    uint8_t targetH;
    uint8_t rowBytes;
    uint16_t glyphCount;
    const uint16_t* codepoints;
    const uint32_t* offsets;
    const uint8_t* bitmap;
};

const Font& fontForHeight(uint8_t targetHeight);
bool hasGlyph(const Font& font, uint32_t cp);

// 在 RGB565 缓冲上画字形 (fg 前景色, 透明背景 — 背景由调用方先铺)。
// 返回实际写入宽度 (px)。
int drawGlyphRgb565(uint16_t* buf, int bufW, int bufH,
                    const Font& font, uint32_t cp, int x, int y, uint16_t fg);

}  // namespace TerminusFont
