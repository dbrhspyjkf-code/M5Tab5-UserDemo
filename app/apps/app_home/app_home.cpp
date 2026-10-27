#include "app_home.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.h>
#include <hal/hal.h>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <xiaozhi_ctl.h>
#include "../app_voice_input/app_voice_input.h"
#include "../app_ha/ha_weather.h"
#include "../app_settings/app_settings.h"
#include "../utils/datetime/lunar_calendar.h"
#include "../utils/theme/theme.h"
#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"
#endif

static const std::string _tag = "app-home";

// Full-coverage Chinese font (same as the Claude/HA apps) — GB2312 common
// ~3500 glyphs, so dialog labels never render as tofu boxes. Replaces the old
// subset font_zh_36 (~135 chars, which boxed e.g. 其/他). On device it's a cbin
// blob used in-place from flash; on the desktop sim we fall back to the linked
// 20px C-array font.
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
// 20px 小字（OPi 详情卡的模块文字）— 放在 res 分区 (pack_assets.py)
static const lv_font_t* zh_font_sm()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
// 24px 中间号（仪表卡正文, 介于 20 与 30 之间）— 同在 res 分区
static const lv_font_t* zh_font_md()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
#endif

AppHome::AppHome()
{
    setAppInfo().name = "AppHome";
}

void AppHome::addApp(const std::string& name, int app_id)
{
    _apps.push_back({name, app_id});
}

void AppHome::restoreScreen()
{
    if (_scr) {
        lv_screen_load(_scr);
    }
    _child_app_id = -1;
}

void AppHome::onCreate()
{
    open();  // home auto-opens immediately after install
}

void AppHome::onOpen()
{
    mclog::tagInfo(_tag, "on open");

    // 防重入守卫 (同其他 app): 若 mooncake 在未 onClose 的情况下再次 open,
    // 直接复用现有屏, 防整屏泄漏.
    if (_scr) {
        lv_screen_load(_scr);
        return;
    }

    // Create a dedicated screen so other apps can own their own screens
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);   // 纯黑(2026-10-26 用户指定, 弃背景图)
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Stable storage for button user_data pointers — must not reallocate after reserve
    // 长按阈值 500ms (每日一句卡长按换句, 与股票卡片一致; 全局 indev 设置)
    for (lv_indev_t* inv = lv_indev_get_next(nullptr); inv;
         inv = lv_indev_get_next(inv))
        if (lv_indev_get_type(inv) == LV_INDEV_TYPE_POINTER)
            lv_indev_set_long_press_time(inv, 500);

    _btn_data.clear();
    _btn_data.reserve(_apps.size());

    // Buttons laid out in a single row along the bottom of the screen.
    const int n = (int)_apps.size();
    static constexpr int BTN_H      = 76;
    static constexpr int BTN_GAP    = 30;
    static constexpr int SIDE_PAD   = 12;   // = 卡间距 (2026-10-27 用户: 四边只留行距)
    static constexpr int BOTTOM_PAD = 12;

    int avail_w = 1280 - 2 * SIDE_PAD - (n - 1) * BTN_GAP;
    int btn_w   = n > 0 ? avail_w / n : avail_w;
    int total_w = n * btn_w + (n - 1) * BTN_GAP;
    int start_x = (1280 - total_w) / 2;
    int y       = 720 - BTN_H - BOTTOM_PAD;

    for (int i = 0; i < n; i++) {
        _btn_data.push_back({this, _apps[i].app_id});
        BtnData& bd = _btn_data.back();

        lv_obj_t* btn = lv_obj_create(_scr);
        lv_obj_set_size(btn, btn_w, BTN_H);
        lv_obj_set_pos(btn, start_x + i * (btn_w + BTN_GAP), y);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::cur().card[3]), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 20, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(theme::cur().border), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, _apps[i].name.c_str());
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);  // 30px
        // Subtle dark text shadow for legibility over the bright gradient.
        lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);

        lv_obj_add_event_cb(btn, _btn_event_cb, LV_EVENT_CLICKED, &bd);
    }

    // ── Hero card (deep pine green, video-style) — replaces the old top status bar ──
    // Clock / date / lunar / weather lines in light ink on flat deep green
    // (#204234, 2026-10-26 用户多轮收敛: 比右侧列表卡略亮、整体更暗), tiny
    // caption on top, wifi/mail/gear/battery icons in a row at the bottom.
    // This card is the single accent block of the whole home screen.
    lv_obj_t* hero = lv_obj_create(_scr);
    lv_obj_set_pos(hero, 12, 12);
    lv_obj_set_size(hero, 400, 608);  // 12+608+12(到按钮)+76(按钮)+12(底)=720
    lv_obj_set_style_bg_color(hero, lv_color_hex(theme::cur().hero), 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hero, 26, 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 0, 0);
    lv_obj_clear_flag(hero, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t* hero_tag = lv_label_create(hero);
    lv_label_set_text(hero_tag, "M5TAB5 驾驶舱");
    lv_obj_set_style_text_font(hero_tag, zh_font_sm(), 0);
    lv_obj_set_style_text_color(hero_tag, lv_color_hex(theme::cur().text_dim), 0);
    lv_obj_set_pos(hero_tag, 24, 18);

    _status_time = lv_label_create(hero);
    lv_obj_set_style_text_font(_status_time, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_status_time, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_time, "--:--");
    lv_obj_align(_status_time, LV_ALIGN_TOP_MID, 0, 104);

    _status_date = lv_label_create(hero);
    lv_obj_set_style_text_font(_status_date, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_status_date, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_date, "");
    lv_obj_align(_status_date, LV_ALIGN_TOP_MID, 0, 180);

    _status_lunar = lv_label_create(hero);
    lv_obj_set_style_text_font(_status_lunar, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_lunar, lv_color_hex(theme::cur().text_soft), 0);
    lv_label_set_text(_status_lunar, "");
    lv_obj_align(_status_lunar, LV_ALIGN_TOP_MID, 0, 230);

    // Weather row (tap → detailed weather dialog): temperature (latin font,
    // has °) + condition (zh font), centered in dark ink.
    lv_obj_t* wbox = lv_obj_create(hero);
    lv_obj_set_size(wbox, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_bg_opa(wbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wbox, 0, 0);
    lv_obj_set_style_pad_all(wbox, 0, 0);
    lv_obj_set_style_pad_column(wbox, 10, 0);
    lv_obj_set_flex_flow(wbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wbox, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(wbox, LV_ALIGN_TOP_MID, 0, 286);

    _status_wtemp = lv_label_create(wbox);
    lv_obj_set_style_text_font(_status_wtemp, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(_status_wtemp, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_wtemp, "");

    _status_wcond = lv_label_create(wbox);
    lv_obj_set_style_text_font(_status_wcond, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_status_wcond, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_wcond, "");

    // Helper: make a hero-card item tappable. tag routes the click:
    // 1 = battery → power dialog, 2 = wifi → network dialog,
    // 3 = gear → quick settings, 4 = weather → weather detail,
    // 5 = mail → settings email sub-page.
    auto make_tappable = [&](lv_obj_t* item, int tag) {
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(item, 18);  // enlarge touch target
        lv_obj_set_user_data(item, (void*)(intptr_t)tag);
        lv_obj_add_event_cb(item, _statusClick_cb, LV_EVENT_CLICKED, this);
    };
    make_tappable(wbox, 4);

    // Thin divider + icon row along the card bottom (light ink on deep green).
    lv_obj_t* div = lv_obj_create(hero);
    lv_obj_set_pos(div, 32, 512);   // 距底 94 (随卡加高同步)
    lv_obj_set_size(div, 336, 2);
    lv_obj_set_style_bg_color(div, lv_color_hex(theme::cur().press), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_60, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_clear_flag(div, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t* irow = lv_obj_create(hero);
    lv_obj_set_pos(irow, 24, 536);
    lv_obj_set_size(irow, 352, 44);
    lv_obj_set_style_bg_opa(irow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(irow, 0, 0);
    lv_obj_set_style_pad_all(irow, 0, 0);
    lv_obj_set_flex_flow(irow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(irow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(irow, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    const uint32_t INK = theme::cur().text_hi;  // icon ink (near-white mint) on deep green

    _status_wifi = lv_label_create(irow);
    lv_obj_set_style_text_font(_status_wifi, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_wifi, lv_color_hex(INK), 0);
    lv_label_set_text(_status_wifi, LV_SYMBOL_WIFI);
    make_tappable(_status_wifi, 2);

    // Mail icon — hidden by default. Only shown when
    // AppSettings::email_unread_total > 0 (polled every 60s in onRunning).
    _status_email = lv_label_create(irow);
    lv_obj_set_style_text_font(_status_email, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_email, lv_color_hex(INK), 0);
    lv_label_set_text(_status_email, LV_SYMBOL_ENVELOPE);
    lv_obj_add_flag(_status_email, LV_OBJ_FLAG_HIDDEN);
    make_tappable(_status_email, 5);

    // Quick-settings gear (volume + brightness).
    _status_tools = lv_label_create(irow);
    lv_obj_set_style_text_font(_status_tools, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_tools, lv_color_hex(INK), 0);
    lv_label_set_text(_status_tools, LV_SYMBOL_SETTINGS);
    make_tappable(_status_tools, 3);

    _status_batt = lv_label_create(irow);
    lv_obj_set_style_text_font(_status_batt, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_status_batt, lv_color_hex(INK), 0);
    lv_label_set_text(_status_batt, LV_SYMBOL_BATTERY_FULL " --%");
    make_tappable(_status_batt, 1);

    // ── Right column: video-style info rows + OPi wide card ───────────────
    // All data comes from the hermes service on svc_host:8766 (see
    // _fetch_dashboard). Rows are flat cards with a dim index number; the
    // four card backgrounds step darker from top to bottom (渐变), the OPi
    // card is a 3x2 module grid with threshold-colored bars.
    // 右列滚动容器 (2026-10-27 用户要求: 5 张卡可上下拉动, OPi 沉底靠滚动查看)
    _dash_scroll = lv_obj_create(_scr);
    lv_obj_set_pos(_dash_scroll, 424, 12);
    // 高度恰好容纳 01-04 四卡 (96*3+240+gap36=564), 底部止于 y=604,
    // 不压底部按钮区 (y=616 起); OPi 第 5 卡下拉查看 (2026-10-27 用户指定)
    lv_obj_set_size(_dash_scroll, 844, 608);   // 宽 = 1280-424-12
    lv_obj_set_style_bg_opa(_dash_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_dash_scroll, 0, 0);
    lv_obj_set_style_pad_all(_dash_scroll, 0, 0);
    lv_obj_set_style_pad_row(_dash_scroll, 0, 0);
    lv_obj_set_style_pad_column(_dash_scroll, 0, 0);
    lv_obj_set_scroll_dir(_dash_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_dash_scroll, LV_SCROLLBAR_MODE_OFF);   // 滚动条不显示
    auto row_card = [&](int y, int h, uint32_t bg, uint32_t border) {
        lv_obj_t* card = lv_obj_create(_dash_scroll);
        lv_obj_set_pos(card, 0, y);
        lv_obj_set_size(card, 844, h);
        lv_obj_set_style_bg_color(card, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(border), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        return card;
    };
    auto mk_lbl = [&](lv_obj_t* parent, const lv_font_t* f, uint32_t color,
                      const char* txt, lv_align_t a, int x, int y) {
        lv_obj_t* l = lv_label_create(parent);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_label_set_text(l, txt);
        lv_obj_align(l, a, x, y);
        return l;
    };
    {   // 01 GLM 用量 / 02 DeepSeek 余额 / 03 提醒 / 日程
        // 卡底从上到下逐级加深（渐变），边框同步走
        const uint32_t IDX  = theme::cur().text_dim;   // dim green index
        const uint32_t NAME = theme::cur().text_soft;   // soft text
        const uint32_t VAL  = theme::cur().text_hi;   // bright value
        lv_obj_t* c = row_card(0, 110, theme::cur().card[0], theme::cur().border);
        mk_lbl(c, &lv_font_montserrat_28, IDX,  "01", LV_ALIGN_LEFT_MID,  26, 0);
        mk_lbl(c, zh_font_md(),          NAME, "GLM 用量",   LV_ALIGN_LEFT_MID,  84, 0);
        // 两行: 上=Pro 5H/7D 剩余%, 下=重置时间 (2026-10-27 用户要求)
        _dash_glm  = mk_lbl(c, zh_font_md(), VAL, "--", LV_ALIGN_TOP_RIGHT, -28, 14);
        _dash_glm2 = mk_lbl(c, zh_font_sm(), theme::cur().text_dim, "", LV_ALIGN_TOP_RIGHT, -28, 52);

        c = row_card(122, 110, theme::cur().card[1], theme::cur().border);
        mk_lbl(c, &lv_font_montserrat_28, IDX,  "02", LV_ALIGN_LEFT_MID,  26, 0);
        mk_lbl(c, zh_font_md(),          NAME, "DeepSeek 余额", LV_ALIGN_LEFT_MID, 84, 0);
        _dash_ds = mk_lbl(c, zh_font_md(), VAL, "--", LV_ALIGN_RIGHT_MID, -28, 0);

        c = row_card(244, 110, theme::cur().card[2], theme::cur().border);
        mk_lbl(c, &lv_font_montserrat_28, IDX,  "03", LV_ALIGN_LEFT_MID,  26, 0);
        mk_lbl(c, zh_font_md(),          NAME, "提醒 / 日程", LV_ALIGN_LEFT_MID, 84, 0);
        _dash_ag1 = mk_lbl(c, zh_font_md(), VAL, "--", LV_ALIGN_TOP_RIGHT, -28, 16);
        _dash_ag2 = mk_lbl(c, zh_font_sm(), theme::cur().text_dim, "", LV_ALIGN_TOP_RIGHT, -28, 52);
    }
    {   // 04 每日一句英文宽卡 (hermes /daily/sentence?offset=N, 长按换下一句)
        lv_obj_t* card = row_card(366, 242, theme::cur().card[3], theme::cur().border);
        // 垂直均匀分布: 标题顶 / 英文中 / 中文底 (行数变化间距恒定)
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(card, 24, 0);
        lv_obj_set_style_pad_right(card, 24, 0);
        lv_obj_set_style_pad_top(card, 14, 0);
        lv_obj_set_style_pad_bottom(card, 16, 0);
        mk_lbl(card, zh_font_md(), theme::cur().text_dim, "每日一句",
               LV_ALIGN_DEFAULT, 0, 0);
        _dash_sent_en = lv_label_create(card);
        lv_label_set_text(_dash_sent_en, "--");
        lv_obj_set_style_text_font(_dash_sent_en, zh_font_lg(), 0);
        lv_obj_set_style_text_color(_dash_sent_en, lv_color_hex(theme::cur().text_hi), 0);
        lv_obj_set_style_text_line_space(_dash_sent_en, 10, 0);
        lv_label_set_long_mode(_dash_sent_en, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_dash_sent_en, 844 - 48);
        _dash_sent_cn = lv_label_create(card);
        lv_label_set_text(_dash_sent_cn, "");
        lv_obj_set_style_text_font(_dash_sent_cn, zh_font_md(), 0);
        lv_obj_set_style_text_color(_dash_sent_cn, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_text_line_space(_dash_sent_cn, 6, 0);
        lv_label_set_long_mode(_dash_sent_cn, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_dash_sent_cn, 844 - 48);
        // 长按 (与股票卡片同款 500ms): outline 闪一下 → 换下一句
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))
                ->_nextSentence(lv_event_get_current_target_obj(e));
        }, LV_EVENT_LONG_PRESSED, this);
    }
    {   // 05 OrangePi 宽卡: header + 3x2 module grid（阈值变色条, 参考
        // Mac 的 orange-pi.widget; 温度格无条, 显示风扇状态）— 渐变最深一张
        // (2026-10-27 沉底: 右列改 5 卡滚动后放最下, 上拉查看)
        lv_obj_t* card = row_card(620, 240, theme::cur().card[3], theme::cur().border);

        _opi_dot = lv_obj_create(card);
        lv_obj_set_size(_opi_dot, 12, 12);
        lv_obj_set_pos(_opi_dot, 24, 26);
        lv_obj_set_style_radius(_opi_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(_opi_dot, lv_color_hex(theme::cur().ok), 0);
        lv_obj_set_style_bg_opa(_opi_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_opi_dot, 0, 0);
        lv_obj_set_style_shadow_color(_opi_dot, lv_color_hex(theme::cur().ok), 0);
        lv_obj_set_style_shadow_width(_opi_dot, 8, 0);
        lv_obj_clear_flag(_opi_dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        mk_lbl(card, zh_font_md(), theme::cur().text_hi, "OPi", LV_ALIGN_TOP_LEFT, 46, 16);
        _opi_ip     = mk_lbl(card, zh_font_md(), theme::cur().text_dim, "--", LV_ALIGN_TOP_LEFT, 118, 16);
        _opi_uptime = mk_lbl(card, zh_font_md(), theme::cur().text_dim, "--", LV_ALIGN_TOP_RIGHT, -24, 16);

        static const char* const MOD_LABELS[5] = {"负载", "CPU", "内存", "交换", "磁盘"};
        static const int CELL_X[3] = {24, 312, 600};   // 844 卡内均布 (68 间隙)
        static const int CELL_W    = 220;
        auto mod_at = [&](int i) -> OpiModule& {
            OpiModule& m = _opi_mod[i];
            int x = CELL_X[i % 3], y = (i / 3) == 0 ? 70 : 148;
            lv_obj_t* lbl = lv_label_create(card);
            lv_obj_set_style_text_font(lbl, zh_font_md(), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(theme::cur().text_dim), 0);
            lv_label_set_text(lbl, MOD_LABELS[i]);
            lv_obj_set_pos(lbl, x, y + 2);
            m.val = lv_label_create(card);
            lv_obj_set_style_text_font(m.val, zh_font_md(), 0);
            lv_obj_set_style_text_color(m.val, lv_color_hex(theme::cur().text_hi), 0);
            lv_label_set_text(m.val, "--");
            lv_obj_set_pos(m.val, x, y + 2);
            lv_obj_set_width(m.val, CELL_W);
            lv_obj_set_style_text_align(m.val, LV_TEXT_ALIGN_RIGHT, 0);
            m.bar = lv_bar_create(card);
            lv_obj_set_size(m.bar, CELL_W, 10);
            lv_obj_set_pos(m.bar, x, y + 44);
            lv_bar_set_range(m.bar, 0, 100);
            lv_obj_set_style_bg_color(m.bar, lv_color_hex(theme::cur().track), 0);
            lv_obj_set_style_bg_opa(m.bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(m.bar, 5, 0);
            lv_obj_set_style_radius(m.bar, 5, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(m.bar, lv_color_hex(theme::cur().ok), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(m.bar, LV_OPA_COVER, LV_PART_INDICATOR);
            return m;
        };
        for (int i = 0; i < 5; i++) mod_at(i);

        // 温度格（第 6 格）: 数值按阈值变色, 条的位置显示风扇状态
        lv_obj_t* tl = lv_label_create(card);
        lv_obj_set_style_text_font(tl, zh_font_md(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(theme::cur().text_dim), 0);
        lv_label_set_text(tl, "温度");
        lv_obj_set_pos(tl, CELL_X[2], 148 + 2);
        _opi_temp = lv_label_create(card);
        lv_obj_set_style_text_font(_opi_temp, zh_font_md(), 0);
        lv_obj_set_style_text_color(_opi_temp, lv_color_hex(theme::cur().ok), 0);
        lv_label_set_text(_opi_temp, "--℃");
        lv_obj_set_pos(_opi_temp, CELL_X[2], 148 + 2);
        lv_obj_set_width(_opi_temp, CELL_W);
        lv_obj_set_style_text_align(_opi_temp, LV_TEXT_ALIGN_RIGHT, 0);
        _opi_fan = lv_label_create(card);
        lv_obj_set_style_text_font(_opi_fan, zh_font_sm(), 0);
        lv_obj_set_style_text_color(_opi_fan, lv_color_hex(theme::cur().text_dim), 0);
        lv_label_set_text(_opi_fan, "");
        lv_obj_set_pos(_opi_fan, CELL_X[2], 148 + 44);
    }
    _last_dash_ms = 0;  // force first fetch on next onRunning tick
    _last_status_ms = 0;
    _last_batt_ms   = 0;

    lv_screen_load(_scr);
}

void AppHome::onRunning()
{
    // restoreScreen() is called via callback before a child app closes.
    // Here we just refresh the hero card (clock / weather) + icon row.
    if (!_status_time) return;

    uint32_t now = GetHAL()->millis();
    if (_last_status_ms != 0 && now - _last_status_ms < 1000) return;
    _last_status_ms = now;

    // Clock
    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(_status_time, buf);

    // Date + lunar, e.g. "10月25日 周日" / "农历九月十六" — two lines on the
    // hero card now that the top status bar is gone.
    static const char* const wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    char dbuf[48];
    snprintf(dbuf, sizeof(dbuf), "%d月%d日 周%s", lt.tm_mon + 1, lt.tm_mday, wd[lt.tm_wday]);
    lv_label_set_text(_status_date, dbuf);
    char lbuf[24];
    if (lunar::format_lunar_date(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lbuf, sizeof(lbuf))) {
        char l2[32];
        snprintf(l2, sizeof(l2), "农历%s", lbuf);
        lv_label_set_text(_status_lunar, l2);
    }

    // WiFi (light ink on the deep-green hero card; warm red when offline)
    bool wifi = GetHAL()->isWifiConnected();
    lv_obj_set_style_text_color(_status_wifi, lv_color_hex(wifi ? theme::cur().text_hi : theme::cur().danger), 0);

    // Battery — read the (slower) power monitor about every 10 s
    if (_last_batt_ms == 0 || now - _last_batt_ms > 10000) {
        _last_batt_ms = now;
        auto* hal = GetHAL();
        hal->updatePowerMonitorData();
        float v = hal->powerMonitorData.busVoltage;
        int pct = -1;
        if (v > 0.5f) {                       // NP-F550 7.4V pack: 8.4V=100%, 6.0V=0%
            if      (v >= 8.4f) pct = 100;
            else if (v <= 6.0f) pct = 0;
            else                pct = (int)((v - 6.0f) / 2.4f * 100.0f);
        }
        const char* icon = pct < 0   ? LV_SYMBOL_BATTERY_EMPTY
                         : pct >= 80  ? LV_SYMBOL_BATTERY_FULL
                         : pct >= 55  ? LV_SYMBOL_BATTERY_3
                         : pct >= 30  ? LV_SYMBOL_BATTERY_2
                         : pct >= 10  ? LV_SYMBOL_BATTERY_1
                         :              LV_SYMBOL_BATTERY_EMPTY;
        char bb[24];
        if (pct >= 0) snprintf(bb, sizeof(bb), "%s %d%%", icon, pct);
        else          snprintf(bb, sizeof(bb), "%s --%%", icon);
        lv_label_set_text(_status_batt, bb);
        // Light ink tiers on deep green: warm red < 20%, amber < 40%, ink above
        uint32_t c = pct < 0  ? theme::cur().text_dim
                   : pct < 20 ? theme::cur().danger
                   : pct < 40 ? theme::cur().warn
                   :            theme::cur().text_hi;
        lv_obj_set_style_text_color(_status_batt, lv_color_hex(c), 0);
    }

    // Weather — refetch every 15 min; copy the latest cached text each tick.
    if (_last_weather_ms == 0 || now - _last_weather_ms > 15 * 60 * 1000) {
        _last_weather_ms = now;
        _fetch_weather();
    }
    {
        std::lock_guard<std::mutex> lk(_weather_mutex);
        if (!_weather_temp.empty()) lv_label_set_text(_status_wtemp, _weather_temp.c_str());
        if (!_weather_cond.empty()) lv_label_set_text(_status_wcond, _weather_cond.c_str());
    }

    // Weather-detail popup: worker finished → drop the text into its label.
    if (_wx_body && _wx_ready.exchange(false)) {
        std::lock_guard<std::mutex> lk(_weather_mutex);
        lv_label_set_text(_wx_body, _wx_text.c_str());
    }

    // Email — poll the shared cache every 60s. AppSettings::fetchEmail() is
    // in-flight safe (the second call while one is running is a no-op), so
    // it's fine to call it from the UI loop. The actual httpGet runs on a
    // detached worker thread.
    if (_last_email_ms == 0 || now - _last_email_ms > 60 * 1000) {
        _last_email_ms = now;
        AppSettings::fetchEmail();
    }
    // Reflect current unread count in the status-bar mail icon: hidden when 0,
    // visible (and turns red for high count) when there's mail to read.
    if (_status_email) {
        int unread = AppSettings::email_unread_total.load();
        bool should_show = (unread > 0);
        bool is_shown = !lv_obj_has_flag(_status_email, LV_OBJ_FLAG_HIDDEN);
        if (should_show != is_shown) {
            if (should_show) lv_obj_clear_flag(_status_email, LV_OBJ_FLAG_HIDDEN);
            else             lv_obj_add_flag(_status_email, LV_OBJ_FLAG_HIDDEN);
        }
        if (should_show) {
            // Light ink on deep green: amber when there's a lot to read.
            uint32_t c = unread >= 10 ? theme::cur().warn : theme::cur().text_hi;
            lv_obj_set_style_text_color(_status_email, lv_color_hex(c), 0);
        }
    }

    // Dashboard cards — poll hermes every 60s. All four endpoints are
    // server-side caches (ms-fast), the worker does the HTTP off the UI loop.
    if (_last_dash_ms == 0 || now - _last_dash_ms > 60 * 1000) {
        _last_dash_ms = now;
        _fetch_dashboard();
    }
    {
        std::lock_guard<std::mutex> lk(_dash_mutex);
        if (_dash_glm) lv_label_set_text(_dash_glm, _dash_glm_txt.c_str());
        if (_dash_glm2) lv_label_set_text(_dash_glm2, _dash_glm2_txt.c_str());
        if (_dash_sent_en && _dash_sent_en_txt.compare(lv_label_get_text(_dash_sent_en)) != 0)
            lv_label_set_text(_dash_sent_en, _dash_sent_en_txt.c_str());
        if (_dash_sent_cn && _dash_sent_cn_txt.compare(lv_label_get_text(_dash_sent_cn)) != 0)
            lv_label_set_text(_dash_sent_cn, _dash_sent_cn_txt.c_str());
        if (_dash_ds)  lv_label_set_text(_dash_ds,  _dash_ds_txt.c_str());
        if (_dash_ag1) lv_label_set_text(_dash_ag1, _dash_ag1_txt.c_str());
        if (_dash_ag2) lv_label_set_text(_dash_ag2, _dash_ag2_txt.c_str());

        if (_opi_mod[0].val) _updateOpiWidgets();
    }
}

void AppHome::_fetch_weather()
{
    // Home has no HA client, so read the HA weather entity directly via the REST
    // API (GET /api/states/<entity> with the shared token). The worker only
    // writes strings — it must not touch LVGL — onRunning copies them to labels.
    std::string url = ha_weather::baseUrl(GetHAL()->getConfig("ha_host", ""))
                    + "/api/states/" + ha_weather::ENTITY;
    std::string tok = ha_weather::token();
    GetHAL()->tryRunDetached([this, url, tok]() {
        auto resp = GetHAL()->httpGet(url, {{"Authorization", std::string("Bearer ") + tok}});
        if (!resp.ok) return;
        try {
            auto j = nlohmann::json::parse(resp.body);
            std::string st = j.value("state", "");
            if (st.empty() || st == "unavailable" || st == "unknown") return;
            auto attrs = j.value("attributes", nlohmann::json::object());
            std::string temp;
            if (attrs.contains("temperature")) {
                // temperature may be a JSON number (e.g. 28.1) — render whole degrees.
                double t = attrs["temperature"].is_number()
                         ? attrs["temperature"].get<double>()
                         : std::atof(attrs["temperature"].get<std::string>().c_str());
                temp = std::to_string((int)(t + (t >= 0 ? 0.5 : -0.5)));
            }
            std::lock_guard<std::mutex> lk(_weather_mutex);
            if (!temp.empty()) _weather_temp = temp + "°C";
            _weather_cond = ha_weather::condZh(st);
        } catch (...) {}
    });
}

// 每日一句卡点击: outline 闪 160ms + offset+1 异步拉下一句 (worker 只写
// 文本缓存, onRunning 应用 — 与 _fetch_dashboard 同模式)
void AppHome::_nextSentence(lv_obj_t* card)
{
    if (!card) return;
    lv_obj_set_style_outline_color(card, lv_color_hex(theme::cur().accent), 0);
    lv_obj_set_style_outline_width(card, 6, 0);
    lv_timer_create([](lv_timer_t* tm) {
        lv_obj_set_style_outline_width(
            static_cast<lv_obj_t*>(lv_timer_get_user_data(tm)), 0, 0);
        lv_timer_del(tm);
    }, 160, card);

    const int off = ++_sent_offset;
    const std::string base =
        "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
    GetHAL()->tryRunDetached([this, base, off]() {
        auto resp = GetHAL()->httpGet(base + "/daily/sentence?offset="
                                      + std::to_string(off));
        try {
            auto j = nlohmann::json::parse(resp.body);
            if (j.value("ok", false)) {
                std::lock_guard<std::mutex> lk(_dash_mutex);
                _dash_sent_en_txt = j.value("en", std::string());
                _dash_sent_cn_txt = j.value("cn", std::string());
            }
        } catch (...) {}
    });
}

void AppHome::_fetch_dashboard()
{
    // Four cached endpoints on the OrangePi hermes service (port 8766, the
    // same machine as email-status-server via svc_host):
    //   /glm/quota            → {ok, remaining_pct, level, reset_ms}
    //   /api/deepseek/balance → {ok, total_balance, currency}
    //   /agenda               → {ok, next:{text}, second:{text}|null}  (Apple
    //                          提醒/日程, Mac 桥接: CalDAV + remindctl)
    //   /system/status        → {host, load, memory, disk[], thermal} (OPi 详情卡)
    // Worker only writes strings/_opi; any failure leaves that card at "--".
    std::string base = "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
    GetHAL()->tryRunDetached([this, base]() {
        // → parsed json on {"ok": true, ...}, null json on any failure
        auto pick = [](const std::string& body) -> nlohmann::json {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.value("ok", false)) return j;
            } catch (...) {}
            return nlohmann::json();
        };
        auto sub = [](const nlohmann::json& j, const char* key) -> nlohmann::json {
            return j.is_object() && j.contains(key) && j[key].is_object() ? j[key] : nlohmann::json::object();
        };

        // Row values — the row name ("GLM 用量"…) lives on the left of the
        // card, so these carry data only.
        std::string glm_txt = "--", glm_sub = "", ds_txt = "--";
        std::string sent_en, sent_cn;
        std::string ag1, ag2;

        if (auto j = pick(GetHAL()->httpGet(base + "/glm/quota").body); !j.is_null()) {
            std::string lvl = j.value("level", std::string());
            if (!lvl.empty() && lvl[0] >= 'a' && lvl[0] <= 'z') lvl[0] -= 32;
            // 新版服务端: 5H/7D 双窗口剩余百分比 + 各自重置时间
            if (j.contains("pct_5h") || j.contains("pct_7d")) {
                glm_txt = lvl + "  5H " + std::to_string(j.value("pct_5h", 0)) + "%"
                        + "  7D " + std::to_string(j.value("pct_7d", 0)) + "%";
                // 重置时间: 5H 窗口只显时分, 7D 窗口带日期
                auto fmt_reset = [](int64_t ms, bool with_date) -> std::string {
                    if (ms <= 0) return "--";
                    time_t t = (time_t)(ms / 1000);
                    struct tm tv;
                    localtime_r(&t, &tv);
                    char b[24];
                    if (with_date)
                        std::snprintf(b, sizeof b, "%02d-%02d %02d:%02d",
                                      tv.tm_mon + 1, tv.tm_mday, tv.tm_hour, tv.tm_min);
                    else
                        std::snprintf(b, sizeof b, "%02d:%02d", tv.tm_hour, tv.tm_min);
                    return b;
                };
                glm_sub = "重置 " +
                    fmt_reset(j.value("reset_5h_ms", (int64_t)0), false) + "  /  " +
                    fmt_reset(j.value("reset_7d_ms", (int64_t)0), true);
            } else {
                // 旧版服务端兜底 (只有 5h 窗口的 remaining_pct)
                glm_txt = lvl + "级 剩" + std::to_string(j.value("remaining_pct", 0)) + "%";
            }
        }
        if (auto j = pick(GetHAL()->httpGet(base + "/api/deepseek/balance").body); !j.is_null()) {
            ds_txt = j.value("total_balance", std::string("?")) + " 元";
        }
        if (auto j = pick(GetHAL()->httpGet(base + "/daily/sentence?offset="
                                          + std::to_string(_sent_offset)).body); !j.is_null()) {
            // 每日一句 (按日期轮换, 服务端缓存; 失败保持上次内容)
            std::string en = j.value("en", std::string());
            std::string cn = j.value("cn", std::string());
            if (!en.empty()) { sent_en = en; sent_cn = cn; }
        }
        OpiData od;
        if (auto j = pick(GetHAL()->httpGet(base + "/system/status").body); !j.is_null()) {
            od.ok     = true;
            od.ip     = sub(j, "host").value("ip", std::string());
            od.uptime = sub(j, "host").value("uptime_human", std::string());
            auto ld = sub(j, "load");
            od.load1  = ld.value("load1", 0.0);
            od.load5  = ld.value("load5", 0.0);
            od.load15 = ld.value("load15", 0.0);
            od.cpu    = ld.value("cpu_percent", 0.0);
            auto mm = sub(j, "memory");
            od.mem_pct   = mm.value("percent", 0.0);
            od.swap_pct  = mm.value("swap_percent", 0.0);
            od.mem_used_a = (int)(mm.value("used_mb", 0.0) / 102.4 + 0.5);  // GB×10
            od.mem_tot_a  = (int)(mm.value("total_mb", 0.0) / 102.4 + 0.5);
            nlohmann::json dk = nlohmann::json::object();
            if (j.contains("disk") && j["disk"].is_array() && !j["disk"].empty()
                && j["disk"][0].is_object())
                dk = j["disk"][0];
            od.disk_pct  = dk.value("percent", 0.0);
            od.disk_used = (int)(dk.value("used_gb", 0.0) + 0.5);
            od.disk_tot  = (int)(dk.value("total_gb", 0.0) + 0.5);
            auto th = sub(j, "thermal");
            od.temp = th.value("cpu_temp_c", 0.0);
            if (th.contains("fan_rpm") && th["fan_rpm"].is_number())
                od.fan_rpm = th["fan_rpm"].get<int>();
        }
        if (auto j = pick(GetHAL()->httpGet(base + "/agenda").body); !j.is_null()) {
            if (j.contains("next") && j["next"].is_object())  ag1 = j["next"].value("text", std::string());
            if (j.contains("second") && j["second"].is_object())
                ag2 = j["second"].value("text", std::string());
        }

        std::lock_guard<std::mutex> lk(_dash_mutex);
        _dash_glm_txt = glm_txt;
        _dash_glm2_txt = glm_sub;
        if (!sent_en.empty()) { _dash_sent_en_txt = sent_en; _dash_sent_cn_txt = sent_cn; }
        _dash_ds_txt  = ds_txt;
        _dash_ag1_txt = ag1.empty() ? "今日无提醒/行程" : ag1;
        _dash_ag2_txt = ag2;
        _opi          = od;
    });
}

void AppHome::_updateOpiWidgets()
{
    char v[48];
    auto tier = [](double pct, double warn, double danger) -> uint32_t {
        if (pct >= danger) return theme::cur().danger;   // red
        if (pct >= warn)   return theme::cur().warn;   // amber
        return theme::cur().ok;                      // green
    };
    auto set_bar = [&](lv_obj_t* bar, double pct, double warn, double danger) {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(bar, (int)(pct + 0.5), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(tier(pct, warn, danger)), LV_PART_INDICATOR);
    };

    if (!_opi.ok) {
        for (auto& m : _opi_mod) {
            lv_label_set_text(m.val, "--");
            lv_bar_set_value(m.bar, 0, LV_ANIM_OFF);
        }
        lv_label_set_text(_opi_uptime, "--");
        lv_label_set_text(_opi_ip, "--");
        lv_label_set_text(_opi_temp, "--");
        lv_label_set_text(_opi_fan, "");
        lv_obj_set_style_bg_color(_opi_dot, lv_color_hex(theme::cur().danger), 0);
        lv_obj_set_style_shadow_color(_opi_dot, lv_color_hex(theme::cur().danger), 0);
        return;
    }
    lv_obj_set_style_bg_color(_opi_dot, lv_color_hex(theme::cur().ok), 0);
    lv_obj_set_style_shadow_color(_opi_dot, lv_color_hex(theme::cur().ok), 0);
    lv_label_set_text(_opi_uptime, _opi.uptime.c_str());
    lv_label_set_text(_opi_ip, _opi.ip.c_str());

    // 负载（条取 load1/4 核 ≈ 100%，与 Mac widget 一致）
    snprintf(v, sizeof(v), "%.2f %.2f %.2f", _opi.load1, _opi.load5, _opi.load15);
    lv_label_set_text(_opi_mod[0].val, v);
    set_bar(_opi_mod[0].bar, _opi.load1 / 4.0 * 100.0, 60, 80);

    snprintf(v, sizeof(v), "%.1f%%", _opi.cpu);
    lv_label_set_text(_opi_mod[1].val, v);
    set_bar(_opi_mod[1].bar, _opi.cpu, 60, 80);

    snprintf(v, sizeof(v), "%d.%d/%dG %.0f%%", _opi.mem_used_a / 10, _opi.mem_used_a % 10,
             (_opi.mem_tot_a + 5) / 10, _opi.mem_pct);
    lv_label_set_text(_opi_mod[2].val, v);
    set_bar(_opi_mod[2].bar, _opi.mem_pct, 70, 85);

    snprintf(v, sizeof(v), "%.0f%%", _opi.swap_pct);
    lv_label_set_text(_opi_mod[3].val, v);
    set_bar(_opi_mod[3].bar, _opi.swap_pct, 70, 85);

    snprintf(v, sizeof(v), "%d/%dG %.0f%%", _opi.disk_used, _opi.disk_tot, _opi.disk_pct);
    lv_label_set_text(_opi_mod[4].val, v);
    set_bar(_opi_mod[4].bar, _opi.disk_pct, 70, 85);

    // 温度格: 数值按阈值变色, 风扇状态放在条的位置
    snprintf(v, sizeof(v), "%.0f℃", _opi.temp);
    lv_label_set_text(_opi_temp, v);
    lv_obj_set_style_text_color(_opi_temp, lv_color_hex(tier(_opi.temp, 60, 75)), 0);
    if (_opi.fan_rpm >= 0) snprintf(v, sizeof(v), "风扇 %d", _opi.fan_rpm);
    else                   snprintf(v, sizeof(v), "被动散热");
    lv_label_set_text(_opi_fan, v);
}

void AppHome::onClose()
{
    mclog::tagInfo(_tag, "on close");
    // The modal lives on lv_layer_top(), not on _scr, so delete it explicitly.
    if (_modal) {
        lv_obj_delete(_modal);
        _modal = nullptr;
    }
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _status_time  = nullptr;
    _status_date  = nullptr;
    _status_lunar = nullptr;
    _status_wtemp = nullptr;
    _status_wcond = nullptr;
    _status_wifi  = nullptr;
    _status_batt  = nullptr;
    _status_tools = nullptr;
    _status_email = nullptr;
    _opi_fan      = nullptr;
    _qs_vol_lbl = _qs_brt_lbl = nullptr;
    _net_ssid = _net_pass = _net_host = _net_ssid_dd = _net_kb = _net_status = nullptr;
    _btn_data.clear();
}

void AppHome::_btn_event_cb(lv_event_t* e)
{
    auto* bd = static_cast<BtnData*>(lv_event_get_user_data(e));
    bd->home->_child_app_id = bd->app_id;
    mooncake::GetMooncake().openApp(bd->app_id);
}

// ════════════════════════════════════════════════════════════════════════════
//  Status-bar quick-access popups (power / quick settings / network)
// ════════════════════════════════════════════════════════════════════════════

namespace {
// 弹窗配色 — 全部跟随全局主题（utils/theme/theme.h）
inline uint32_t T_BG()     { return theme::cur().card[1]; }
inline uint32_t T_ACCENT() { return theme::cur().accent; }
inline uint32_t T_LBL()    { return theme::cur().text_soft; }
inline uint32_t T_BTN()    { return theme::cur().press; }
inline uint32_t T_SAVE()   { return theme::cur().ok; }
inline uint32_t T_DANGER() { return theme::cur().danger; }
inline uint32_t T_WARN()   { return theme::cur().warn; }

// Styled pill button. cb is a captureless lambda; `self` is passed as user_data.
lv_obj_t* mk_btn(lv_obj_t* parent, const char* text, uint32_t color, int w, int h,
                 lv_event_cb_t cb, void* self)
{
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, self);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, zh_font_lg(), 0);
    return b;
}
}  // namespace

void AppHome::_closeModal()
{
    if (!_modal) return;
    lv_obj_delete_async(_modal);  // safe to call from within the modal's own event
    _modal      = nullptr;
    _qs_vol_lbl = _qs_brt_lbl = nullptr;
    _qs_theme_lbl = nullptr;
    _net_ssid = _net_pass = _net_host = _net_ssid_dd = _net_kb = _net_status = nullptr;
    _wx_body  = nullptr;
}

lv_obj_t* AppHome::_openModal(int card_w, int card_h, const char* title)
{
    // Full-screen dimming backdrop on the top layer (covers status bar + buttons).
    _modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_modal, 1280, 720);
    lv_obj_set_pos(_modal, 0, 0);
    lv_obj_set_style_bg_color(_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_modal, 0, 0);
    lv_obj_set_style_radius(_modal, 0, 0);
    lv_obj_set_style_pad_all(_modal, 0, 0);
    lv_obj_clear_flag(_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_CLICKABLE);
    // Tap outside the card → dismiss.
    lv_obj_add_event_cb(_modal, _modalBg_cb, LV_EVENT_CLICKED, this);

    // Centered card. CLICKABLE so taps on it are absorbed (don't reach backdrop).
    lv_obj_t* card = lv_obj_create(_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(T_BG()), 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_text_color(t, lv_color_hex(T_ACCENT()), 0);
    lv_obj_set_style_text_font(t, zh_font_lg(), 0);
    return card;
}

// ─── Weather detail popup (weather tap) ─────────────────────────────────────
void AppHome::_openWeatherDialog()
{
    lv_obj_t* card = _openModal(680, 500, "天气详情");
    // Slightly translucent so the home cards show through.
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    lv_obj_center(card);

    _wx_body = lv_label_create(card);
    lv_obj_set_pos(_wx_body, 32, 74);
    lv_obj_set_width(_wx_body, 616);
    lv_obj_set_style_text_color(_wx_body, lv_color_hex(T_LBL()), 0);
    lv_obj_set_style_text_font(_wx_body, zh_font_lg(), 0);
    lv_obj_set_style_text_line_space(_wx_body, 12, 0);
    lv_label_set_text(_wx_body, "加载中…");

    _wx_ready = false;
    _fetchWeatherDetail();
}

void AppHome::_fetchWeatherDetail()
{
    std::string base = ha_weather::baseUrl(GetHAL()->getConfig("ha_host", ""));
    std::string tok  = std::string("Bearer ") + ha_weather::token();
    GetHAL()->tryRunDetached([this, base, tok]() {
        std::string out;
        // ── Current conditions ──
        try {
            auto resp = GetHAL()->httpGet(base + "/api/states/" + ha_weather::ENTITY,
                                          {{"Authorization", tok}});
            if (resp.ok) {
                auto j = nlohmann::json::parse(resp.body);
                std::string st = j.value("state", "");
                auto a = j.value("attributes", nlohmann::json::object());
                auto num = [&](const char* k, double def = 0) {
                    return a.contains(k) && a[k].is_number() ? a[k].get<double>() : def;
                };
                // 和风天气(heweather) already reports wind in km/h, pressure in
                // hPa, and wind_bearing as a Chinese direction string (e.g. "东南风")
                // — no unit conversion needed (unlike the old met.no entity).
                std::string wind_dir = a.value("wind_bearing", std::string("--"));
                char line[160];
                out += ha_weather::condZh(st) + "   " +
                       std::to_string((int)(num("temperature") + 0.5)) + "°C\n";
                snprintf(line, sizeof(line), "湿度 %d%%   露点 %d°C   紫外线 %.1f\n",
                         (int)num("humidity"), (int)(num("dew_point") + 0.5), num("uv_index"));
                out += line;
                snprintf(line, sizeof(line), "云量 %d%%   风 %s%dkm/h   气压 %dhPa\n",
                         (int)num("cloud_coverage"), wind_dir.c_str(),
                         (int)(num("wind_speed") + 0.5),
                         (int)(num("pressure") + 0.5));
                out += line;
            }
        } catch (...) {}

        // ── Daily forecast ──
        try {
            auto resp = GetHAL()->httpPost(
                base + "/api/services/weather/get_forecasts?return_response",
                std::string("{\"entity_id\":\"") + ha_weather::ENTITY + "\",\"type\":\"daily\"}",
                {{"Authorization", tok}, {"Content-Type", "application/json"}});
            if (resp.ok) {
                auto j = nlohmann::json::parse(resp.body);
                auto fc = j["service_response"][ha_weather::ENTITY]["forecast"];
                if (fc.is_array() && !fc.empty()) {
                    out += "未来几天\n";
                    int n = 0;
                    for (auto& f : fc) {
                        if (n >= 6) break;
                        std::string dt = f.value("datetime", "");      // YYYY-MM-DD...
                        std::string md = dt.size() >= 10 ? dt.substr(5, 2) + "/" + dt.substr(8, 2) : dt;
                        std::string cond = ha_weather::condZh(f.value("condition", ""));
                        int hi = f.contains("temperature") && f["temperature"].is_number()
                                 ? (int)(f["temperature"].get<double>() + 0.5) : 0;
                        int lo = f.contains("templow") && f["templow"].is_number()
                                 ? (int)(f["templow"].get<double>() + 0.5) : 0;
                        char cell[64];
                        snprintf(cell, sizeof(cell), "%s %s %d~%d°", md.c_str(), cond.c_str(), lo, hi);
                        out += cell;
                        // Two days per line.
                        out += (n % 2 == 1) ? "\n" : "    ";
                        n++;
                    }
                    if (n % 2 == 1) out += "\n";
                }
            }
        } catch (...) {}

        if (out.empty()) out = "天气获取失败";
        {
            std::lock_guard<std::mutex> lk(_weather_mutex);
            _wx_text = out;
        }
        _wx_ready = true;
    });
}

// ─── Power dialog (battery tap) ─────────────────────────────────────────────
void AppHome::_openPowerDialog()
{
    lv_obj_t* card = _openModal(560, 300, "电源选项");

    lv_obj_t* q = lv_label_create(card);
    lv_label_set_text(q, "是否关机？");
    lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_text_color(q, lv_color_hex(T_LBL()), 0);
    lv_obj_set_style_text_font(q, zh_font_lg(), 0);

    const int BW = 150, BH = 64, GAP = 24;
    int total = BW * 3 + GAP * 2;
    int x0 = (560 - total) / 2;

    lv_obj_t* b_off = mk_btn(card, "关机", T_DANGER(), BW, BH,
        [](lv_event_t* e) {
            auto* s = static_cast<AppHome*>(lv_event_get_user_data(e));
            s->_closeModal();
            GetHAL()->powerOff();
        }, this);
    lv_obj_align(b_off, LV_ALIGN_BOTTOM_LEFT, x0, -32);

    lv_obj_t* b_reb = mk_btn(card, "重启", T_WARN(), BW, BH,
        [](lv_event_t* e) {
            auto* s = static_cast<AppHome*>(lv_event_get_user_data(e));
            s->_closeModal();
            GetHAL()->reboot();
        }, this);
    lv_obj_align(b_reb, LV_ALIGN_BOTTOM_LEFT, x0 + BW + GAP, -32);

    lv_obj_t* b_cancel = mk_btn(card, "取消", T_BTN(), BW, BH,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_closeModal();
        }, this);
    lv_obj_align(b_cancel, LV_ALIGN_BOTTOM_LEFT, x0 + 2 * (BW + GAP), -32);
}

// ─── Quick-settings dialog (gear tap): volume + brightness + theme ──────────
void AppHome::_openQuickSettings()
{
    lv_obj_t* card = _openModal(680, 440, "音量与亮度");

    const int LBL_X = 40, SLD_X = 180, SLD_W = 380, SLD_H = 44;
    const int VAL_X = SLD_X + SLD_W + 18;

    auto mk_row = [&](const char* name, int yy, int min_v, int max_v, int cur_v,
                      lv_event_cb_t cb, lv_obj_t** out_val) -> lv_obj_t* {
        lv_obj_t* l = lv_label_create(card);
        lv_label_set_text(l, name);
        lv_obj_set_pos(l, LBL_X, yy + 6);
        lv_obj_set_style_text_color(l, lv_color_hex(T_LBL()), 0);
        lv_obj_set_style_text_font(l, zh_font_lg(), 0);

        lv_obj_t* sl = lv_slider_create(card);
        lv_obj_set_pos(sl, SLD_X, yy + 6);
        lv_obj_set_size(sl, SLD_W, SLD_H);
        lv_slider_set_range(sl, min_v, max_v);
        lv_slider_set_value(sl, cur_v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(theme::cur().track), LV_PART_MAIN);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(T_ACCENT()), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_radius(sl, SLD_H / 2, LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
        lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t* v = lv_label_create(card);
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", cur_v);
        lv_label_set_text(v, buf);
        lv_obj_set_pos(v, VAL_X, yy + 8);
        lv_obj_set_style_text_color(v, lv_color_hex(T_ACCENT()), 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_28, 0);
        *out_val = v;
        return sl;
    };

    int vol = xiaozhi_get_speaker_volume();
    int brt = GetHAL()->getDisplayBrightness();
    mk_row("音量", 86,  0, 100, vol, _qsVol_cb, &_qs_vol_lbl);
    mk_row("亮度", 158, 10, 100, brt, _qsBrt_cb, &_qs_brt_lbl);

    // 主题循环切换（松绿→暖橙→海蓝→岩紫…）：立即重建主页，其他 app 下次
    // 打开生效。按钮文字显示当前主题名。
    _qs_theme_lbl = mk_btn(card, "", T_BTN(), 300, 60, _qsTheme_cb, this);
    _refreshThemeBtn();
    lv_obj_align(_qs_theme_lbl, LV_ALIGN_BOTTOM_LEFT, 40, -28);

    lv_obj_t* done = mk_btn(card, "完成", T_SAVE(), 200, 60,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_closeModal();
        }, this);
    lv_obj_align(done, LV_ALIGN_BOTTOM_RIGHT, -40, -28);
}

void AppHome::_refreshThemeBtn()
{
    if (!_qs_theme_lbl) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "主题：%s", theme::cur().name);
    lv_label_set_text(lv_obj_get_child(_qs_theme_lbl, 0), buf);
}

void AppHome::_qsTheme_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    theme::cycle();                 // 切换并持久化到 NVS
    self->_refreshThemeBtn();
    self->_closeModal();
    self->rebuild();                // 主页立即换装
}

void AppHome::rebuild()
{
    // 就地重建主页屏幕（主题切换用）。不能同步删除当前活动屏（LVGL9 会
    // 黑屏, app_radio onClose 同款坑）：先切到临时屏 → 删旧屏 → onOpen
    // 建新屏并接管 → 临时屏异步回收。
    if (_scr) {
        lv_obj_t* tmp = lv_obj_create(NULL);
        lv_screen_load(tmp);
        lv_obj_delete(_scr);
        _scr = nullptr;
        onOpen();
        lv_obj_delete_async(tmp);
    } else {
        onOpen();
    }
}

void AppHome::_qsVol_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
    xiaozhi_set_speaker_volume(v);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", v);
    if (self->_qs_vol_lbl) lv_label_set_text(self->_qs_vol_lbl, buf);
}

void AppHome::_qsBrt_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
    GetHAL()->setDisplayBrightness((uint8_t)v);
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", v);
    if (self->_qs_brt_lbl) lv_label_set_text(self->_qs_brt_lbl, buf);
}

// ─── Network dialog (WiFi tap): WiFi + HA server ────────────────────────────
void AppHome::_openNetworkDialog()
{
    auto* hal = GetHAL();
    lv_obj_t* card = _openModal(820, 540, "网络设置");

    const int LBL_X = 36, FLD_X = 250, FLD_W = 520, ROW_H = 66;
    int y = 86;

    auto mk_label = [&](const char* text, int yy) {
        lv_obj_t* l = lv_label_create(card);
        lv_label_set_text(l, text);
        lv_obj_set_pos(l, LBL_X, yy + 12);
        lv_obj_set_style_text_color(l, lv_color_hex(T_LBL()), 0);
        lv_obj_set_style_text_font(l, zh_font_lg(), 0);
    };
    auto mk_ta = [&](int yy, int w, bool password) -> lv_obj_t* {
        lv_obj_t* ta = lv_textarea_create(card);
        lv_obj_set_pos(ta, FLD_X, yy);
        lv_obj_set_size(ta, w, ROW_H - 10);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_password_mode(ta, password);
        lv_obj_set_style_text_font(ta, zh_font_lg(), 0);
        lv_obj_add_event_cb(ta, _netTa_cb, LV_EVENT_ALL, this);
        return ta;
    };

    // WiFi SSID + 扫描
    mk_label("WiFi 名称", y);
    _net_ssid = mk_ta(y, FLD_W - 140, false);
    lv_textarea_set_text(_net_ssid, hal->getConfig("wifi_ssid", "").c_str());
    lv_obj_t* scan = mk_btn(card, "扫描", T_BTN(), 124, ROW_H - 10,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_doNetworkScan();
        }, this);
    lv_obj_set_pos(scan, FLD_X + FLD_W - 124, y);
    y += ROW_H;

    // Scan results dropdown
    mk_label("搜索结果", y);
    _net_ssid_dd = lv_dropdown_create(card);
    lv_obj_set_pos(_net_ssid_dd, FLD_X, y);
    lv_obj_set_size(_net_ssid_dd, FLD_W, ROW_H - 10);
    lv_dropdown_set_options(_net_ssid_dd, "(点扫描)");
    lv_obj_set_style_text_font(_net_ssid_dd, zh_font_lg(), 0);
    lv_obj_add_event_cb(_net_ssid_dd, [](lv_event_t* e) {
        auto* s = static_cast<AppHome*>(lv_event_get_user_data(e));
        char buf[64] = {0};
        lv_dropdown_get_selected_str(s->_net_ssid_dd, buf, sizeof(buf));
        if (buf[0] && buf[0] != '(') lv_textarea_set_text(s->_net_ssid, buf);
    }, LV_EVENT_VALUE_CHANGED, this);
    y += ROW_H;

    // WiFi password
    mk_label("WiFi 密码", y);
    _net_pass = mk_ta(y, FLD_W, true);
    lv_textarea_set_text(_net_pass, hal->getConfig("wifi_pass", "").c_str());
    y += ROW_H;

    // Server host — HA (:8123) + hermes/weather/Claude bridge (:8766/:8770) now
    // all run on the same box, so one field drives both NVS keys.
    // HA token is not entered here — too long for the on-screen keyboard. It
    // falls back to a local, gitignored secrets.h at compile time (or can
    // still be set via NVS "ha_token" some other way); see ha_weather.h.
    mk_label("服务器", y);
    _net_host = mk_ta(y, FLD_W, false);
    lv_textarea_set_text(_net_host, hal->getConfig("ha_host", "").c_str());
    y += ROW_H;

    // Reachy 主机 — Reachy Mini 跑 YRobot FastAPI 服务 (默认 :8042).
    // 独立字段, 因为 Reachy 装在 192.168.1.14, 而 HA/SVC 装在 192.168.1.200/133.
    mk_label("Reachy 主机", y);
    _net_reachy = mk_ta(y, FLD_W, false);
    lv_textarea_set_text(_net_reachy, hal->getConfig("reachy_host", "192.168.1.14").c_str());
    y += ROW_H + 6;

    // Save + status
    lv_obj_t* save = mk_btn(card, "保存并重启", T_SAVE(), 260, 60,
        [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))->_doNetworkSave();
        }, this);
    lv_obj_set_pos(save, FLD_X, y);

    _net_status = lv_label_create(card);
    lv_label_set_text(_net_status, "");
    lv_obj_set_pos(_net_status, FLD_X + 280, y + 16);
    lv_obj_set_style_text_color(_net_status, lv_color_hex(T_WARN()), 0);
    lv_obj_set_style_text_font(_net_status, zh_font_lg(), 0);

    // On-screen keyboard — child of the backdrop so it floats above the card.
    _net_kb = lv_keyboard_create(_modal);
    lv_obj_set_size(_net_kb, 1280, 300);
    lv_obj_align(_net_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_net_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_net_kb, _netKb_cb, LV_EVENT_ALL, this);
}

void AppHome::_doNetworkScan()
{
    if (!_net_status) return;
    lv_label_set_text(_net_status, "扫描中…");
    auto aps = GetHAL()->wifiScan();
    if (aps.empty()) {
        lv_dropdown_set_options(_net_ssid_dd, "(未发现网络)");
        lv_label_set_text(_net_status, "未发现网络");
        return;
    }
    std::string opts;
    for (size_t i = 0; i < aps.size(); i++) {
        if (i) opts += "\n";
        opts += aps[i].ssid;
    }
    lv_dropdown_set_options(_net_ssid_dd, opts.c_str());
    lv_dropdown_set_selected(_net_ssid_dd, 0);
    lv_textarea_set_text(_net_ssid, aps[0].ssid.c_str());
    lv_label_set_text(_net_status, "");
}

void AppHome::_doNetworkSave()
{
    auto* hal = GetHAL();
    std::string ssid  = lv_textarea_get_text(_net_ssid);
    std::string pass  = lv_textarea_get_text(_net_pass);
    std::string host  = ha_weather::hostOnly(lv_textarea_get_text(_net_host));
    std::string reachy_host = ha_weather::hostOnly(lv_textarea_get_text(_net_reachy));
    if (ssid.empty()) {
        lv_label_set_text(_net_status, "请填写 WiFi 名称");
        return;
    }
    hal->setConfig("wifi_ssid", ssid);
    hal->setConfig("wifi_pass", pass);
    if (!host.empty()) {
        hal->setConfig("ha_host", host);
        hal->setConfig("svc_host", host);
    }
    if (!reachy_host.empty()) {
        hal->setConfig("reachy_host", reachy_host);
    }
    lv_label_set_text(_net_status, "已保存，正在重启…");
    mclog::tagInfo(_tag, "network saved ssid={} host={} reachy={}, rebooting", ssid, host, reachy_host);
    hal->delay(800);
    hal->reboot();
}

void AppHome::_netTa_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (!self->_net_kb) return;
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(self->_net_kb, ta);
        lv_obj_clear_flag(self->_net_kb, LV_OBJ_FLAG_HIDDEN);
        AppVoiceInput::requestVoiceInput(ta, 300);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(self->_net_kb, LV_OBJ_FLAG_HIDDEN);
        AppVoiceInput::dismissMicButton();
    }
}

void AppHome::_netKb_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if ((code == LV_EVENT_READY || code == LV_EVENT_CANCEL) && self->_net_kb) {
        lv_obj_add_flag(self->_net_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── Status-bar click routing + backdrop dismiss ────────────────────────────
void AppHome::_statusClick_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    if (self->_modal) return;  // one popup at a time
    lv_obj_t* item = (lv_obj_t*)lv_event_get_target(e);
    intptr_t tag = (intptr_t)lv_obj_get_user_data(item);
    if      (tag == 1) self->_openPowerDialog();
    else if (tag == 2) self->_openNetworkDialog();
    else if (tag == 3) self->_openQuickSettings();
    else if (tag == 4) self->_openWeatherDialog();
    else if (tag == 5 && self->_open_email) self->_open_email();
}

void AppHome::_modalBg_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    // Only a tap on the backdrop itself (not bubbled from a child) dismisses.
    if ((lv_obj_t*)lv_event_get_target(e) == self->_modal) {
        self->_closeModal();
    }
}
