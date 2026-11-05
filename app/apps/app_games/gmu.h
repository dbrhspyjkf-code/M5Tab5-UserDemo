#pragma once
/*
 * gmu — app_games 公共小件 (2026-11-05)。
 *
 * 4 个游戏 cpp (game_memory/popstar/mines/sudoku) 共用的字体访问、
 * 单调时钟、随机源和控件 maker。字体链路抄自 app_lottery (设备端
 * cbin 30px 全字库 + res 分区 20px; 桌面/网页端 puhui 20 兜底)。
 */
#include <lvgl.h>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <random>

#ifdef PLATFORM_BUILD_DESKTOP
extern "C" const lv_font_t font_puhui_20_4;
#elif defined(PLATFORM_BUILD_WEB)
#include "web_fonts.h"
#else
#include <esp_timer.h>
#include <cbin_font.h>
#include "../utils/assets/assets_partition.h"
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
#endif

namespace gmu {

// ── 字体 ──────────────────────────────────────────────────────────────────
inline const lv_font_t* zh30()
{
#ifdef PLATFORM_BUILD_DESKTOP
    return &font_puhui_20_4;
#elif defined(PLATFORM_BUILD_WEB)
    return webui::font_30();
#else
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
#endif
}

inline const lv_font_t* zh20()
{
#ifdef PLATFORM_BUILD_DESKTOP
    return &font_puhui_20_4;
#elif defined(PLATFORM_BUILD_WEB)
    return webui::font_20();
#else
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
#endif
}

// 备用: 候选小数字/计数 (montserrat 全量启用, lv_conf.h)
inline const lv_font_t* mono14() { return &lv_font_montserrat_14; }
inline const lv_font_t* mono16() { return &lv_font_montserrat_16; }
inline const lv_font_t* mono20() { return &lv_font_montserrat_20; }

// ── 单调毫秒 (app_lottery/stocks 同款) ────────────────────────────────────
inline int64_t mono_ms()
{
#ifdef PLATFORM_BUILD_DESKTOP
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    return (int64_t)(esp_timer_get_time() / 1000);
#endif
}

// ── 随机源 (IDF std::random_device 走硬件 RNG; 桌面平台原生) ───────────────
inline uint32_t rng_u32()
{
    static std::mt19937 rng(std::random_device{}());
    return rng();
}
inline int rng_range(int n) { return n > 0 ? (int)(rng_u32() % (uint32_t)n) : 0; }

// ── 控件 maker ────────────────────────────────────────────────────────────
// tab/难度 chip 按钮: 深底圆角, CHECKABLE, 选中=主题强调色 (lottery tab 同款)
inline lv_obj_t* chip_btn(lv_obj_t* parent, const char* txt, int x, int y,
                          int w, int h, uint32_t accent, lv_event_cb_t cb,
                          void* user_data, const lv_font_t* font = nullptr)
{
    lv_obj_t* btn = lv_button_create(parent);
    if (!btn) return nullptr;
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(accent), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xB8BCC0), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font ? font : zh20(), 0);
    lv_obj_center(l);
    return btn;
}

// 普通动作按钮 (重开/擦除等, 不可选中)
inline lv_obj_t* action_btn(lv_obj_t* parent, const char* txt, int x, int y,
                            int w, int h, lv_event_cb_t cb, void* user_data,
                            const lv_font_t* font = nullptr)
{
    lv_obj_t* btn = lv_button_create(parent);
    if (!btn) return nullptr;
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xE8EAEC), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font ? font : zh20(), 0);
    lv_obj_center(l);
    return btn;
}

// 次要文字标签
inline lv_obj_t* soft_label(lv_obj_t* parent, const char* txt, int x, int y,
                            uint32_t color, const lv_font_t* font = nullptr)
{
    lv_obj_t* l = lv_label_create(parent);
    if (!l) return nullptr;
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font ? font : zh20(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

}  // namespace gmu
