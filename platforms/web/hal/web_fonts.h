#pragma once
// web 三档中文字体。真机分档: lg=30px / md=24px / sm=20px (app_home 分层终版)。
// 30/24px: lv_font_conv 直出 C 数组 (与 20px 同一路径, wasm32 安全);
// cbin 序列化在 wasm32 首次渲染 OOB 已弃用 —— 详见 memory/tab5-web-project。
#include <lvgl.h>

extern "C" const lv_font_t font_puhui_20_4;
extern "C" const lv_font_t font_zh_web_30;
extern "C" const lv_font_t font_zh_web_24;

namespace webui {

inline const lv_font_t* font_30() { return &font_zh_web_30; }
inline const lv_font_t* font_24() { return &font_zh_web_24; }
inline const lv_font_t* font_20() { return &font_puhui_20_4; }

}  // namespace webui
