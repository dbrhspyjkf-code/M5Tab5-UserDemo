// SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
// SPDX-License-Identifier: MIT
#pragma once

#include <lvgl.h>
#include <cstdint>

// Small, stateless LVGL component factory for the YRobot cockpit.  The view
// owns its data and callbacks; this header only guarantees a shared visual
// language across pages.
namespace reachy_ui {
constexpr uint32_t kBg = 0x061629;
constexpr uint32_t kSurface = 0x102642;
constexpr uint32_t kSurfaceAlt = 0x0B1E36;
constexpr uint32_t kStroke = 0x254562;
constexpr uint32_t kAccent = 0x32D7E6;
constexpr uint32_t kAccentSoft = 0x1F5A84;
constexpr uint32_t kText = 0xF4F8FC;
constexpr uint32_t kTextMuted = 0xA2B5CA;
constexpr uint32_t kOk = 0x42DFA3;
constexpr uint32_t kWarn = 0xF4B95D;
constexpr uint32_t kError = 0xFF6978;

inline lv_obj_t* makeSurface(lv_obj_t* parent, int x, int y, int w, int h,
                             uint32_t color = kSurface, int radius = 20) {
    lv_obj_t* surface = lv_obj_create(parent);
    lv_obj_set_size(surface, w, h);
    lv_obj_set_pos(surface, x, y);
    lv_obj_set_style_bg_color(surface, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(surface, lv_color_hex(kStroke), 0);
    lv_obj_set_style_border_width(surface, 1, 0);
    lv_obj_set_style_radius(surface, radius, 0);
    lv_obj_set_style_pad_all(surface, 0, 0);
    lv_obj_set_style_shadow_width(surface, 10, 0);
    lv_obj_set_style_shadow_color(surface, lv_color_hex(0x010916), 0);
    lv_obj_set_style_shadow_opa(surface, LV_OPA_30, 0);
    lv_obj_clear_flag(surface, LV_OBJ_FLAG_SCROLLABLE);
    return surface;
}

inline lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, int x, int y,
                           const lv_font_t* font, uint32_t color = kText) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

inline lv_obj_t* makeButton(lv_obj_t* parent, const char* text, int x, int y,
                            int w, int h, const lv_font_t* font,
                            uint32_t color = kAccentSoft,
                            uint32_t text_color = kText) {
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
    lv_obj_t* pill = makeSurface(parent, x, y, w, 40, kSurfaceAlt, 20);
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
