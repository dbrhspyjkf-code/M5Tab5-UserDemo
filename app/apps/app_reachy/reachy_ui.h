// SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
// SPDX-License-Identifier: MIT
#pragma once

#include <lvgl.h>
#include <cstdint>

#include "../utils/theme/theme.h"

// Small, stateless LVGL component factory for the YRobot cockpit.  The view
// owns its data and callbacks; this header only guarantees a shared visual
// language across pages.
namespace reachy_ui {
// 配色: 实时读全局主题 (utils/theme/theme.h) — 函数形式, 默认参数在调用点求值
inline uint32_t kBg()         { return 0x000000; }
inline uint32_t kSurface()    { return theme::cur().card[1]; }
inline uint32_t kSurfaceAlt() { return theme::cur().card[2]; }
inline uint32_t kStroke()     { return theme::cur().border; }
inline uint32_t kAccent()     { return theme::cur().accent; }
inline uint32_t kAccentSoft() { return theme::cur().card[0]; }
inline uint32_t kText()       { return theme::cur().text_hi; }
inline uint32_t kTextMuted()  { return theme::cur().text_dim; }
inline uint32_t kOk()         { return theme::cur().ok; }
inline uint32_t kWarn()       { return theme::cur().warn; }
inline uint32_t kError()      { return theme::cur().danger; }

inline lv_obj_t* makeSurface(lv_obj_t* parent, int x, int y, int w, int h,
                             uint32_t color = kSurface(), int radius = 20) {
    lv_obj_t* surface = lv_obj_create(parent);
    lv_obj_set_size(surface, w, h);
    lv_obj_set_pos(surface, x, y);
    lv_obj_set_style_bg_color(surface, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(surface, lv_color_hex(kStroke()), 0);
    lv_obj_set_style_border_width(surface, 1, 0);
    lv_obj_set_style_radius(surface, radius, 0);
    lv_obj_set_style_pad_all(surface, 0, 0);
    lv_obj_set_style_shadow_width(surface, 10, 0);
    lv_obj_set_style_shadow_color(surface, lv_color_hex(theme::cur().track), 0);
    lv_obj_set_style_shadow_opa(surface, LV_OPA_30, 0);
    lv_obj_clear_flag(surface, LV_OBJ_FLAG_SCROLLABLE);
    return surface;
}

inline lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, int x, int y,
                           const lv_font_t* font, uint32_t color = kText()) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

inline lv_obj_t* makeButton(lv_obj_t* parent, const char* text, int x, int y,
                            int w, int h, const lv_font_t* font,
                            uint32_t color = kAccentSoft(),
                            uint32_t text_color = kText()) {
    lv_obj_t* button = makeSurface(parent, x, y, w, h, color, 16);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return button;
}

inline lv_obj_t* makeStatusPill(lv_obj_t* parent, const char* text, int x, int y,
                                int w, const lv_font_t* font, uint32_t color) {
    lv_obj_t* pill = makeSurface(parent, x, y, w, 40, kSurfaceAlt(), 20);
    lv_obj_set_style_border_color(pill, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(pill, 0, 0);
    lv_obj_t* label = lv_label_create(pill);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return pill;
}
}  // namespace reachy_ui
