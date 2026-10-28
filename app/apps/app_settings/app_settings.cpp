#include "app_settings.h"
#include "../utils/theme/theme.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cstring>
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

// 工具页 tile 图标全部为 make_tile 图形分支现画 (线条取主题强调色, 随主题切换):
// 自选股/万年历 2026-10-27 弃用 stocks8_icon/calendar_icon, 计算器/汇率/单位换算/
// LoRa/RFID/邮件/Reachy 同日弃用 calc_icon/fx_icon/unit_icon/lora_logo/reachy_icon.
// 上述 .c 图片源文件仅存档, 不再被引用 (真机 gc-sections 自动剔除).

// 工具页图形分支共用: 画一块纯色小矩形 (图标线条/圆点), 自动冒泡点击到父 tile.
static lv_obj_t* tool_accent_rect(lv_obj_t* parent, uint32_t color,
                                  int x, int y, int w, int h, int radius)
{
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}

// 工具页图形分支共用: 画一个纯色描边圆角框 (透明底; radius=边长一半即圆环),
// 自动冒泡点击到父 tile.
static lv_obj_t* tool_accent_frame(lv_obj_t* parent, uint32_t color,
                                   int x, int y, int w, int h, int radius, int border_w)
{
    lv_obj_t* f = lv_obj_create(parent);
    lv_obj_set_size(f, w, h);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(f, radius, 0);
    lv_obj_set_style_border_width(f, border_w, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(f, 0, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(f, LV_OBJ_FLAG_EVENT_BUBBLE);
    return f;
}

// 工具页图形分支共用: 用 lv_line 画折线 (pts 相对线对象原点, 线对象放在 (x,y)),
// 自动冒泡点击到父 tile.
static lv_obj_t* tool_accent_line(lv_obj_t* parent, uint32_t color, int x, int y,
                                  const lv_point_precise_t pts[], uint32_t n, int width)
{
    lv_obj_t* line = lv_line_create(parent);
    lv_line_set_points(line, pts, n);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_EVENT_BUBBLE);
    return line;
}

// 工具页图形分支共用: 用 33 段折线近似画一段圆弧 (角度制, 0°=右, 顺时针增,
// cx/cy 为圆心在父 tile 里的绝对坐标). lv_line 的点数组必须跨帧存活 -> 存静态池;
// 图标几何固定, 页面重建时原槽位重算复用.
static lv_obj_t* tool_accent_arc(lv_obj_t* parent, uint32_t color, int cx, int cy,
                                 int radius, float a0_deg, float a1_deg, int width)
{
    static lv_point_precise_t pool[16][33];
    static int used = 0;
    lv_point_precise_t* pts = pool[used];
    used = (used + 1) % 16;
    for (int i = 0; i <= 32; i++) {
        const float a = (a0_deg + (a1_deg - a0_deg) * i / 32.0f) * 3.14159265f / 180.0f;
        pts[i].x = cx + radius * cosf(a);
        pts[i].y = cy + radius * sinf(a);
    }
    return tool_accent_line(parent, color, 0, 0, pts, 33, width);
}

// Calculator fonts (Arial Unicode subsets) defined in this app's font_calc_*.c.
extern const lv_font_t font_calc_big;   // 80px result line
extern const lv_font_t font_calc_op;    // 40px button labels (has × ÷)
extern const lv_font_t font_calc_expr;  // 30px expression line

// ── Palette ─────────────────────────────────────────────────────────────────
// 配色: 主题快照(onOpen 时从全局主题取, 见 utils/theme/theme.h);
// 计算器数字/功能键的中性灰保留(任何主题下都读作"中性")。
static uint32_t C_BG, C_ACCENT, C_CARD, C_CARD_PR, C_TEXT,
                CALC_BG, CALC_NUM, CALC_FUNC, CALC_OP, CALC_TXT, CALC_EXPR,
                C_SHADOW;

static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    C_BG = 0x000000;        // 纯黑(与主页一致)
    C_ACCENT = T.accent;
    C_CARD = T.card[1];     C_CARD_PR = T.card[0];
    C_TEXT = T.text_hi;     C_SHADOW = T.border;
    CALC_BG = 0x000000;
    CALC_NUM = 0x3A3A3C;    CALC_FUNC = 0x545456;
    CALC_OP = T.accent;     // 运算键随主题强调色
    CALC_TXT = 0xFFFFFF;    CALC_EXPR = T.text_dim;
}

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
}  // namespace

// Static-member definitions for the shared email cache (declared in
// app_settings.h). Mirrors the g_fx_* pattern above: detached fetch worker
// writes email_list under email_list_mutex and flips email_updated; the UI
// loop detects the flag in onRunning() and rebuilds the lv_list. AppHome's
// status-bar icon polls fetchEmail() every 60s and reads email_unread_total.
std::atomic<int>        AppSettings::email_unread_total{0};  // total, from j["count"]
std::atomic<bool>       AppSettings::email_updated{false};
std::atomic<bool>       AppSettings::email_fetching{false};
std::atomic<bool>       AppSettings::email_error{false};
std::string             AppSettings::email_error_msg;
std::mutex              AppSettings::email_list_mutex;
std::vector<EmailItem>  AppSettings::email_list;

AppSettings::AppSettings()
{
    setAppInfo().name = "工具";
}

void AppSettings::onCreate() {}

void AppSettings::onOpen()
{
    snap_theme();
    mclog::tagInfo(_tag, "open");
    // 防重入守卫: mooncake 的 openApp() 不会关前台 app (onClose 不触发),
    // 子 app 退出回到本页时 onOpen 会被再次调用——若无守卫, 这里 new 一个新屏
    // 并覆盖旧 _scr, 整屏对象全部泄漏; 工具↔子 app 每往返一次漏一屏,
    // 几轮后 LVGL OOM (lv_realloc failed → lv_obj_create assert), main 任务
    // 卡死 = "闪屏 + 触摸无响应" (2026-10-27 自选股实锤). 有屏直接复用.
    if (_scr) {
        lv_screen_load(_scr);
        _installSwipeGesture();
        return;
    }
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    _buildToolsPage();
    lv_screen_load(_scr);
    _installSwipeGesture();
#ifdef PLATFORM_BUILD_DESKTOP
    if (const char* e = getenv("TAB5_OPEN_CALENDAR")) {
        if (e[0] == '1') {
            // 桌面调试: 直落万年历. onOpen 内直接 openApp 会撞 mooncake
            // 状态机 (本 app 还在 Opening), 延迟到下一拍再开.
            lv_timer_create([](lv_timer_t* tm) {
                auto* self = static_cast<AppSettings*>(lv_timer_get_user_data(tm));
                if (self) self->openCalendar();
                lv_timer_del(tm);
            }, 800, this);
        }
    }
#endif
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
    if (email_updated.exchange(false) && _email_page) {
        _emailRefresh();
    }
    if (email_error.exchange(false) && _email_page) {
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
    // 不清零 email_unread_total: 本 app 只展示邮件列表, 没有"标记已读"功能,
    // 退出不代表邮件已读. 未读数是服务器真实状态 (由轮询刷新), 退出后状态栏
    // 信封图标 / 灯阵 NEW EMAIL 应继续按真实未读显示.
    email_updated.store(false);
    email_error.store(false);
    email_error_msg.clear();
    email_fetching.store(false);  // 防止重入保护卡死
    if (_close_cb) _close_cb();
}

// ─── Swipe-up gesture closes the whole app (back to home) ───────────────────
void AppSettings::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            static lv_point_t s_gpress{0, 0};   // PRESSED 起点 (底部起手判定)
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSED) {
                    lv_point_t p; lv_indev_get_point(dev, &p);
                    s_gpress = p;
                    return;
                }
                if (code != LV_EVENT_GESTURE) return;
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // 位置守卫: 只认屏幕最下缘 (y>650) 起手的上拨
                    // (前台守卫在下方 async 里, 2026-10-27 实锤 indev 广播)
                    if (s_gpress.y <= 650) return;
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppSettings*>(udata);
                        if (!app) return;
                        // Only act when our screen is actually on display. This
                        // hook stays installed while another app opened from
                        // the tools page (e.g. the YRobot cockpit) owns the
                        // foreground — its joystick drags must not close us.
                        if (!app->_scr || lv_screen_active() != app->_scr) return;
                        // Back out of the deepest open sub-page first.
                        if (app->_email_page)   app->_closeEmail();
                        else if (app->_unit_page)   app->_closeUnit();
                        else if (app->_fx_page) app->_closeFx();
                        else if (app->_calc_page) app->_closeCalculator();
                        else                     app->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_ALL, this);
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
    // 4 行 tile (10 个工具) 超出一屏: 纵向可滚 (2026-10-27 加万年历第 4 行)
    lv_obj_set_scroll_dir(_tools_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_tools_page, LV_SCROLLBAR_MODE_AUTO);

    // Tool card — same style as the HA "灯光" device cards: rounded card with a
    // soft shadow, icon on the left, name on the right. 3×3 网格铺满整屏
    // (2026-10-27 用户要求参考智能家居布局、少留白、上下对齐).
    // img==nullptr 走图形分支 (不依赖字体): kind==0 LED 点阵 (灯阵) /
    // kind==1 信封 (邮件) / kind==2 NFC 圆环 (RFID).
    constexpr int TW = 296, TH = 206, GAP_X = 16, GAP_Y = 16, MARGIN = 24;  // 4 列 × 3 行 (2026-10-27 用户指定一行 4 个)
    auto make_tile = [&](int col, int row, const lv_image_dsc_t* img, int img_w,
                         const char* label, lv_event_cb_t cb,
                         int img_scale = 256, int icon_x = 12,
                         int img_scale_y = -1,
                         int kind = 0) {
        const int x = MARGIN + col * (TW + GAP_X);
        const int y = 20 + row * (TH + GAP_Y);
        lv_obj_t* tile = lv_obj_create(_tools_page);
        lv_obj_set_size(tile, TW, TH);
        lv_obj_align(tile, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_shadow_width(tile, 18, 0);
        lv_obj_set_style_shadow_color(tile, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_opa(tile, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(tile, 4, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
        // 按下"放大"效果: 同底色描边外扩 (LVGL 按住自动置 PRESSED 态, 松开消失;
        // 与每日一句/股票卡的 outline 闪烁同观感, 2026-10-28 用户要求)
        lv_obj_set_style_outline_width(tile, 6, LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(tile, lv_color_hex(C_CARD), LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, this);

        lv_obj_t* name = lv_label_create(tile);
        lv_label_set_text(name, label);
        lv_obj_set_style_text_font(name, zh_font_30(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
        // 内容成组水平居中: 文字宽 + 图标宽 + 间距, 整组 (TW-总宽)/2 起
        const char* name_txt = lv_label_get_text(name);
        const int name_w = (int)lv_txt_get_width(name_txt, (uint32_t)strlen(name_txt),
                                                 zh_font_30(), 0);
        constexpr int ICON_GAP = 20;

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
            const int start = (TW - img_w - ICON_GAP - name_w) / 2;
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, start, 0);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, start + img_w + ICON_GAP, 0);
        } else {
            // kind 分支: 0=点阵 (灯阵) / 1=信封 (邮件) / 2=嵌套方框标签 (RFID) /
            // 3=走势 (自选股) / 4=日历 (万年历) / 5=计算器 / 6=硬币双箭头 (汇率) /
            // 7=尺子 (单位换算) / 8=电波 (LoRa) / 9=机器人 (Reachy).
            // 线条全部随主题强调色 (2026-10-27 起工具页图标全图形化, 静态图全弃用).
            if (kind == 1) {
                // 信封 (邮件 tile) — 圆角轮廓 + V 形封盖, 参考 icons8-邮件 描边款.
                const int EW = 84, EH = 56;
                const int ex = (TW - EW - ICON_GAP - name_w) / 2;
                const int ey = (TH - EH) / 2;
                tool_accent_frame(tile, C_ACCENT, ex, ey, EW, EH, 8, 6);
                // V 形封盖: 点位相对线对象原点 (线对象放在信封左上角)
                static const lv_point_precise_t flap[] = {{7, 4}, {42, 30}, {77, 4}};
                tool_accent_line(tile, C_ACCENT, ex, ey, flap, 3, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, ex + EW + ICON_GAP, 0);
            } else if (kind == 2) {
                // RFID 标签 — 三层嵌套圆角方框 + 中心芯片, 参考 icons8-rfid标签.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 8, top + 8, 80, 80, 10, 6);
                tool_accent_frame(tile, C_ACCENT, start + 22, top + 22, 52, 52, 8, 5);
                tool_accent_frame(tile, C_ACCENT, start + 34, top + 34, 28, 28, 5, 4);
                tool_accent_rect(tile, C_ACCENT, start + 43, top + 43, 10, 10, 2);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 3) {
                // 自选股 (行情走势) — L 形坐标轴 + 上升折线 + 终点圆点,
                // 线条同灯阵圆点一样取主题强调色 C_ACCENT. 图标盒 96x96.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                // 纵轴 / 横轴 (6px 粗)
                tool_accent_rect(tile, C_ACCENT, start, top, 6, 84, 3);
                tool_accent_rect(tile, C_ACCENT, start, top + 78, 84, 6, 3);
                // 上升折线 (点位相对线对象原点, 线对象放在图标盒 (12,6) 处)
                static const lv_point_precise_t trend_pts[] = {
                    {0, 52}, {15, 34}, {27, 42}, {46, 18}, {58, 26}, {74, 2}
                };
                tool_accent_line(tile, C_ACCENT, start + 12, top + 6, trend_pts, 6, 6);
                // 折线终点圆点 (终点绝对坐标 start+86, top+8)
                tool_accent_rect(tile, C_ACCENT, start + 80, top + 2, 12, 12, 6);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 4) {
                // 万年历 — 圆角日历框 + 顶部两个装订环 + 表头分隔线 + 日期圆点,
                // 线条同灯阵圆点一样取主题强调色 C_ACCENT. 图标盒 96x96.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int fx = start + 6, fy = top + 18, fw = 84, fh = 72;
                // 装订环 (先画, 下半截被日历框边框盖住, 露出上半截)
                tool_accent_rect(tile, C_ACCENT, fx + 14, top + 8, 8, 20, 4);
                tool_accent_rect(tile, C_ACCENT, fx + fw - 22, top + 8, 8, 20, 4);
                // 日历框: 圆角边框, 背景透明
                tool_accent_frame(tile, C_ACCENT, fx, fy, fw, fh, 14, 6);
                // 表头分隔线
                tool_accent_rect(tile, C_ACCENT, fx + 6, fy + 22, fw - 12, 4, 2);
                // 日期圆点 3x2
                for (int r = 0; r < 2; r++)
                    for (int c = 0; c < 3; c++)
                        tool_accent_rect(tile, C_ACCENT,
                                         fx + 14 + c * 22, fy + 36 + r * 16, 10, 10, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 5) {
                // 计算器 — 圆角机身 + 显示屏条 + 4x3 按键阵, 参考 icons8-计算器.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int bx = start + 16, by = top + 6;   // 机身 64x84
                tool_accent_frame(tile, C_ACCENT, bx, by, 64, 84, 12, 6);
                tool_accent_rect(tile, C_ACCENT, bx + 10, by + 10, 44, 13, 3);  // 显示屏
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 4; c++)
                        tool_accent_rect(tile, C_ACCENT,
                                         bx + 10 + c * 12, by + 31 + r * 13, 8, 8, 2);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 6) {
                // 汇率 (外币兑换) — 对角两枚硬币 + 上下反向箭头, 参考 icons8-外币兑换.
                // 币面不带字: 96px 盒里 30px 的 ￥ 过挤, 表意交给 tile 文字「汇率」.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 6, top + 8, 34, 34, 17, 6);
                tool_accent_frame(tile, C_ACCENT, start + 56, top + 54, 34, 34, 17, 6);
                // 上箭头 (向左) / 下箭头 (向右)
                tool_accent_rect(tile, C_ACCENT, start + 52, top + 10, 38, 5, 2);
                tool_accent_rect(tile, C_ACCENT, start + 6, top + 81, 38, 5, 2);
                // 箭头头部 (V 形折线, 点位相对线对象原点)
                static const lv_point_precise_t head_l[] = {{6, 0}, {0, 6}, {6, 12}};
                static const lv_point_precise_t head_r[] = {{0, 0}, {6, 6}, {0, 12}};
                tool_accent_line(tile, C_ACCENT, start + 52, top + 7, head_l, 3, 5);
                tool_accent_line(tile, C_ACCENT, start + 38, top + 78, head_r, 3, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 7) {
                // 单位换算 (尺子) — 圆角尺身 + 5 根刻度, 参考 icons8-尺子 (横放).
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 4, top + 26, 88, 44, 8, 6);
                for (int i = 0; i < 5; i++)
                    tool_accent_rect(tile, C_ACCENT,
                                     start + 12 + i * 16, top + 32, 5, 16, 2);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 8) {
                // LoRa Chat (无线电波) — 中心圆环 + 左右各两道弧, 参考 icons8-无线电波.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int icx = start + IW / 2, icy = TH / 2;
                tool_accent_frame(tile, C_ACCENT, icx - 9, icy - 9, 18, 18, 9, 6);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 30, 145, 215, 5);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 44, 150, 210, 5);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 30, -35, 35, 5);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 44, -30, 30, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 9) {
                // Reachy (机器人头) — 圆角头框 + 双耳 + 双天线 + 双眼 + 嘴,
                // 参考 icons8-机器人.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int icx = start + IW / 2, icy = TH / 2;
                tool_accent_frame(tile, C_ACCENT, icx - 30, icy - 24, 60, 48, 12, 6);
                tool_accent_rect(tile, C_ACCENT, icx - 38, icy - 7, 8, 14, 3);   // 左耳
                tool_accent_rect(tile, C_ACCENT, icx + 30, icy - 7, 8, 14, 3);   // 右耳
                tool_accent_rect(tile, C_ACCENT, icx - 16, icy - 38, 4, 14, 2);  // 左天线
                tool_accent_rect(tile, C_ACCENT, icx + 12, icy - 38, 4, 14, 2);  // 右天线
                tool_accent_rect(tile, C_ACCENT, icx - 18, icy - 46, 8, 8, 4);   // 天线顶球
                tool_accent_rect(tile, C_ACCENT, icx + 10, icy - 46, 8, 8, 4);
                tool_accent_rect(tile, C_ACCENT, icx - 18, icy - 12, 8, 8, 2);   // 双眼
                tool_accent_rect(tile, C_ACCENT, icx + 10, icy - 12, 8, 8, 2);
                tool_accent_rect(tile, C_ACCENT, icx - 8, icy + 4, 16, 5, 2);    // 嘴
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else {
                // 3x3 实心圆点网格, 象征 8x8 LED 矩阵缩略图.
                // 不用 LV_SYMBOL_BULLET (依赖字体覆盖, 16px montserrat 渲染可能出方框),
                // 直接画 9 个圆角矩形, 完全脱离字体. 9 个圆点既省资源也清晰.
                const int CELL = 24, GAP = 6, COLS = 3, ROWS = 3;
                const int GRID_W = COLS * CELL + (COLS - 1) * GAP;
                const int start_x = (TW - GRID_W - ICON_GAP - name_w) / 2;
                const int start_y = (TH - ROWS * CELL - (ROWS - 1) * GAP) / 2;
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
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start_x + GRID_W + ICON_GAP, 0);
            }
        }
    };

    // 行 0: 计算器 / 汇率 / 单位换算 / 灯阵 — 图标全部走 make_tile 图形分支
    // (kind 5/6/7/0), 线条随主题强调色, 原静态图 calc_icon/fx_icon/unit_icon 已弃用.
    make_tile(0, 0, nullptr, 130, "计算器",  _toolBtn_cb, 256, 12, -1, 5);
    make_tile(1, 0, nullptr, 130, "汇率",    _toolFx_cb, 256, 12, -1, 6);
    make_tile(2, 0, nullptr, 130, "单位换算", _toolUnit_cb, 256, 12, -1, 7);
    make_tile(3, 0, nullptr, 130, "灯  阵", _toolPuzzle_cb);

    // 第 2 行第 1 列: LoRa 聊天 (Unit C6L SX1262, PORT.A UART) — kind=8 电波分支,
    // 线条随主题强调色 (原 lora_logo 静态图 + 手写 tile 块已弃用).
    make_tile(0, 1, nullptr, 130, "LoRa Chat", _toolLoraChat_cb, 256, 12, -1, 8);

    // 第 3 行: 邮件 (调 hermes :8766/unread_emails 显示未读列表), kind=1 信封分支
    make_tile(2, 1, nullptr, 130, "邮  件", _toolMail_cb, 256, 12, -1, 1);

    // (2026-10-28: 金融入口移到主页底部按钮, 工具页不再放)

    // 第 3 行第 1 列: Reachy 仪表盘 (调 Reachy :8042/api/* 等接口, 5 tab 只读)
    //   kind=9 机器人分支; 2026-10-27 用户要求由「YRobot 控制台」改名「Reachy」,
    //   原 reachy_icon 静态图弃用.
    make_tile(0, 2, nullptr, 130, "Reachy", _toolReachy_cb, 256, 12, -1, 9);
    //   kind=4 日历分支, 线条随主题强调色 (原 calendar_icon 静态图已弃用)
    //   2026-10-28: 移到 (3,1) 补金融入口移除后的空位
    make_tile(3, 1, nullptr, 130, "万年历", _toolCalendar_cb, 256, 12, -1, 4);

    // 第 2 行第 3 列: RFID 读写器 (Unit NFC / MFRC522, PORT A I2C 0x28)
    //   图标走 kind=2 分支: 3 个同心圆环 + 中心芯片, 象征 NFC 感应
    make_tile(1, 1, nullptr, 130, "RFID", _toolRfid_cb, 256, 12, -1, 2);
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

void AppSettings::_toolLoraChat_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (self->_lora_chat_id > 0) {
        mooncake::GetMooncake().openApp(self->_lora_chat_id);
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

void AppSettings::_toolStocks_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openStocks();
}

void AppSettings::openCalendar()
{
    if (_calendar_id > 0) {
        _tools_page = nullptr;   // 子 app 打开后本页随 _scr 一起由 onClose 清
        mooncake::GetMooncake().openApp(_calendar_id);
    }
}

void AppSettings::_toolCalendar_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openCalendar();
}

void AppSettings::openStocks()
{
    if (_stocks_id > 0) {
        mooncake::GetMooncake().openApp(_stocks_id);
    }
}

void AppSettings::_toolReachy_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openReachy();
}

void AppSettings::openReachy()
{
    if (_reachy_id > 0) {
        mooncake::GetMooncake().openApp(_reachy_id);
    }
}

void AppSettings::_toolRfid_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (self->_rfid_id > 0) {
        mooncake::GetMooncake().openApp(self->_rfid_id);
    }
}

void AppSettings::_emailRefresh_cb(lv_event_t* e)
{
    // 邮件子页 "刷新" 按钮 → 重新拉取. _emailFetch 内部 exchange(true) 跳过 in-flight.
    auto* app = static_cast<AppSettings*>(lv_event_get_user_data(e));
    mclog::tagInfo(_tag, "email refresh button clicked");
    app->fetchEmail();  // (also static; the explicit instance call keeps the
                       //  refresh-button event handler readable)
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
                lv_color_hex(k.color == CALC_OP ? C_TEXT
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
        lv_obj_set_style_shadow_color(c, lv_color_hex(C_SHADOW), 0);
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
    lv_obj_set_size(swap, 88, 88);   // 64→88: 圆和图标都放大 (2026-10-28 用户)
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 44, 290 - 44);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _fxSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(swap_lbl, &lv_font_montserrat_38, 0);
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

    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.200");   // 默认与主页一致 (模拟器无 NVS 配置)
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
        lv_obj_set_style_shadow_color(c, lv_color_hex(C_SHADOW), 0);
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
    lv_obj_set_size(swap, 88, 88);
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 44, 306);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _unitSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(swap_lbl, &lv_font_montserrat_38, 0);
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
void AppSettings::openEmailPage()
{
    // Public entry used by the home status-bar mail icon. Same as the user
    // tapping the "邮件" tool tile — build the sub-page and kick off a fetch.
    // No-op if the app hasn't been opened yet (no _scr).
    if (!_scr) return;
    _openEmail();
}

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

    // 已有缓存列表先渲染 (状态条轮询成功过的数据直接显示, 不空等)
    if (!email_list.empty() && _email_list) _emailRefresh();
    // 清残留错误 (旧失败若在子页未开时发生, 打开时不该显示旧错误)
    email_error.store(false);
    email_error_msg.clear();

    mclog::tagInfo(_tag, "_openEmail: dispatching fetchEmail");
    fetchEmail();
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

void AppSettings::fetchEmail()
{
    if (email_fetching.exchange(true)) {
        mclog::tagWarn(_tag, "fetchEmail: already in flight, skip");
        return;
    }
    mclog::tagInfo(_tag, "fetchEmail: entered");

    // email-status-server.py 跑在 8768 端口, 直接读 /tmp/email-status.json
    // 缓存 (由 cron email-led.py 周期更新), <10ms 返回, 不需要 IMAP 同步.
    // 跟桌面 Übersicht unread-emails.widget 是同一个数据源, 完全同步.
    // 用 svc_host 而不是 ha_host:
    // ha_host 指向 HA 主机, svc_host 才是 hermes + email-status-server 跑的机器.
    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.200");   // 默认与主页一致 (模拟器无 NVS 配置)
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
                std::lock_guard<std::mutex> lk(email_list_mutex);
                email_list.clear();
                // 8768 schema: {count, folders:[{name, unread, latest_subject,
                // latest_from, latest_preview}], error, account, time}
                if (j.contains("folders") && j["folders"].is_array()) {
                    // 草稿箱里的"未读"是自己没写完的草稿, 不算真正的未读邮件.
                    // 按 folder 名 (含 "draft" / "草稿", 大小写不敏感) 跳过, 并用
                    // 各 folder 的 unread 重新累加 total (不再直接用服务器 count).
                    auto is_draft = [](std::string n) {
                        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
                        return n.find("draft") != std::string::npos ||
                               n.find("草稿") != std::string::npos;
                    };
                    int total = 0;
                    for (const auto& f : j["folders"]) {
                        if (!f.is_object()) continue;
                        std::string name = f.value("name", std::string("?"));
                        if (is_draft(name)) continue;  // 跳过草稿箱
                        total += f.value("unread", 0);
                        // 单条数据只 latest 一封, 用 preview 字段当 subject
                        email_list.push_back(EmailItem{
                            f.value("latest_from", std::string("?")),
                            f.value("latest_subject", std::string("(无主题)")),
                            std::string(),  // 8768 不暴露 date
                            name,
                        });
                    }
                    email_unread_total.store(total);
                    email_updated.store(true);
                    mclog::tagInfo(_tag, "email list updated ({} folders, {} total, drafts excluded)",
                                   email_list.size(), email_unread_total.load());
                } else {
                    email_error.store(true);
                    email_error_msg = j.value("error", std::string("未知错误"));
                }
            } catch (const std::exception& ex) {
                mclog::tagWarn(_tag, "email parse failed: {}", ex.what());
                email_error.store(true);
                email_error_msg = std::string("JSON 解析失败: ") + ex.what();
            }
        } else {
            // resp.status == 0 通常是 timeout / 连接失败 / DNS 失败
            mclog::tagWarn(_tag, "email fetch failed (status={}, body={})",
                           resp.status, resp.body);
            email_error.store(true);
            if (resp.status == 0) {
                email_error_msg = "网络/timeout (status=0) — 检查 8768 email-status-server";
            } else {
                email_error_msg = "HTTP " + std::to_string(resp.status);
            }
        }
        email_fetching.store(false);
    });
    if (!spawned) {
        mclog::tagWarn(_tag, "emailFetch: tryRunDetached failed (pthread_create?)");
        email_error.store(true);
        email_error_msg = "本地线程创建失败 (内存不足?)";
        // Do NOT reset email_fetching here — that would allow the next
        // onRunning frame (~16 ms later) to call fetchEmail() again, creating
        // a tight retry loop that burns CPU and prevents the idle task from
        // ever freeing memory. Keep the flag set; the worker (if ever spawned)
        // will clear it on completion, and the 60 s poll cycle will naturally
        // retry later.
    }
}

void AppSettings::_emailRefresh()
{
    if (!_email_list) return;
    // lv_list_clean 不存在于本项目 LVGL 版本, 用通用 lv_obj_clean 删子对象
    lv_obj_clean(_email_list);

    std::vector<EmailItem> copy;
    {
        std::lock_guard<std::mutex> lk(email_list_mutex);
        copy = email_list;
    }

    if (copy.empty()) {
        lv_label_set_text(_email_title, "邮件");
        lv_label_set_text(_email_status, "没有未读邮件");
        return;
    }

    int total = email_unread_total.load();
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
    mclog::tagWarn(_tag, "email page: 拉取失败 ({})", email_error_msg);
    lv_label_set_text(_email_status,
        ("拉取失败: " + email_error_msg).c_str());
}
