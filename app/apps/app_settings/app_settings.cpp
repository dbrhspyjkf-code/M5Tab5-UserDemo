#include "app_settings.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#else
extern "C" const lv_font_t font_puhui_20_4;
#endif

static const std::string _tag = "app-tools";

// Xiaozhi's puhui common 30px cbin font — GB2312 ~3500 glyphs, XIP from flash.
static const lv_font_t* zh_font_30()
{
#ifndef PLATFORM_BUILD_DESKTOP
    extern const uint8_t font_puhui_common_30_4_bin_start[] asm("_binary_font_puhui_common_30_4_bin_start");
    static lv_font_t* f = nullptr;
    if (!f) f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
#else
    return &font_puhui_20_4;
#endif
}

// Chinese font defined in app_ha/view/font_zh_36.c (now includes 工具计算器).
extern lv_font_t font_zh_36;

// Tool tile icons (ARGB8888) defined in app_settings/calc_icon.c & fx_icon.c & unit_icon.c.
extern const lv_image_dsc_t calc_icon;  // 80x112
extern const lv_image_dsc_t fx_icon;    // 100x100
extern const lv_image_dsc_t unit_icon;  // 200x200

// Calculator fonts (Arial Unicode subsets) defined in this app's font_calc_*.c.
extern const lv_font_t font_calc_big;   // 80px result line
extern const lv_font_t font_calc_op;    // 40px button labels (has × ÷)
extern const lv_font_t font_calc_expr;  // 30px expression line

// ── Palette ─────────────────────────────────────────────────────────────────
// Tools-page colors mirror the HA "灯光" dashboard cards (app_ha/view.cpp).
static constexpr uint32_t C_BG       = 0x0A1A2E;  // page background (HA C_BG)
static constexpr uint32_t C_ACCENT   = 0x4FA3FF;
static constexpr uint32_t C_CARD     = 0x13304E;  // HA card background
static constexpr uint32_t C_CARD_PR  = 0x1A3A5C;  // HA pressed fill (C_ACCENT_SOFT)
static constexpr uint32_t C_TEXT     = 0xEAF3FB;  // HA primary text

// Calculator palette (matches the reference iPad calculator).
static constexpr uint32_t CALC_BG   = 0x000000;
static constexpr uint32_t CALC_NUM  = 0x3A3A3C;  // dark grey digit keys
static constexpr uint32_t CALC_FUNC = 0x545456;  // medium grey  ⌫ / AC / %
static constexpr uint32_t CALC_OP   = 0xFF9F0A;  // orange operators
static constexpr uint32_t CALC_TXT  = 0xFFFFFF;
static constexpr uint32_t CALC_EXPR = 0x9A9AA0;  // grey expression line

// ── Currency table + live rate cache ─────────────────────────────────────────
namespace {
struct Ccy { const char* code; const char* cn; double per_usd; };
// Static fallback rates (units per 1 USD); overwritten at runtime by the HA
// server's /rate proxy (see _fxFetch). Order must match g_fx_rate below.
const Ccy FX[] = {
    {"USD", "美元",   1.00},
    {"CNY", "人民币", 7.25},
    {"EUR", "欧元",   0.92},
    {"JPY", "日元",   157.0},
    {"GBP", "英镑",   0.79},
    {"HKD", "港元",   7.81},
    {"KRW", "韩元",   1380.0},
    {"AUD", "澳元",   1.51},
};
constexpr int FX_N = (int)(sizeof(FX) / sizeof(FX[0]));

// Live per-USD rates (seeded from the static table, overwritten by /rate). The
// detached fetch worker writes under g_fx_mutex; the UI reads under it too.
std::mutex        g_fx_mutex;
double            g_fx_rate[FX_N] = {1.00, 7.25, 0.92, 157.0, 0.79, 7.81, 1380.0, 1.51};
std::atomic<bool> g_fx_updated{false};   // set by worker, consumed in onRunning
std::atomic<bool> g_fx_fetching{false};
std::string       g_fx_source = "static";

// ── Email cache (worker thread fills, UI thread reads) ──────────────────────
// Mirrors the g_fx_* pattern: detached fetch worker writes g_emails under
// g_email_mutex and flips g_email_updated; the UI loop detects the flag in
// onRunning() and rebuilds the lv_list via _emailRefresh().
struct EmailItem { std::string from, subject, date, folder; };
std::mutex             g_email_mutex;
std::vector<EmailItem> g_emails;
std::atomic<int>       g_email_total{0};        // 总未读数, 从 j["count"] 取
std::atomic<bool>      g_email_updated{false};
std::atomic<bool>      g_email_fetching{false};
std::atomic<bool>      g_email_error{false};
std::string            g_email_error_msg;
}  // namespace

AppSettings::AppSettings()
{
    setAppInfo().name = "工具";
}

void AppSettings::onCreate() {}

void AppSettings::onOpen()
{
    mclog::tagInfo(_tag, "open");
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    _buildToolsPage();
    lv_screen_load(_scr);
    _installSwipeGesture();
}

void AppSettings::onRunning()
{
    // Apply freshly fetched live rates (worker thread only flips the flag; the
    // label/recompute must happen on the UI loop).
    if (g_fx_updated.exchange(false) && _fx_page) {
        _fxRecompute();
    }
    // Same pattern for the email cache: worker fills g_emails + flips
    // g_email_updated; we rebuild the lv_list on the UI loop.
    if (g_email_updated.exchange(false) && _email_page) {
        _emailRefresh();
    }
    if (g_email_error.exchange(false) && _email_page) {
        _emailShowError();
    }
    // (移除 500ms 倒计时 — lv_label_set_text 触发 lvgl layer 重新分配 1200x16 横条
    //  buffer (76.8KB), 在 ESP32-P4 上连续失败, 渲染管线堵塞, onRunning 自己都
    //  拿不到 LVGL lock, status 文字卡死. worker 完成时改一次 status 即可.)
}

void AppSettings::onClose()
{
    mclog::tagInfo(_tag, "close");
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _tools_page = _calc_page = _fx_page = _unit_page = nullptr;
    _calc_expr_lbl = _calc_res_lbl = nullptr;
    _fx_from_dd = _fx_to_dd = _fx_amt_lbl = _fx_res_lbl = _fx_rate_lbl = nullptr;
    _unit_cat_dd = _unit_from_dd = _unit_to_dd = _unit_amt_lbl = _unit_res_lbl = _unit_eq_lbl = nullptr;
    _email_page = _email_title = _email_status = _email_list = nullptr;
    g_email_total.store(0);
    g_email_updated.store(false);
    g_email_error.store(false);
    g_email_error_msg.clear();
    g_email_fetching.store(false);  // 防止重入保护卡死
    if (_close_cb) _close_cb();
}

// ─── Swipe-up gesture closes the whole app (back to home) ───────────────────
void AppSettings::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppSettings*>(udata);
                        if (!app) return;
                        // Back out of the deepest open sub-page first.
                        if (app->_email_page)   app->_closeEmail();
                        else if (app->_unit_page)   app->_closeUnit();
                        else if (app->_fx_page) app->_closeFx();
                        else if (app->_calc_page) app->_closeCalculator();
                        else                     app->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppSettings::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Tools launcher page
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_buildToolsPage()
{
    _tools_page = lv_obj_create(_scr);
    lv_obj_set_size(_tools_page, 1280, 720);
    lv_obj_set_pos(_tools_page, 0, 0);
    lv_obj_set_style_bg_opa(_tools_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tools_page, 0, 0);
    lv_obj_set_style_pad_all(_tools_page, 0, 0);
    lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_tools_page);
    lv_label_set_text(title, "工具");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, zh_font_30(), 0);

    // Tool card — same style as the HA "灯光" device cards: dark-blue rounded
    // card with a soft shadow, icon on the left, name on the right.
    // img==nullptr 走"点阵分支": 用 LV_SYMBOL_BULLET 拼一个 6x6 的 LED 矩阵缩略图
    // (象征 8x8 Puzzle), y_pos 默认 140 (第 1 行), 第 2 行 tile 传 310.
    // kind==0 (默认) 走点阵分支 (灯阵); kind==1 走信封分支 (邮件).
    auto make_tile = [&](int x, const lv_image_dsc_t* img, int img_w,
                         const char* label, lv_event_cb_t cb,
                         int img_scale = 256, int icon_x = 12,
                         int img_scale_y = -1, int y_pos = 140,
                         int kind = 0) {
        lv_obj_t* tile = lv_obj_create(_tools_page);
        lv_obj_set_size(tile, 380, 140);
        lv_obj_align(tile, LV_ALIGN_TOP_LEFT, x, y_pos);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_shadow_width(tile, 18, 0);
        lv_obj_set_style_shadow_color(tile, lv_color_hex(0x5A7A9C), 0);
        lv_obj_set_style_shadow_opa(tile, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(tile, 4, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, this);

        lv_obj_t* name = lv_label_create(tile);
        lv_label_set_text(name, label);
        lv_obj_set_style_text_font(name, zh_font_30(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);

        if (img) {
            lv_obj_t* icon = lv_image_create(tile);
            lv_image_set_src(icon, img);
            if (img_scale_y > 0) {
                // Non-uniform scale (e.g. stretch a portrait icon to a square box).
                lv_image_set_scale_x(icon, img_scale);
                lv_image_set_scale_y(icon, img_scale_y);
            } else if (img_scale != 256) {
                lv_image_set_scale(icon, img_scale);
            }
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, icon_x, 0);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, icon_x + img_w + 16, 0);
        } else {
            // kind 分支: 0=点阵 (灯阵) / 1=信封 (邮件)
            if (kind == 1) {
                // 信封 (邮件 tile) — 白色矩形 + 顶上一条横线表示封口.
                // 跟点阵分支一样完全脱离字体, 用纯 lv_obj 矩形拼.
                const int EW = 84, EH = 56;
                const int ex = icon_x + 12;
                const int ey = (140 - EH) / 2;
                lv_obj_t* env_body = lv_obj_create(tile);
                lv_obj_set_size(env_body, EW, EH);
                lv_obj_set_pos(env_body, ex, ey);
                lv_obj_set_style_bg_color(env_body, lv_color_hex(C_TEXT), 0);
                lv_obj_set_style_bg_opa(env_body, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(env_body, 6, 0);
                lv_obj_set_style_border_width(env_body, 0, 0);
                lv_obj_set_style_shadow_width(env_body, 0, 0);
                lv_obj_set_style_pad_all(env_body, 0, 0);
                lv_obj_clear_flag(env_body, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(env_body, LV_OBJ_FLAG_CLICKABLE);
                // 关键: 让 env_body 不吞 click 事件, 冒泡到父 tile
                lv_obj_add_flag(env_body, LV_OBJ_FLAG_EVENT_BUBBLE);
                // 封口横线
                lv_obj_t* flap = lv_obj_create(tile);
                lv_obj_set_size(flap, EW - 16, 2);
                lv_obj_set_pos(flap, ex + 8, ey + 18);
                lv_obj_set_style_bg_color(flap, lv_color_hex(0x6A8AAA), 0);
                lv_obj_set_style_bg_opa(flap, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(flap, 0, 0);
                lv_obj_set_style_radius(flap, 0, 0);
                lv_obj_set_style_shadow_width(flap, 0, 0);
                lv_obj_set_style_pad_all(flap, 0, 0);
                lv_obj_clear_flag(flap, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(flap, LV_OBJ_FLAG_CLICKABLE);
                // 关键: flap 不吞 click 事件, 冒泡到父 tile
                lv_obj_add_flag(flap, LV_OBJ_FLAG_EVENT_BUBBLE);
                // label 右移
                lv_obj_align(name, LV_ALIGN_LEFT_MID, ex + EW + 16, 0);
            } else {
                // 3x3 实心圆点网格, 象征 8x8 LED 矩阵缩略图.
                // 不用 LV_SYMBOL_BULLET (依赖字体覆盖, 16px montserrat 渲染可能出方框),
                // 直接画 9 个圆角矩形, 完全脱离字体. 9 个圆点既省资源也清晰.
                const int CELL = 24, GAP = 6, COLS = 3, ROWS = 3;
                const int GRID_W = COLS * CELL + (COLS - 1) * GAP;
                const int start_x = icon_x + 8;
                const int start_y = (140 - ROWS * CELL - (ROWS - 1) * GAP) / 2;
                for (int r = 0; r < ROWS; r++) {
                    for (int c = 0; c < COLS; c++) {
                        lv_obj_t* dot = lv_obj_create(tile);
                        lv_obj_set_size(dot, CELL, CELL);
                        lv_obj_set_pos(dot,
                            start_x + c * (CELL + GAP),
                            start_y + r * (CELL + GAP));
                        lv_obj_set_style_bg_color(dot, lv_color_hex(C_ACCENT), 0);
                        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                        lv_obj_set_style_radius(dot, CELL / 2, 0);  // 圆形
                        lv_obj_set_style_border_width(dot, 0, 0);
                        lv_obj_set_style_shadow_width(dot, 0, 0);
                        lv_obj_set_style_pad_all(dot, 0, 0);
                        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                        // 关键: 9 个 dot 不吞 click, 冒泡到父 tile
                        // (灯阵 tile 也用同一份 make_tile, 之前不点就是这问题)
                        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);
                    }
                }
                // label 整体右移更多, 避免压到点阵
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start_x + GRID_W + 12, 0);
            }
        }
    };

    // All three tool icons render at 130×130 to match the 汇率 (fx) icon.
    //   fx_icon  : 100×100 native × 1.30 (scale 333) → 130×130
    //   unit_icon: 130×130 native × 1.00             → 130×130
    //   calc_icon: 130×130 native × 1.00             → 130×130
    make_tile(35,  &calc_icon, 130, "计算器",  _toolBtn_cb);
    make_tile(450, &fx_icon,   150, "汇率",    _toolFx_cb,   384);  // 384 = 256 × 1.5x → 150×150
    make_tile(865, &unit_icon, 130, "单位换算", _toolUnit_cb);

    // 第 2 行: 灯阵 (Unit-Puzzle 8x8 WS2812 矩阵测试 App)
    //   y=140 (第 1 行) + 140 (tile 高) + 30 (行间距) = 310
    //   图标走 make_tile 的 nullptr 分支, 用 LV_SYMBOL_BULLET 拼 6x6 LED 缩略图
    make_tile(35, nullptr, 130, "灯  阵", _toolPuzzle_cb, 256, 12, -1, 310);

    // 第 3 行: 邮件 (调 hermes :8766/unread_emails 显示未读列表)
    //   y=310 + 140 + 30 = 480, 走 kind=1 信封分支
    make_tile(35, nullptr, 130, "邮  件", _toolMail_cb, 256, 12, -1, 480, 1);
}

void AppSettings::_toolBtn_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openCalculator();
}

void AppSettings::_toolPuzzle_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (self->_puzzle_id > 0) {
        mooncake::GetMooncake().openApp(self->_puzzle_id);
    }
}

void AppSettings::_toolFx_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openFx();
}

void AppSettings::_toolUnit_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openUnit();
}

void AppSettings::_toolMail_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openEmail();
}

void AppSettings::_emailRefresh_cb(lv_event_t* e)
{
    // 邮件子页 "刷新" 按钮 → 重新拉取. _emailFetch 内部 exchange(true) 跳过 in-flight.
    auto* app = static_cast<AppSettings*>(lv_event_get_user_data(e));
    mclog::tagInfo(_tag, "email refresh button clicked");
    app->_emailFetch();
}

// ════════════════════════════════════════════════════════════════════════════
//  Calculator
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openCalculator()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _calc_page = lv_obj_create(_scr);
    lv_obj_set_size(_calc_page, 1280, 720);
    lv_obj_set_pos(_calc_page, 0, 0);
    lv_obj_set_style_bg_color(_calc_page, lv_color_hex(CALC_BG), 0);
    lv_obj_set_style_border_width(_calc_page, 0, 0);
    lv_obj_set_style_radius(_calc_page, 0, 0);
    lv_obj_set_style_pad_all(_calc_page, 0, 0);
    lv_obj_clear_flag(_calc_page, LV_OBJ_FLAG_SCROLLABLE);

    // ── Display: expression (small, grey) over result (big, white), right-aligned.
    _calc_expr_lbl = lv_label_create(_calc_page);
    lv_label_set_text(_calc_expr_lbl, "");
    lv_obj_set_style_text_font(_calc_expr_lbl, &font_calc_expr, 0);
    lv_obj_set_style_text_color(_calc_expr_lbl, lv_color_hex(CALC_EXPR), 0);
    lv_obj_align(_calc_expr_lbl, LV_ALIGN_TOP_RIGHT, -36, 86);

    _calc_res_lbl = lv_label_create(_calc_page);
    lv_label_set_text(_calc_res_lbl, "0");
    lv_obj_set_style_text_font(_calc_res_lbl, &font_calc_big, 0);
    lv_obj_set_style_text_color(_calc_res_lbl, lv_color_hex(CALC_TXT), 0);
    lv_obj_align(_calc_res_lbl, LV_ALIGN_TOP_RIGHT, -36, 130);

    // ── Button grid: 5 columns × 4 rows ──────────────────────────────────────
    struct Key { const char* label; const char* code; uint32_t color; bool sym; };
    // code is what _calcInput receives. "DEL"/"AC"/"PCT"/"NEG" are special.
    static const Key KEYS[4][5] = {
        {{"7","7",CALC_NUM,false},{"8","8",CALC_NUM,false},{"9","9",CALC_NUM,false},
         {LV_SYMBOL_BACKSPACE,"DEL",CALC_FUNC,true},{"\xC3\xB7","/",CALC_OP,false}},
        {{"4","4",CALC_NUM,false},{"5","5",CALC_NUM,false},{"6","6",CALC_NUM,false},
         {"AC","AC",CALC_FUNC,false},{"\xC3\x97","*",CALC_OP,false}},
        {{"1","1",CALC_NUM,false},{"2","2",CALC_NUM,false},{"3","3",CALC_NUM,false},
         {"%","PCT",CALC_FUNC,false},{"-","-",CALC_OP,false}},
        {{"+/-","NEG",CALC_NUM,false},{"0","0",CALC_NUM,false},{".",".",CALC_NUM,false},
         {"=","=",CALC_OP,false},{"+","+",CALC_OP,false}},
    };

    const int SIDE = 24, GAP = 16, COLS = 5, ROWS = 4;
    const int grid_top = 270;
    const int bw = (1280 - 2 * SIDE - (COLS - 1) * GAP) / COLS;   // ~233
    const int bh = (720 - grid_top - SIDE - (ROWS - 1) * GAP) / ROWS;  // ~95

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const Key& k = KEYS[r][c];
            lv_obj_t* b = lv_obj_create(_calc_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, SIDE + c * (bw + GAP), grid_top + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(k.color), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);   // pill
            lv_obj_set_style_border_width(b, 0, 0);
            // Pressed: lighten.
            lv_obj_set_style_bg_color(b,
                lv_color_hex(k.color == CALC_OP ? 0xFFBE5C
                           : k.color == CALC_NUM ? 0x5A5A5C : 0x6E6E70),
                LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _calcKey_cb, LV_EVENT_CLICKED, this);

            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    _calcReset();
}

void AppSettings::_back_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeCalculator();
}

void AppSettings::_closeCalculator()
{
    if (_calc_page) {
        lv_obj_delete(_calc_page);
        _calc_page = nullptr;
        _calc_expr_lbl = _calc_res_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

// ── Calculator logic ─────────────────────────────────────────────────────────
namespace {
// Group the integer part of a numeric string with thousands separators,
// preserving sign and any fractional part / trailing dot as typed.
std::string group(const std::string& s)
{
    std::string sign, num = s;
    if (!num.empty() && (num[0] == '-' || num[0] == '+')) { sign = num.substr(0, 1); num = num.substr(1); }
    size_t dot = num.find('.');
    std::string ip = (dot == std::string::npos) ? num : num.substr(0, dot);
    std::string fp = (dot == std::string::npos) ? ""  : num.substr(dot);  // includes '.'
    std::string out;
    int cnt = 0;
    for (int i = (int)ip.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), ip[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    if (ip.empty()) out = "0";
    return sign + out + fp;
}

// Format a double for display: up to 10 significant digits, trailing zeros
// trimmed, then thousands-grouped.
std::string fmtNum(double v)
{
    if (std::isnan(v) || std::isinf(v)) return "Error";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.10g", v);
    return group(buf);
}

// Currency-converter formatter: at most 3 decimal places, trailing zeros
// trimmed, then thousands-grouped. Keeps the result line short (full-precision
// values like 0.1476688479 otherwise overflow the card).
std::string fxFmt(double v)
{
    if (std::isnan(v) || std::isinf(v)) return "Error";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        size_t e = s.size();
        while (e > 0 && s[e - 1] == '0') e--;
        if (e > 0 && s[e - 1] == '.') e--;
        s = s.substr(0, e);
    }
    return group(s);
}

double applyOp(double a, double b, char op)
{
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0.0 ? a / b : NAN;
    }
    return b;
}

const char* opSym(char op)
{
    switch (op) {
        case '+': return "+";
        case '-': return "-";
        case '*': return "\xC3\x97";  // ×
        case '/': return "\xC3\xB7";  // ÷
    }
    return "";
}
}  // namespace

void AppSettings::_calcReset()
{
    _entry = "0";
    _acc   = 0.0;
    _op    = 0;
    _fresh = true;
    _lhs_disp.clear();
    _calcRefresh();
}

void AppSettings::_calcRefresh()
{
    if (_calc_res_lbl)  lv_label_set_text(_calc_res_lbl, group(_entry).c_str());
    if (_calc_expr_lbl) {
        std::string ex;
        if (_op) ex = _lhs_disp + opSym(_op) + (_fresh ? "" : group(_entry));
        lv_label_set_text(_calc_expr_lbl, ex.c_str());
    }
}

void AppSettings::_calcInput(const char* key)
{
    std::string k = key;

    if (k == "AC") { _calcReset(); return; }

    if (k == "DEL") {
        if (!_fresh && _entry != "0") {
            _entry.pop_back();
            if (_entry.empty() || _entry == "-") _entry = "0";
        }
        _calcRefresh();
        return;
    }

    if (k == "NEG") {
        if (_entry != "0") {
            if (_entry[0] == '-') _entry = _entry.substr(1);
            else                  _entry = "-" + _entry;
        }
        _calcRefresh();
        return;
    }

    if (k == "PCT") {
        double v = atof(_entry.c_str()) / 100.0;
        _entry = fmtNum(v);
        // strip grouping commas back out so further edits stay numeric
        std::string t; for (char ch : _entry) if (ch != ',') t += ch;
        _entry = t;
        _fresh = true;
        _calcRefresh();
        return;
    }

    if (k == ".") {
        if (_fresh) { _entry = "0."; _fresh = false; }
        else if (_entry.find('.') == std::string::npos) _entry += ".";
        _calcRefresh();
        return;
    }

    // Digit 0-9
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') {
        if (_fresh) { _entry = k; _fresh = false; }
        else if (_entry == "0") _entry = k;
        else if (_entry == "-0") _entry = "-" + k;
        else _entry += k;
        _calcRefresh();
        return;
    }

    // Operators + - * /
    if (k == "+" || k == "-" || k == "*" || k == "/") {
        if (_op && !_fresh) {
            _acc = applyOp(_acc, atof(_entry.c_str()), _op);
        } else if (!_op) {
            _acc = atof(_entry.c_str());
        }
        _op = k[0];
        _lhs_disp = fmtNum(_acc);
        _entry = fmtNum(_acc);
        { std::string t; for (char ch : _entry) if (ch != ',') t += ch; _entry = t; }
        _fresh = true;
        _calcRefresh();
        return;
    }

    // Equals
    if (k == "=") {
        if (_op) {
            double rhs = atof(_entry.c_str());
            std::string rhs_disp = group(_entry);
            double res = applyOp(_acc, rhs, _op);
            // Show full "lhs op rhs" expression, result below.
            if (_calc_expr_lbl)
                lv_label_set_text(_calc_expr_lbl, (_lhs_disp + opSym(_op) + rhs_disp).c_str());
            _entry = fmtNum(res);
            { std::string t; for (char ch : _entry) if (ch != ',') t += ch; _entry = t; }
            if (_calc_res_lbl) lv_label_set_text(_calc_res_lbl, group(_entry).c_str());
            _acc = res;
            _op = 0;
            _fresh = true;
        }
        return;
    }
}

void AppSettings::_calcKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_calcInput(code);
}

// ════════════════════════════════════════════════════════════════════════════
//  Currency converter (汇率)
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openFx()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _fx_page = lv_obj_create(_scr);
    lv_obj_set_size(_fx_page, 1280, 720);
    lv_obj_set_pos(_fx_page, 0, 0);
    lv_obj_set_style_bg_color(_fx_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_fx_page, 0, 0);
    lv_obj_set_style_radius(_fx_page, 0, 0);
    lv_obj_set_style_pad_all(_fx_page, 0, 0);
    lv_obj_clear_flag(_fx_page, LV_OBJ_FLAG_SCROLLABLE);

    // Title.
    lv_obj_t* title = lv_label_create(_fx_page);
    lv_label_set_text(title, "汇率");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, zh_font_30(), 0);

    // Currency code options for the dropdowns (ASCII → renders in montserrat).
    std::string opts;
    for (int i = 0; i < FX_N; i++) { if (i) opts += "\n"; opts += FX[i].code; }

    // ── Conversion panel (left) ──────────────────────────────────────────────
    const int CARD_X = 40, CARD_W = 600, CARD_H = 120;

    auto make_conv_card = [&](int y, bool is_from) -> lv_obj_t* {
        lv_obj_t* c = lv_obj_create(_fx_page);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, CARD_X, y);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_shadow_width(c, 18, 0);
        lv_obj_set_style_shadow_color(c, lv_color_hex(0x5A7A9C), 0);
        lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dd = lv_dropdown_create(c);
        lv_obj_set_size(dd, 130, 60);
        lv_obj_align(dd, LV_ALIGN_LEFT_MID, 16, 0);
        lv_dropdown_set_options(dd, opts.c_str());
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_30, 0);
        lv_obj_t* dd_list = lv_dropdown_get_list(dd);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_30, 0);
        lv_obj_add_event_cb(dd, _fxDd_cb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t* val = lv_label_create(c);
        lv_label_set_text(val, "0");
        lv_obj_set_style_text_font(val, &font_calc_op, 0);
        lv_obj_set_style_text_color(val,
            lv_color_hex(is_from ? CALC_TXT : C_ACCENT), 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0);

        if (is_from) { _fx_from_dd = dd; _fx_amt_lbl = val; }
        else         { _fx_to_dd   = dd; _fx_res_lbl = val; }
        return c;
    };

    make_conv_card(120, true);
    make_conv_card(340, false);

    // Swap button centered in the gap between the two cards.
    // FROM card: 120..(120+CARD_H); TO card starts at 340 → gap centre = 290.
    lv_obj_t* swap = lv_obj_create(_fx_page);
    lv_obj_set_size(swap, 64, 64);
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 32, 290 - 32);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _fxSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_center(swap_lbl);
    lv_obj_set_style_text_color(swap_lbl, lv_color_hex(0xFFFFFF), 0);

    // Rate line.
    _fx_rate_lbl = lv_label_create(_fx_page);
    lv_label_set_text(_fx_rate_lbl, "");
    lv_obj_set_pos(_fx_rate_lbl, CARD_X + 6, 480);
    lv_obj_set_style_text_color(_fx_rate_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_fx_rate_lbl, zh_font_30(), 0);

    // ── Keypad (right) ───────────────────────────────────────────────────────
    const int KX = 700, KW = 540, GAP = 16, COLS = 3, ROWS = 4;

    // AC bar above the keypad.
    lv_obj_t* ac = lv_obj_create(_fx_page);
    lv_obj_set_size(ac, KW, 60);
    lv_obj_set_pos(ac, KX, 70);
    lv_obj_set_style_bg_color(ac, lv_color_hex(CALC_FUNC), 0);
    lv_obj_set_style_radius(ac, 16, 0);
    lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_bg_color(ac, lv_color_hex(0x6E6E70), LV_STATE_PRESSED);
    lv_obj_clear_flag(ac, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ac, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ac, (void*)"AC");
    lv_obj_add_event_cb(ac, _fxKey_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* ac_lbl = lv_label_create(ac);
    lv_label_set_text(ac_lbl, "AC");
    lv_obj_center(ac_lbl);
    lv_obj_set_style_text_color(ac_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ac_lbl, &font_calc_op, 0);

    struct FKey { const char* label; const char* code; bool sym; };
    static const FKey FKEYS[4][3] = {
        {{"7","7",false},{"8","8",false},{"9","9",false}},
        {{"4","4",false},{"5","5",false},{"6","6",false}},
        {{"1","1",false},{"2","2",false},{"3","3",false}},
        {{".",".",false},{"0","0",false},{LV_SYMBOL_BACKSPACE,"DEL",true}},
    };
    const int ktop = 150;
    const int bw = (KW - (COLS - 1) * GAP) / COLS;
    const int bh = (720 - ktop - 30 - (ROWS - 1) * GAP) / ROWS;
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            const FKey& k = FKEYS[r][col];
            lv_obj_t* b = lv_obj_create(_fx_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, KX + col * (bw + GAP), ktop + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(CALC_NUM), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5C), LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _fxKey_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    // Defaults: 1 USD → CNY.
    _fx_entry = "1";
    lv_dropdown_set_selected(_fx_from_dd, 0);
    lv_dropdown_set_selected(_fx_to_dd, 1);
    _fxRecompute();

    // Pull live rates from the HA server in the background (falls back to the
    // static table if the server is unreachable).
    _fxFetch();
}

void AppSettings::_fxFetch()
{
    if (g_fx_fetching.exchange(true)) return;  // already in flight

    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.142");
    std::string url  = "http://" + host + ":8766/rate";

    bool spawned = GetHAL()->tryRunDetached([url]() {
        auto resp = GetHAL()->httpGet(url);
        if (resp.ok) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                if (j.value("ok", false) && j.contains("rates")) {
                    const auto& rates = j["rates"];
                    std::lock_guard<std::mutex> lk(g_fx_mutex);
                    for (int i = 0; i < FX_N; i++) {
                        auto it = rates.find(FX[i].code);
                        if (it != rates.end() && it->is_number()) {
                            double v = it->get<double>();
                            if (v > 0) g_fx_rate[i] = v;
                        }
                    }
                    g_fx_source = j.value("source", std::string("?"));
                    g_fx_updated.store(true);  // UI loop will recompute
                    mclog::tagInfo(_tag, "fx rates updated ({})", g_fx_source.c_str());
                }
            } catch (...) {
                mclog::tagWarn(_tag, "fx parse failed");
            }
        } else {
            mclog::tagWarn(_tag, "fx fetch failed (keeping static rates)");
        }
        g_fx_fetching.store(false);
    });
    if (!spawned) g_fx_fetching.store(false);
}

void AppSettings::_fxBack_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeFx();
}

void AppSettings::_closeFx()
{
    if (_fx_page) {
        lv_obj_delete(_fx_page);
        _fx_page = nullptr;
        _fx_from_dd = _fx_to_dd = _fx_amt_lbl = _fx_res_lbl = _fx_rate_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

void AppSettings::_fxRecompute()
{
    if (!_fx_from_dd || !_fx_to_dd) return;
    int fi = lv_dropdown_get_selected(_fx_from_dd);
    int ti = lv_dropdown_get_selected(_fx_to_dd);
    if (fi < 0 || fi >= FX_N) fi = 0;
    if (ti < 0 || ti >= FX_N) ti = 0;

    double rf, rt;
    {
        std::lock_guard<std::mutex> lk(g_fx_mutex);
        rf = g_fx_rate[fi];
        rt = g_fx_rate[ti];
    }
    if (rf <= 0) rf = 1;
    double amt = atof(_fx_entry.c_str());
    double res = amt / rf * rt;

    if (_fx_amt_lbl) lv_label_set_text(_fx_amt_lbl, group(_fx_entry).c_str());
    if (_fx_res_lbl) lv_label_set_text(_fx_res_lbl, fxFmt(res).c_str());
    if (_fx_rate_lbl) {
        double one = rt / rf;  // 1 FROM = one TO
        char buf[96];
        snprintf(buf, sizeof(buf), "1 %s = %s %s", FX[fi].cn, fxFmt(one).c_str(), FX[ti].cn);
        lv_label_set_text(_fx_rate_lbl, buf);
    }
}

void AppSettings::_fxInput(const char* key)
{
    std::string k = key;
    if (k == "AC") {
        _fx_entry = "0";
    } else if (k == "DEL") {
        if (!_fx_entry.empty()) _fx_entry.pop_back();
        if (_fx_entry.empty()) _fx_entry = "0";
    } else if (k == ".") {
        if (_fx_entry.find('.') == std::string::npos) _fx_entry += ".";
    } else {  // digit
        if (_fx_entry == "0") _fx_entry = k;
        else if (_fx_entry.size() < 12) _fx_entry += k;
    }
    _fxRecompute();
}

void AppSettings::_fxKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_fxInput(code);
}

void AppSettings::_fxDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_fxRecompute();
}

void AppSettings::_fxSwap_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self->_fx_from_dd || !self->_fx_to_dd) return;
    int fi = lv_dropdown_get_selected(self->_fx_from_dd);
    int ti = lv_dropdown_get_selected(self->_fx_to_dd);
    lv_dropdown_set_selected(self->_fx_from_dd, ti);
    lv_dropdown_set_selected(self->_fx_to_dd, fi);
    self->_fxRecompute();
}

// ════════════════════════════════════════════════════════════════════════════
//  Unit converter (单位换算)
// ════════════════════════════════════════════════════════════════════════════

struct UnitCat {
    const char* name;
    struct Unit {
        const char* code;       // abbreviation shown in dropdown
        const char* label;      // full name for the equation line
        double to_base;         // multiply to get base-unit value
    };
    const Unit* units;
    int n_units;
};

// Conversion factors relative to the base unit of each category.
// Temperature is handled separately (not a linear factor).

static const UnitCat::Unit UNITS_LENGTH[] = {
    {"mm",  "毫米",  0.001},
    {"cm",  "厘米",  0.01},
    {"m",   "米",    1.0},
    {"km",  "千米",  1000.0},
    {"in",  "英寸",  0.0254},
    {"ft",  "英尺",  0.3048},
    {"yd",  "码",    0.9144},
    {"mi",  "英里",  1609.344},
};
static const UnitCat::Unit UNITS_WEIGHT[] = {
    {"mg",  "毫克",  0.001},
    {"g",   "克",    1.0},
    {"kg",  "千克",  1000.0},
    {"t",   "吨",    1e6},
    {"oz",  "盎司",  28.3495},
    {"lb",  "磅",    453.592},
};
static const UnitCat::Unit UNITS_TEMP[] = {
    {"°C",  "摄氏度", 0},
    {"°F",  "华氏度", 0},
    {"K",   "开尔文", 0},
};
static const UnitCat::Unit UNITS_AREA[] = {
    {"mm²", "平方毫米", 1e-6},
    {"cm²", "平方厘米", 0.0001},
    {"m²",  "平方米",   1.0},
    {"km²", "平方公里",  1e6},
    {"ha",  "公顷",     10000.0},
    {"亩",  "亩",       666.6667},
    {"acre","英亩",     4046.86},
    {"ft²", "平方英尺", 0.092903},
};
static const UnitCat::Unit UNITS_VOLUME[] = {
    {"mL",  "毫升",   0.001},
    {"L",   "升",     1.0},
    {"m³",  "立方米", 1000.0},
    {"gal", "加仑",   3.78541},
    {"cup", "杯",     0.236588},
};
static const UnitCat::Unit UNITS_SPEED[] = {
    {"m/s",  "米/秒",    1.0},
    {"km/h", "千米/时",  0.277778},
    {"mph",  "英里/时",  0.44704},
    {"kn",   "节",       0.514444},
};
static const UnitCat::Unit UNITS_DATA[] = {
    {"B",   "字节", 1.0},
    {"KB",  "千字节", 1024.0},
    {"MB",  "兆字节", 1048576.0},
    {"GB",  "吉字节", 1073741824.0},
    {"TB",  "太字节", 1099511627776.0},
};

static const UnitCat UNIT_CATS[] = {
    {"长度", UNITS_LENGTH, 8},
    {"重量", UNITS_WEIGHT, 6},
    {"温度", UNITS_TEMP,   3},
    {"面积", UNITS_AREA,   8},
    {"体积", UNITS_VOLUME, 5},
    {"速度", UNITS_SPEED,  4},
    {"数据", UNITS_DATA,   5},
};
static constexpr int UNIT_CAT_N = sizeof(UNIT_CATS) / sizeof(UNIT_CATS[0]);

static std::string unitFmt(double v)
{
    if (fabs(v) < 1e-12) return "0";
    char buf[32];
    if (fabs(v) >= 1e9)
        snprintf(buf, sizeof(buf), "%.8g", v);
    else if (fabs(v) < 0.001 && v != 0)
        snprintf(buf, sizeof(buf), "%.8g", v);
    else
        snprintf(buf, sizeof(buf), "%.3f", v);
    // Trim trailing zeros after decimal (but keep at least one decimal)
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.size() > 2 && s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

void AppSettings::_openUnit()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _unit_page = lv_obj_create(_scr);
    lv_obj_set_size(_unit_page, 1280, 720);
    lv_obj_set_pos(_unit_page, 0, 0);
    lv_obj_set_style_bg_color(_unit_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_unit_page, 0, 0);
    lv_obj_set_style_radius(_unit_page, 0, 0);
    lv_obj_set_style_pad_all(_unit_page, 0, 0);
    lv_obj_clear_flag(_unit_page, LV_OBJ_FLAG_SCROLLABLE);

    // Title.
    lv_obj_t* title = lv_label_create(_unit_page);
    lv_label_set_text(title, "单位换算");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, zh_font_30(), 0);

    // Category dropdown — left-aligned so it doesn't overlap the AC bar.
    std::string cat_opts;
    for (int i = 0; i < UNIT_CAT_N; i++) {
        if (i) cat_opts += "\n";
        cat_opts += UNIT_CATS[i].name;
    }
    _unit_cat_dd = lv_dropdown_create(_unit_page);
    lv_obj_set_size(_unit_cat_dd, 420, 60);
    lv_obj_set_pos(_unit_cat_dd, 40, 76);
    lv_dropdown_set_options(_unit_cat_dd, cat_opts.c_str());
    lv_obj_set_style_text_font(_unit_cat_dd, zh_font_30(), 0);
    lv_dropdown_set_selected(_unit_cat_dd, 0);
    _unit_cat_idx = 0;
    // The dropdown list is a separate object — must set its font too.
    lv_obj_t* cat_list = lv_dropdown_get_list(_unit_cat_dd);
    lv_obj_set_style_text_font(cat_list, zh_font_30(), 0);
    lv_obj_add_event_cb(_unit_cat_dd, _unitCatDd_cb, LV_EVENT_VALUE_CHANGED, this);

    // ── Conversion panel (left) ──────────────────────────────────────────────
    const int CARD_X = 40, CARD_W = 580, CARD_H = 120;
    const int KEY_X = 660, KEY_W = 580;

    _unit_from_dd = nullptr;
    _unit_to_dd   = nullptr;
    _unit_amt_lbl = nullptr;
    _unit_res_lbl = nullptr;

    auto make_card = [&](int y, bool is_from) -> lv_obj_t* {
        lv_obj_t* c = lv_obj_create(_unit_page);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, CARD_X, y);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_shadow_width(c, 18, 0);
        lv_obj_set_style_shadow_color(c, lv_color_hex(0x5A7A9C), 0);
        lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dd = lv_dropdown_create(c);
        lv_obj_set_size(dd, 140, 60);
        lv_obj_align(dd, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_30, 0);
        // Dropdown list needs its own font (otherwise defaults to montserrat).
        lv_obj_t* dd_list = lv_dropdown_get_list(dd);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_30, 0);

        lv_obj_t* val = lv_label_create(c);
        lv_label_set_text(val, "0");
        lv_obj_set_style_text_font(val, &font_calc_op, 0);
        lv_obj_set_style_text_color(val,
            lv_color_hex(is_from ? CALC_TXT : C_ACCENT), 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0);

        if (is_from) {
            _unit_from_dd = dd;
            _unit_amt_lbl = val;
            lv_obj_add_event_cb(dd, _unitFromDd_cb, LV_EVENT_VALUE_CHANGED, this);
        } else {
            _unit_to_dd = dd;
            _unit_res_lbl = val;
            lv_obj_add_event_cb(dd, _unitToDd_cb, LV_EVENT_VALUE_CHANGED, this);
        }
        return c;
    };

    make_card(180, true);
    make_card(400, false);

    // Swap button. FROM card bottom = 180+120=300, TO card top = 400.
    // Gap centre = (300+400)/2 = 350.  64 px button centred at y=318.
    lv_obj_t* swap = lv_obj_create(_unit_page);
    lv_obj_set_size(swap, 64, 64);
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 32, 318);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _unitSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_center(swap_lbl);
    lv_obj_set_style_text_color(swap_lbl, lv_color_hex(0xFFFFFF), 0);

    // Equivalence line.
    _unit_eq_lbl = lv_label_create(_unit_page);
    lv_label_set_text(_unit_eq_lbl, "");
    lv_obj_set_pos(_unit_eq_lbl, CARD_X + 6, 555);
    lv_obj_set_style_text_color(_unit_eq_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_unit_eq_lbl, zh_font_30(), 0);

    // ── Keypad (right) ───────────────────────────────────────────────────────
    const int GAP = 16, COLS = 3, ROWS = 4;

    lv_obj_t* ac = lv_obj_create(_unit_page);
    lv_obj_set_size(ac, KEY_W, 60);
    lv_obj_set_pos(ac, KEY_X, 76);
    lv_obj_set_style_bg_color(ac, lv_color_hex(CALC_FUNC), 0);
    lv_obj_set_style_radius(ac, 16, 0);
    lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_bg_color(ac, lv_color_hex(0x6E6E70), LV_STATE_PRESSED);
    lv_obj_clear_flag(ac, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ac, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ac, (void*)"AC");
    lv_obj_add_event_cb(ac, _unitKey_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* ac_lbl = lv_label_create(ac);
    lv_label_set_text(ac_lbl, "AC");
    lv_obj_center(ac_lbl);
    lv_obj_set_style_text_color(ac_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ac_lbl, &font_calc_op, 0);

    struct UKey { const char* label; const char* code; bool sym; };
    static const UKey UKEYS[4][3] = {
        {{"7","7",false},{"8","8",false},{"9","9",false}},
        {{"4","4",false},{"5","5",false},{"6","6",false}},
        {{"1","1",false},{"2","2",false},{"3","3",false}},
        {{".",".",false},{"0","0",false},{LV_SYMBOL_BACKSPACE,"DEL",true}},
    };
    const int ktop = 150;
    const int bw = (KEY_W - (COLS - 1) * GAP) / COLS;
    const int bh = (720 - ktop - 30 - (ROWS - 1) * GAP) / ROWS;
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            const UKey& k = UKEYS[r][col];
            lv_obj_t* b = lv_obj_create(_unit_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, KEY_X + col * (bw + GAP), ktop + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(CALC_NUM), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5C), LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _unitKey_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    // Populate unit dropdowns for the initial category.
    _unitPopulateUnits();
    _unit_entry = "1";
    _unitRecompute();
}

void AppSettings::_unitPopulateUnits()
{
    int ci = _unit_cat_idx;
    if (ci < 0 || ci >= UNIT_CAT_N) ci = 0;
    const UnitCat& cat = UNIT_CATS[ci];

    std::string opts;
    for (int i = 0; i < cat.n_units; i++) {
        if (i) opts += "\n";
        opts += cat.units[i].code;
    }

    if (_unit_from_dd) {
        lv_dropdown_set_options(_unit_from_dd, opts.c_str());
        lv_dropdown_set_selected(_unit_from_dd, 0);
    }
    if (_unit_to_dd) {
        lv_dropdown_set_options(_unit_to_dd, opts.c_str());
        lv_dropdown_set_selected(_unit_to_dd, cat.n_units > 1 ? 1 : 0);
    }
}

void AppSettings::_unitRecompute()
{
    if (!_unit_from_dd || !_unit_to_dd) return;
    int ci = _unit_cat_idx;
    if (ci < 0 || ci >= UNIT_CAT_N) ci = 0;
    const UnitCat& cat = UNIT_CATS[ci];

    int fi = lv_dropdown_get_selected(_unit_from_dd);
    int ti = lv_dropdown_get_selected(_unit_to_dd);
    if (fi < 0 || fi >= cat.n_units) fi = 0;
    if (ti < 0 || ti >= cat.n_units) ti = 0;

    double amt = atof(_unit_entry.c_str());

    double res;
    if (ci == 2) {
        // Temperature — special handling
        // Convert from unit fi to °C, then to unit ti.
        double celsius;
        if (fi == 0)      celsius = amt;              // °C → °C
        else if (fi == 1) celsius = (amt - 32) / 1.8; // °F → °C
        else              celsius = amt - 273.15;       // K → °C

        if (ti == 0)      res = celsius;
        else if (ti == 1) res = celsius * 1.8 + 32;
        else              res = celsius + 273.15;
    } else {
        // Linear conversion: value × to_base / from_to_base
        double f_factor = cat.units[fi].to_base;
        double t_factor = cat.units[ti].to_base;
        if (f_factor <= 0) f_factor = 1;
        res = amt * f_factor / t_factor;
    }

    if (_unit_amt_lbl) lv_label_set_text(_unit_amt_lbl, group(_unit_entry).c_str());
    if (_unit_res_lbl) lv_label_set_text(_unit_res_lbl, unitFmt(res).c_str());
    if (_unit_eq_lbl) {
        if (ci == 2) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s = %s %s",
                group(_unit_entry).c_str(), cat.units[fi].label,
                unitFmt(res).c_str(), cat.units[ti].label);
            lv_label_set_text(_unit_eq_lbl, buf);
        } else {
            double f_factor = cat.units[fi].to_base;
            double t_factor = cat.units[ti].to_base;
            if (f_factor <= 0) f_factor = 1;
            double one = f_factor / t_factor;
            char buf[96];
            snprintf(buf, sizeof(buf), "1 %s = %s %s",
                cat.units[fi].label, unitFmt(one).c_str(), cat.units[ti].label);
            lv_label_set_text(_unit_eq_lbl, buf);
        }
    }
}

void AppSettings::_unitInput(const char* key)
{
    std::string k = key;
    if (k == "AC") {
        _unit_entry = "0";
    } else if (k == "DEL") {
        if (!_unit_entry.empty()) _unit_entry.pop_back();
        if (_unit_entry.empty()) _unit_entry = "0";
    } else if (k == ".") {
        if (_unit_entry.find('.') == std::string::npos) _unit_entry += ".";
    } else {  // digit
        if (_unit_entry == "0") _unit_entry = k;
        else if (_unit_entry.size() < 14) _unit_entry += k;
    }
    _unitRecompute();
}

void AppSettings::_closeUnit()
{
    if (_unit_page) {
        lv_obj_delete(_unit_page);
        _unit_page = nullptr;
        _unit_cat_dd = _unit_from_dd = _unit_to_dd = nullptr;
        _unit_amt_lbl = _unit_res_lbl = _unit_eq_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

// ── Static callbacks ───────────────────────────────────────────────────────

void AppSettings::_unitKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_unitInput(code);
}

void AppSettings::_unitCatDd_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    self->_unit_cat_idx = lv_dropdown_get_selected(self->_unit_cat_dd);
    self->_unitPopulateUnits();
    self->_unit_entry = "1";
    self->_unitRecompute();
}

void AppSettings::_unitFromDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_unitRecompute();
}

void AppSettings::_unitToDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_unitRecompute();
}

void AppSettings::_unitSwap_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self->_unit_from_dd || !self->_unit_to_dd) return;
    int fi = lv_dropdown_get_selected(self->_unit_from_dd);
    int ti = lv_dropdown_get_selected(self->_unit_to_dd);
    lv_dropdown_set_selected(self->_unit_from_dd, ti);
    lv_dropdown_set_selected(self->_unit_to_dd, fi);
    self->_unitRecompute();
}

void AppSettings::_unitBack_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeUnit();
}

// ════════════════════════════════════════════════════════════════════════════
//  Email (邮件) — pulls unread emails from hermes :8766/unread_emails
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openEmail()
{
    mclog::tagInfo(_tag, "_openEmail: entered");
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _email_page = lv_obj_create(_scr);
    lv_obj_set_size(_email_page, 1280, 720);
    lv_obj_set_pos(_email_page, 0, 0);
    lv_obj_set_style_bg_color(_email_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_email_page, 0, 0);
    lv_obj_set_style_radius(_email_page, 0, 0);
    lv_obj_set_style_pad_all(_email_page, 0, 0);
    lv_obj_clear_flag(_email_page, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部 title — "邮件" 或 "邮件 (N 封未读)"
    _email_title = lv_label_create(_email_page);
    lv_label_set_text(_email_title, "邮件");
    lv_obj_set_style_text_color(_email_title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(_email_title, zh_font_30(), 0);
    lv_obj_align(_email_title, LV_ALIGN_TOP_MID, 0, 28);

    // status line — 加载中 / 拉取失败 / 空 提示
    _email_status = lv_label_create(_email_page);
    lv_label_set_text(_email_status, "加载中…");
    lv_obj_set_style_text_color(_email_status, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_email_status, zh_font_30(), 0);
    lv_obj_align(_email_status, LV_ALIGN_TOP_MID, 0, 76);

    // 刷新按钮 (右上角) — 点击重新拉取, _emailFetch 内部有 in-flight 保护
    lv_obj_t* refresh = lv_btn_create(_email_page);
    lv_obj_set_size(refresh, 100, 50);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, -30, 30);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(refresh, 12, 0);
    lv_obj_set_style_border_width(refresh, 0, 0);
    lv_obj_add_event_cb(refresh, _emailRefresh_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* refresh_lbl = lv_label_create(refresh);
    lv_label_set_text(refresh_lbl, "刷新");
    lv_obj_set_style_text_font(refresh_lbl, zh_font_30(), 0);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(refresh_lbl);

    // 列表区 (手画 lv_obj 容器, 不用 lv_list — lv_list 内部 scroll/clip 渲染会
    // 触发 1200x16 RGBA8888 layer buffer (76.8KB) 反复分配, LVGL 死循环, 卡 LVGL lock)
    _email_list = lv_obj_create(_email_page);
    lv_obj_set_size(_email_list, 1200, 540);
    lv_obj_align(_email_list, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(_email_list, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(_email_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_email_list, 0, 0);
    lv_obj_set_style_radius(_email_list, 16, 0);
    lv_obj_set_style_pad_all(_email_list, 12, 0);
    lv_obj_set_scroll_dir(_email_list, LV_DIR_VER);
    lv_obj_set_flex_flow(_email_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(_email_list, 8, 0);

    mclog::tagInfo(_tag, "_openEmail: dispatching _emailFetch");
    _emailFetch();
    mclog::tagInfo(_tag, "_openEmail: done");
}

void AppSettings::_closeEmail()
{
    if (_email_page) {
        lv_obj_delete(_email_page);
        _email_page = _email_title = _email_status = _email_list = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

void AppSettings::_emailFetch()
{
    if (g_email_fetching.exchange(true)) {
        mclog::tagWarn(_tag, "emailFetch: already in flight, skip");
        return;
    }
    mclog::tagInfo(_tag, "emailFetch: entered");

    // email-status-server.py 跑在 8768 端口, 直接读 /tmp/email-status.json
    // 缓存 (由 cron email-led.py 周期更新), <10ms 返回, 不需要 IMAP 同步.
    // 跟桌面 Übersicht unread-emails.widget 是同一个数据源, 完全同步.
    // 用 svc_host (默认 192.168.1.142) 而不是 ha_host (默认 192.168.1.133):
    // ha_host 指向 HA 主机, svc_host 才是 hermes + email-status-server 跑的机器.
    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.142");
    std::string url  = "http://" + host + ":8768/api/email/status";
    mclog::tagInfo(_tag, "emailFetch: url={}", url);

    bool spawned = GetHAL()->tryRunDetached([url]() {
        mclog::tagInfo(_tag, "emailFetch: worker started");
        auto resp = GetHAL()->httpGet(url);
        mclog::tagInfo(_tag, "emailFetch: httpGet done, ok={}, status={}, body_len={}",
                       resp.ok, resp.status, resp.body.size());
        if (resp.ok) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                std::lock_guard<std::mutex> lk(g_email_mutex);
                g_emails.clear();
                // 8768 schema: {count, folders:[{name, unread, latest_subject,
                // latest_from, latest_preview}], error, account, time}
                if (j.contains("folders") && j["folders"].is_array()) {
                    g_email_total.store(j.value("count", 0));
                    for (const auto& f : j["folders"]) {
                        if (!f.is_object()) continue;
                        // 单条数据只 latest 一封, 用 preview 字段当 subject
                        g_emails.push_back(EmailItem{
                            f.value("latest_from", std::string("?")),
                            f.value("latest_subject", std::string("(无主题)")),
                            std::string(),  // 8768 不暴露 date
                            f.value("name", std::string("?")),
                        });
                    }
                    g_email_updated.store(true);
                    mclog::tagInfo(_tag, "email list updated ({} folders, {} total)",
                                   g_emails.size(), g_email_total.load());
                } else {
                    g_email_error.store(true);
                    g_email_error_msg = j.value("error", std::string("未知错误"));
                }
            } catch (const std::exception& ex) {
                mclog::tagWarn(_tag, "email parse failed: {}", ex.what());
                g_email_error.store(true);
                g_email_error_msg = std::string("JSON 解析失败: ") + ex.what();
            }
        } else {
            // resp.status == 0 通常是 timeout / 连接失败 / DNS 失败
            mclog::tagWarn(_tag, "email fetch failed (status={}, body={})",
                           resp.status, resp.body);
            g_email_error.store(true);
            if (resp.status == 0) {
                g_email_error_msg = "网络/timeout (status=0) — 检查 8768 email-status-server";
            } else {
                g_email_error_msg = "HTTP " + std::to_string(resp.status);
            }
        }
        g_email_fetching.store(false);
    });
    if (!spawned) {
        // 之前这里只 store(false) 不设 error → UI 永远"加载中". 修.
        mclog::tagWarn(_tag, "emailFetch: tryRunDetached failed (pthread_create?)");
        g_email_error.store(true);
        g_email_error_msg = "本地线程创建失败 (内存不足?)";
        g_email_fetching.store(false);
    }
}

void AppSettings::_emailRefresh()
{
    if (!_email_list) return;
    // lv_list_clean 不存在于本项目 LVGL 版本, 用通用 lv_obj_clean 删子对象
    lv_obj_clean(_email_list);

    std::vector<EmailItem> copy;
    {
        std::lock_guard<std::mutex> lk(g_email_mutex);
        copy = g_emails;
    }

    if (copy.empty()) {
        lv_label_set_text(_email_title, "邮件");
        lv_label_set_text(_email_status, "没有未读邮件");
        return;
    }

    int total = g_email_total.load();
    char buf[64];
    snprintf(buf, sizeof(buf), "邮件 (%d 封未读)", total);
    lv_label_set_text(_email_title, buf);
    lv_label_set_text(_email_status, "");

    for (const auto& m : copy) {
        // 一行: "[folder] from — subject" (手画 row, 不用 lv_list_add_button)
        char row_text[320];
        snprintf(row_text, sizeof(row_text), "[%s] %s — %s",
                 m.folder.c_str(), m.from.c_str(), m.subject.c_str());
        lv_obj_t* row = lv_obj_create(_email_list);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD_PR), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, row_text);
        lv_obj_set_style_text_font(lbl, zh_font_30(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    }
}

void AppSettings::_emailShowError()
{
    if (!_email_status) return;
    lv_label_set_text(_email_status,
        ("拉取失败: " + g_email_error_msg).c_str());
}
