#pragma once
// web 三档中文字体 (与真机逐字节相同的 cbin, 资产镜像内加载)
// 真机分档: lg=30px / md=24px / sm=20px (app_home 2026-10-31 分层终版)
#include <lvgl.h>
#include <cstddef>
#include "assets/assets_partition.h"
#include "cbin_font.h"

// cbin 生成器假设的 lv_font_t 布局 (riscv32: 36B, dsc@24)。
// 若 wasm32 布局不同, cbin_font_create 的 memcpy 会读错 dsc 指针 → 越界。
static_assert(sizeof(lv_font_t) == 36, "wasm32 lv_font_t layout != cbin 假设 (36B)");
static_assert(offsetof(lv_font_t, dsc) == 24, "wasm32 lv_font_t.dsc 偏移 != cbin 假设 (24)");

extern "C" const lv_font_t font_puhui_20_4;  // 兜底/临时回退

namespace webui {
// ⚠️ cbin 字体在 wasm32 首次渲染即 OOB (create 成功, 布局断言 36B/24 通过;
// 嫌疑: lv_font_fmt_txt_dsc_t/glyph_dsc 序列化布局与 wasm32 编译不一致)。
// 待查: 对比两端 struct 尺寸或改用 lv_font_conv 直出 C 数组。暂回退 20px。
#if 1  // FONT_WASM_DISABLED
inline const lv_font_t* font_30() { return &font_puhui_20_4; }
inline const lv_font_t* font_24() { return &font_puhui_20_4; }
inline const lv_font_t* font_20() { return &font_puhui_20_4; }
#else

void _dbg_font_log(const char* tag, void* p);

inline const lv_font_t* font_30()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_30");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    _dbg_font_log("font30", f);
    return f ? f : &font_puhui_20_4;
}
inline const lv_font_t* font_24()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    _dbg_font_log("font24", f);
    return f ? f : &font_puhui_20_4;
}
inline const lv_font_t* font_20()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    _dbg_font_log("font20", f);
    return f ? f : &font_puhui_20_4;
}

#endif  // FONT_WASM_DISABLED
}  // namespace webui
