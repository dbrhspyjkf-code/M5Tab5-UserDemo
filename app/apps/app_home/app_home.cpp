#include "app_home.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.h>
#include <hal/hal.h>
#include <ctime>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
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

// 中文字体: 设备从 res 分区 mmap 20px cbin (与 calendar/stocks/rfid/ha/
// radio/reachy 同一模式)。此前直接链接 C 数组版 font_puhui_20_4, 与 res 的
// font_zh_20 是同一字体的两份拷贝, 白占 app 镜像 1.19MB (2026-10-30 移除)。
// res 异常时回退 xiaozhi 内嵌 30px 聊天字体 (已在镜像, 不额外占 flash)。
// 桌面: 无 res 分区, 用 xiaozhi-fonts 编译进的 puhui_20 子集——缺股票生僻
// 字 (钶锶等), 仅影响模拟器观感, 设备端 res 20px 正常 (2026-10-28 实锤)。
#ifdef PLATFORM_BUILD_DESKTOP
#include "../../../platforms/tab5/managed_components/78__xiaozhi-fonts/include/cbin_font.h"
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#else
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
// 2026-10-31 分层终版 (用户确认): 主页基准视觉 = 小智对话 30px——
//   lg = 内嵌 30px, 无缩放标签直用 (状态栏/每日一句/弹窗);
//   md = res font_zh_24 (24px), 专供带 1.25x transform 的标签 (hero 日期/
//        农历/天气/主页按钮): 24×1.25 = 30px 视觉, 缩放架构不动;
//   sm = res font_zh_20 (20px), 右列密集卡 + IPO + 天气弹窗正文:
//        20×1.25 = 25px 视觉 (历史观感, 密排不溢出)。
// res 异常一律回退内嵌 30px。此前 97bf27d 曾把三档全指 res font_zh_20,
// 首刷后全主页缩成 20px (用户实锤"没变大"), 勿再合并档位。
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_md()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f ? f : zh_font_lg();
}
static const lv_font_t* zh_font_sm()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f ? f : zh_font_lg();
}
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
        lv_obj_set_style_text_font(lbl, zh_font_md(), 0);  // 24px×1.25=30px 视觉
        lv_obj_set_style_transform_scale(lbl, 320, 0);   // ≈1.25x: 按钮文字再大一点 (用户指定)
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



    _status_time = lv_label_create(hero);
    lv_obj_set_style_text_font(_status_time, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_status_time, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_time, "--:--");
    // 2026-10-30 二次上移: time 104→24 (顶部大空白), 各行保持原间距 52/54,
    // 天气下句子区扩到 150px (en 3 行 + cn 2 行视觉高度)
    lv_obj_align(_status_time, LV_ALIGN_TOP_MID, 0, 24);

    _status_date = lv_label_create(hero);
    lv_obj_set_style_text_font(_status_date, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_date, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_date, "");
    lv_obj_align(_status_date, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_transform_scale(_status_date, 320, 0);   // 1.25x 与右列卡片一致 (时钟不动, 2026-10-28 用户指定)
    // transform 默认 pivot=左上角: 放大后整体右移 ~25% 自宽, 视觉不居中
    // (2026-10-30 用户实锤); 锁中心 pivot 后 TOP_MID 对齐即真水平居中
    lv_obj_set_style_transform_pivot_x(_status_date, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(_status_date, LV_PCT(50), 0);

    _status_lunar = lv_label_create(hero);
    lv_obj_set_style_text_font(_status_lunar, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_lunar, lv_color_hex(theme::cur().text_soft), 0);
    lv_label_set_text(_status_lunar, "");
    lv_obj_align(_status_lunar, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_transform_scale(_status_lunar, 320, 0);
    lv_obj_set_style_transform_pivot_x(_status_lunar, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(_status_lunar, LV_PCT(50), 0);

    // Weather row (tap → detailed weather dialog): temperature (latin font,
    // has °) + condition (zh font), centered in dark ink.
    lv_obj_t* wbox = lv_obj_create(hero);
    // 宽度给足 (392≈卡宽): SIZE_CONTENT 会按未缩放文字算宽, 容器默认裁剪会截掉
    // 子标签 1.25x 溢出部分 (「多云」的云被截半); OVERFLOW_VISIBLE 在 9.2 只放宽
    // 到容器自身 ext draw size (无变换=0) 不管用, 直接给宽到卡缘
    lv_obj_set_size(wbox, 400 - 8, 40);
    lv_obj_set_style_bg_opa(wbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wbox, 0, 0);
    lv_obj_set_style_pad_all(wbox, 0, 0);
    lv_obj_set_style_pad_column(wbox, 10, 0);
    lv_obj_set_flex_flow(wbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wbox, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(wbox, LV_ALIGN_TOP_MID, 0, 202);

    _status_wtemp = lv_label_create(wbox);
    // 数字用 zh 字体 (与 44.11 元同款): montserrat 同尺寸下笔画粗、视觉偏大 (用户反馈)
    lv_obj_set_style_text_font(_status_wtemp, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_wtemp, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_wtemp, "");
    lv_obj_set_style_transform_scale(_status_wtemp, 320, 0);
    lv_obj_set_style_transform_pivot_x(_status_wtemp, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(_status_wtemp, LV_PCT(50), 0);

    _status_wcond = lv_label_create(wbox);
    lv_obj_set_style_text_font(_status_wcond, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_wcond, lv_color_hex(theme::cur().text_hi), 0);
    lv_label_set_text(_status_wcond, "");
    lv_obj_set_style_transform_scale(_status_wcond, 320, 0);
    lv_obj_set_style_transform_pivot_x(_status_wcond, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(_status_wcond, LV_PCT(50), 0);

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

    // 每日一句 (2026-10-30 从右列轮播卡移来): 天气下方, 英文 + 中文均
    // **不加 1.25x 缩放**——变换后视觉宽会溢出布局盒, 右端"底"字被容器
    // 裁切 (用户实锤); 直接用卡内全宽 352 排版, sbox 横向居中=卡片居中。
    // 长按换下一句 (offset+1, 轻量重拉)。
    // (2026-10-30 晚: en/cn 升 30px zh_font_lg, 与状态栏农历同大小 (用户两次
    // 反馈 20px/24px 均嫌小); 原生字号无变换, 换行按 352 真宽计算。最长句
    // (en 4 行 + cn 2 行 ≈ 218px) 超出 sbox 150 的部分向上下对称溢出, 下距
    // 分隔线 (512) 仍有 ~80px 余量, 上方极端时贴天气块但不裁切)
    lv_obj_t* sbox = lv_obj_create(hero);
    lv_obj_set_pos(sbox, 24, 246);
    lv_obj_set_size(sbox, 352, 150);
    lv_obj_set_style_bg_opa(sbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sbox, 0, 0);
    lv_obj_set_style_pad_all(sbox, 0, 0);
    lv_obj_set_flex_flow(sbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sbox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sbox, 6, 0);
    lv_obj_clear_flag(sbox, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    _hero_sent_en = lv_label_create(sbox);
    lv_obj_set_style_text_font(_hero_sent_en, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_hero_sent_en, lv_color_hex(theme::cur().text_dim), 0);
    lv_obj_set_style_text_line_space(_hero_sent_en, 2, 0);
    lv_obj_set_style_text_align(_hero_sent_en, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_hero_sent_en, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_hero_sent_en, 352);
    lv_label_set_text(_hero_sent_en, "");

    _hero_sent_cn = lv_label_create(sbox);
    lv_obj_set_style_text_font(_hero_sent_cn, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_hero_sent_cn, lv_color_hex(theme::cur().text_soft), 0);
    lv_obj_set_style_text_line_space(_hero_sent_cn, 2, 0);
    lv_obj_set_style_text_align(_hero_sent_cn, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_hero_sent_cn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_hero_sent_cn, 352);
    lv_label_set_text(_hero_sent_cn, "");

    lv_obj_add_flag(sbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sbox, [](lv_event_t* e) {
        static_cast<AppHome*>(lv_event_get_user_data(e))->_sentenceNext();
    }, LV_EVENT_LONG_PRESSED, this);

    // Thin divider + icon row along the card bottom (light ink on deep green).
    lv_obj_t* div = lv_obj_create(hero);
    // 新股申购 (hermes /stocks/ipo; 2026-10-28 用户要求: 第一行"新股申购",
    // 之后每股一行, 最多 3 条; 无申购隐藏)
    _ipo_lbl = lv_label_create(hero);
    lv_obj_set_style_text_font(_ipo_lbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_ipo_lbl, lv_color_hex(theme::cur().accent), 0);
    lv_obj_set_style_text_line_space(_ipo_lbl, 4, 0);   // 1.25x 后收紧, 防满额 4 行顶到天气行
    lv_obj_set_style_text_align(_ipo_lbl, LV_TEXT_ALIGN_CENTER, 0);   // 居中 (用户指定)
    lv_label_set_long_mode(_ipo_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_ipo_lbl, 312);   // 312*1.25=390: 放大后视觉宽恰好回到卡内 (400-2*5)
    lv_obj_set_style_transform_scale(_ipo_lbl, 320, 0);   // 1.25x 与全主页平齐
    // 基准锁底边中心: 横向保持卡片居中 (默认中心 pivot 放大后横向超宽跑偏, 用户实锤),
    // 纵向向上生长、视觉底边贴回分隔线上方
    lv_obj_set_style_transform_pivot_x(_ipo_lbl, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(_ipo_lbl, LV_PCT(100), 0);
    // 相对分隔线定位: 最后一条股票行贴在横线上方, 行数变化向上生长 (用户指定).
    // 建屏时屏未加载, 需 update_layout 先算出真实坐标, align_to 才不跑偏.
    lv_obj_update_layout(_ipo_lbl);
    lv_obj_align_to(_ipo_lbl, div, LV_ALIGN_OUT_TOP_MID, 0, -6);
    _hero_div = div;
    lv_label_set_text(_ipo_lbl, "");
    lv_obj_add_flag(_ipo_lbl, LV_OBJ_FLAG_HIDDEN);

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

    // 收音机后台播放指示 (2026-10-28): 流任务在播时显示, 点按回收音机页。
    // 上拨退出收音机不再停流 —— 主页这个音符图标就是回到它的入口。
    _np_lbl = lv_label_create(irow);
    lv_obj_set_style_text_font(_np_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_np_lbl, lv_color_hex(theme::cur().ok), 0);
    lv_label_set_text(_np_lbl, LV_SYMBOL_AUDIO);
    lv_obj_add_flag(_np_lbl, LV_OBJ_FLAG_HIDDEN);
    make_tappable(_np_lbl, 6);

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
    // 卡片文字统一放大 1.25x (2026-10-28 用户指定: 与 DeepSeek 数值同尺寸)。
    // LVGL 默认 pivot=对象中心; 左上角锚定的标签必须锁 pivot, 否则放大后跑位。
    auto big = [](lv_obj_t* l) { lv_obj_set_style_transform_scale(l, 320, 0); };
    auto big_tl = [](lv_obj_t* l) {   // 顶格标签: 基准左上角, 向右下长
        lv_obj_set_style_transform_scale(l, 320, 0);
        lv_obj_set_style_transform_pivot_x(l, 0, 0);
        lv_obj_set_style_transform_pivot_y(l, 0, 0);
    };
    auto big_left = [](lv_obj_t* l) {   // LEFT_MID 名称: 只锁左缘, 垂直居中不变
        lv_obj_set_style_transform_scale(l, 320, 0);
        lv_obj_set_style_transform_pivot_x(l, 0, 0);
    };
    auto big_right = [](lv_obj_t* l) {   // 右对齐标签: 锁右缘向左长 (右缘永不越界)
        lv_obj_set_style_transform_scale(l, 320, 0);
        lv_obj_set_style_transform_pivot_x(l, LV_PCT(100), 0);
    };
    {   // 01 GLM 用量 / 02 DeepSeek 余额 / 03 提醒 / 日程
        // 卡底从上到下逐级加深（渐变），边框同步走
        const uint32_t IDX  = theme::cur().text_dim;   // dim green index
        const uint32_t NAME = theme::cur().text_soft;   // soft text
        const uint32_t VAL  = theme::cur().text_hi;   // bright value
        lv_obj_t* c = row_card(0, 143, theme::cur().card[0], theme::cur().border);
        mk_lbl(c, zh_font_lg(), NAME, "GLM 用量", LV_ALIGN_LEFT_MID, 26, 0);
        // 左右分区: 左=GLM (5H/7D 剩余% + 重置时间), 右=DeepSeek 余额 (小标+数值)。
        // 全卡 1.25x 后数据列 x280 会顶到右栏, 左移 x200 腾位 (名称放大后到 ~178)。
        _dash_glm  = mk_lbl(c, zh_font_lg(), VAL, "--", LV_ALIGN_TOP_LEFT, 200, 28);
        _dash_glm2 = mk_lbl(c, zh_font_lg(), theme::cur().text_dim, "", LV_ALIGN_TOP_LEFT, 200, 72);
        mk_lbl(c, zh_font_lg(), NAME, "DeepSeek 余额",
               LV_ALIGN_TOP_RIGHT, -28, 28);
        _dash_ds = mk_lbl(c, zh_font_lg(), VAL, "--", LV_ALIGN_TOP_RIGHT, -28, 70);

        c = row_card(155, 143, theme::cur().card[2], theme::cur().border);
        mk_lbl(c, zh_font_lg(), NAME, "提醒 / 日程", LV_ALIGN_LEFT_MID, 26, 0);
        // 内容居中 (2026-10-28 用户: 太靠右); 放大后中心 pivot 保持居中不跑位
        _dash_ag1 = mk_lbl(c, zh_font_lg(), VAL, "--", LV_ALIGN_TOP_MID, 0, 26);
        lv_obj_set_width(_dash_ag1, 844 - 200);
        lv_label_set_long_mode(_dash_ag1, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(_dash_ag1, LV_TEXT_ALIGN_CENTER, 0);
        _dash_ag2 = mk_lbl(c, zh_font_lg(), theme::cur().text_dim, "", LV_ALIGN_TOP_MID, 0, 74);
        lv_obj_set_width(_dash_ag2, 844 - 200);
        lv_label_set_long_mode(_dash_ag2, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(_dash_ag2, LV_TEXT_ALIGN_CENTER, 0);
    }
    {   // 04 AI 新闻轮播宽卡 (hermes /news/ai; 每日一句已移 hero 主卡, 2026-10-30)
        // 长按/每 10s 自动推进下一条; 30 分钟节流拉取
        // 加高 110→140 容纳 1.25x 文字; OPi 卡相应 240→212, 右列仍填满 608
        // 2026-10-30 内容改水平居中: cross=CENTER + 中心 pivot + text CENTER
        // (换行宽 636→560: 视觉 700 恰在卡内容区 708 内, 放大不越界)
        lv_obj_t* card = row_card(310, 143, theme::cur().card[3], theme::cur().border);
        // 垂直均匀分布: 标题/副行随行数变化间距恒定
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(card, 24, 0);
        lv_obj_set_style_pad_right(card, 24, 0);
        lv_obj_set_style_pad_top(card, 8, 0);
        lv_obj_set_style_pad_bottom(card, 8, 0);
        _dash_sent_en = lv_label_create(card);
        lv_label_set_text(_dash_sent_en, "--");
        // 2026-10-31: 字号 sm→md (24×1.25=30px 视觉, 与 hero 每日一句同大);
        // 底部「AI 新闻 · 日期」副行按用户要求移除, 单标签 SPACE_EVENLY 居中
        lv_obj_set_style_text_font(_dash_sent_en, zh_font_lg(), 0);
        lv_obj_set_style_transform_pivot_x(_dash_sent_en, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(_dash_sent_en, LV_PCT(50), 0);
        lv_obj_set_style_text_align(_dash_sent_en, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(_dash_sent_en, lv_color_hex(theme::cur().text_hi), 0);
        lv_obj_set_style_text_line_space(_dash_sent_en, 10, 0);
        lv_label_set_long_mode(_dash_sent_en, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_dash_sent_en, 560);
        // 长按 (与股票卡片同款 500ms): outline 闪一下 → 换下一句
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            static_cast<AppHome*>(lv_event_get_user_data(e))
                ->_carouselNext(lv_event_get_current_target_obj(e));
        }, LV_EVENT_LONG_PRESSED, this);
    }
    {   // 05 健康卡 (hermes /api/health, iPhone 快捷指令推送; 2026-10-31 用户
        // 指定加在 OPi 卡上方): 3x2 六指标 — 标签 sm 暗色 + 数值 md 放大 1.25x
        lv_obj_t* card = row_card(465, 143, theme::cur().card[3], theme::cur().border);
        static const char* const kHLabels[kHealthCells] = {
            "步数", "步行距离", "心率", "血氧",
            "呼吸速率", "活动能量", "爬楼层", "睡眠"};
        for (int i = 0; i < kHealthCells; i++) {
            const int col = i % 4, row = i / 4;   // 4x2 (2026-10-31 加爬楼/睡眠)
            const int cx = 26 + col * 205, cy = 20 + row * 56;
            mk_lbl(card, zh_font_sm(), theme::cur().text_dim, kHLabels[i],
                   LV_ALIGN_TOP_LEFT, cx, cy);
            _dash_h_val[i] = mk_lbl(card, zh_font_lg(), theme::cur().text_hi, "-",
                                    LV_ALIGN_TOP_LEFT, cx, cy + 24);
        }
        // 右下角数据时间 (updated_at 的 HH:MM, 数据新鲜度一眼可见)
        _dash_h_time = mk_lbl(card, zh_font_sm(), theme::cur().text_dim, "--:--",
                              LV_ALIGN_BOTTOM_RIGHT, -24, -6);
        // 长按打开健康 app (app_installer 注入回调; 新闻卡长按同款接法)
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
            if (self->_open_health_cb) self->_open_health_cb();
        }, LV_EVENT_LONG_PRESSED, this);
    }
    {   // 06 OrangePi 宽卡: header + 3x2 module grid（阈值变色条, 参考
        // Mac 的 orange-pi.widget; 温度格无条, 显示风扇状态）— 渐变最深一张
        // 2026-10-31 用户定稿: 首屏 4 卡 (各 143, 总高含行距 = 主卡 608),
        // OPi 卡 y=620 滚动露出 (滚动容器本为此设计)
        lv_obj_t* card = row_card(620, 212, theme::cur().card[3], theme::cur().border);

        _opi_dot = lv_obj_create(card);
        lv_obj_set_size(_opi_dot, 12, 12);
        lv_obj_set_pos(_opi_dot, 24, 22);
        lv_obj_set_style_radius(_opi_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(_opi_dot, lv_color_hex(theme::cur().ok), 0);
        lv_obj_set_style_bg_opa(_opi_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_opi_dot, 0, 0);
        lv_obj_set_style_shadow_color(_opi_dot, lv_color_hex(theme::cur().ok), 0);
        lv_obj_set_style_shadow_width(_opi_dot, 8, 0);
        lv_obj_clear_flag(_opi_dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        mk_lbl(card, zh_font_md(), theme::cur().text_hi, "OPi", LV_ALIGN_TOP_LEFT, 46, 14);
        _opi_ip     = mk_lbl(card, zh_font_md(), theme::cur().text_dim, "--", LV_ALIGN_TOP_LEFT, 128, 14);
        _opi_uptime = mk_lbl(card, zh_font_md(), theme::cur().text_dim, "--", LV_ALIGN_TOP_RIGHT, -24, 14);

        static const char* const MOD_LABELS[5] = {"负载", "CPU", "内存", "交换", "磁盘"};
        static const int CELL_X[3] = {24, 312, 600};   // 844 卡内均布 (68 间隙)
        static const int CELL_W    = 220;
        auto mod_at = [&](int i) -> OpiModule& {
            OpiModule& m = _opi_mod[i];
            int x = CELL_X[i % 3], y = (i / 3) == 0 ? 66 : 134;
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

        // 温度格（第 6 格）: 数值按阈值变色 (风扇状态 2026-10-28 用户要求去掉)
        lv_obj_t* tl = lv_label_create(card);
        lv_obj_set_style_text_font(tl, zh_font_md(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(theme::cur().text_dim), 0);
        lv_label_set_text(tl, "温度");
        lv_obj_set_pos(tl, CELL_X[2], 134 + 2);
        _opi_temp = lv_label_create(card);
        lv_obj_set_style_text_font(_opi_temp, zh_font_md(), 0);
        lv_obj_set_style_text_color(_opi_temp, lv_color_hex(theme::cur().ok), 0);
        lv_label_set_text(_opi_temp, "--℃");
        lv_obj_set_pos(_opi_temp, CELL_X[2] + 76, 134 + 2);
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

    // 轮播: AI 新闻每 10s 推进 (长按卡=手动下一条); 首次立即应用;
    // 新闻数据常晚于首帧到达 (worker 串行拉 6 个端点) → 卡还挂着 "--" 时
    // 数据一到就上屏, 不空等第一个 10s
    if (_car_ms == 0) {
        _car_ms = now;
        _applyCarousel();
    } else if (now - _car_ms >= 10000 && _carousel.size() > 1) {
        _carouselNext(nullptr);
    } else if (!_carousel.empty() && _dash_sent_en
               && lv_label_get_text(_dash_sent_en)[0] == '-') {
        _applyCarousel();
    }

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

    // Weather-detail popup: worker finished → 原地换文字 (卡片固定 840x580,
    // 2026-10-29 用户要求不再自适应缩放)。
    if (_wx_body && _wx_ready.exchange(false)) {
        std::lock_guard<std::mutex> lk(_weather_mutex);
        if (_wx_text != _wx_shown) {
            lv_label_set_text(_wx_body, _wx_text.c_str());
            _wx_shown = _wx_text;
        }
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
    // 邮件图标闪烁相位: 每 500ms 翻转 (1 秒一次完整闪动)
    if (now - _last_blink_ms >= 500) {
        _last_blink_ms = now;
        _mail_blink_on = !_mail_blink_on;
    }
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
            // 1 秒闪动一次 (onRunning 每 500ms 翻转透明度; 2026-10-28 用户要求)
            lv_obj_set_style_opa(_status_email,
                                 _mail_blink_on ? LV_OPA_COVER : LV_OPA_30, 0);
        } else {
            lv_obj_set_style_opa(_status_email, LV_OPA_COVER, 0);
        }
    }

    // 收音机后台播放指示: 流任务在播 → 显示音符图标(点按回收音机页)
    if (_np_lbl) {
        bool playing = _np_getter && !_np_getter().empty();
        bool is_shown = !lv_obj_has_flag(_np_lbl, LV_OBJ_FLAG_HIDDEN);
        if (playing != is_shown) {
            if (playing) lv_obj_clear_flag(_np_lbl, LV_OBJ_FLAG_HIDDEN);
            else         lv_obj_add_flag(_np_lbl, LV_OBJ_FLAG_HIDDEN);
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
        if (_ipo_lbl && _ipo_txt.compare(lv_label_get_text(_ipo_lbl)) != 0) {
            lv_label_set_text(_ipo_lbl, _ipo_txt.c_str());
            if (_ipo_txt.empty()) lv_obj_add_flag(_ipo_lbl, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(_ipo_lbl, LV_OBJ_FLAG_HIDDEN);
            // 行数变化后重新贴回分隔线上方 (UI 线程; worker 里做会栈溢出崩溃)
            lv_obj_update_layout(_ipo_lbl);
            lv_obj_align_to(_ipo_lbl, _hero_div, LV_ALIGN_OUT_TOP_MID, 0, -6);
        }
        if (_dash_sent_en && !_dash_sent_en_txt.empty()
            && _dash_sent_en_txt.compare(lv_label_get_text(_dash_sent_en)) != 0)
            lv_label_set_text(_dash_sent_en, _dash_sent_en_txt.c_str());
        if (_hero_sent_en && _hero_sent_en_txt.compare(lv_label_get_text(_hero_sent_en)) != 0)
            lv_label_set_text(_hero_sent_en, _hero_sent_en_txt.c_str());
        if (_hero_sent_cn && _hero_sent_cn_txt.compare(lv_label_get_text(_hero_sent_cn)) != 0)
            lv_label_set_text(_hero_sent_cn, _hero_sent_cn_txt.c_str());
        if (_dash_ds)  lv_label_set_text(_dash_ds,  _dash_ds_txt.c_str());
        if (_dash_ag1) lv_label_set_text(_dash_ag1, _dash_ag1_txt.c_str());
        if (_dash_ag2) lv_label_set_text(_dash_ag2, _dash_ag2_txt.c_str());
        for (int i = 0; i < kHealthCells; i++)
            if (_dash_h_val[i]
                && _dash_h_txt[i].compare(lv_label_get_text(_dash_h_val[i])) != 0)
                lv_label_set_text(_dash_h_val[i], _dash_h_txt[i].c_str());
        if (_dash_h_time
            && _dash_h_time_txt.compare(lv_label_get_text(_dash_h_time)) != 0)
            lv_label_set_text(_dash_h_time, _dash_h_time_txt.c_str());

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
void AppHome::_applyCarousel()
{
    // 写数据源(_dash_sent_*_txt)而非直接写 label —— onRunning 每秒的
    // 同步块会把 label 拉回数据源, 只写 label 会在 1s 内被打回原条目
    // (2026-10-29 "轮播不切/长按松手弹回"真因)
    std::lock_guard<std::mutex> lk(_dash_mutex);
    if (_carousel.empty()) return;
    if (_car_idx >= (int)_carousel.size()) _car_idx = 0;
    _dash_sent_en_txt = _carousel[_car_idx].l1;   // l2 日期副行已移除 (2026-10-31)
}

void AppHome::_sentenceNext()
{
    // hero 每日一句长按: offset+1, 只拉 sentence 端点写 _hero_sent_*_txt,
    // onRunning 同步块应用 (与全量 _fetch_dashboard 相同的 worker→UI 模式)
    ++_sent_offset;
    std::string base = "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
    int off = _sent_offset;
    GetHAL()->tryRunDetached([this, base, off]() {
        auto resp = GetHAL()->httpGet(base + "/daily/sentence?offset=" + std::to_string(off));
        try {
            auto j = nlohmann::json::parse(resp.body);
            if (!j.value("ok", false)) return;
            std::string en = j.value("en", std::string());
            std::string cn = j.value("cn", std::string());
            if (en.empty()) return;
            std::lock_guard<std::mutex> lk(_dash_mutex);
            _hero_sent_en_txt = en;
            _hero_sent_cn_txt = cn;
        } catch (...) {}
    });
}

void AppHome::_carouselNext(lv_obj_t* card)
{
    // 闪烁环(仅长按有 card; 定时推进无环): 画在 lv_layer_top (句卡是滚动
    // 容器最后一张, 左右/底部贴容器缘, 直接 outline 会被容器裁剪只剩顶边
    // —— 顶层层不受裁剪, 四边完整可见), 同卡片底色的 6px 描边环外扩 6px,
    // 160ms 后消失 (同万年历日期格观感).
    if (card) {
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        lv_obj_t* ring = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(ring);
        lv_obj_set_pos(ring, a.x1 - 6, a.y1 - 6);
        lv_obj_set_size(ring, lv_area_get_width(&a) + 12, lv_area_get_height(&a) + 12);
        lv_obj_set_style_radius(ring, 24, 0);
        lv_obj_set_style_border_width(ring, 6, 0);
        lv_obj_set_style_border_color(ring, lv_obj_get_style_bg_color(card, 0), 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_timer_create([](lv_timer_t* tm) {
            lv_obj_del(static_cast<lv_obj_t*>(lv_timer_get_user_data(tm)));
            lv_timer_del(tm);
        }, 160, ring);
    }

    // 推进轮播下一条 + 重置 10s 定时
    _car_ms = GetHAL()->millis();
    {
        std::lock_guard<std::mutex> lk(_dash_mutex);
        if (_carousel.empty()) return;
        _car_idx = (_car_idx + 1) % (int)_carousel.size();
    }
    _applyCarousel();
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
        std::string glm_txt = "--", glm_sub = "", ds_txt = "--", ipo_txt = "\x01";  // \x01=保持不变哨兵
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
        if (auto j = pick(GetHAL()->httpGet(base + "/stocks/ipo").body); !j.is_null()) {
            // 新股申购 (2026-10-28 用户定稿): 首行"新股申购", 之后每股一行,
            // 最多 3 条 (hero 空间限制)
            std::string ipo = "新股申购";
            int shown = 0;
            if (j.contains("items") && j["items"].is_array())
                for (auto& it : j["items"]) {
                    if (shown >= 3) break;
                    ipo += "\n" + it.value("name", std::string()) + " "
                         + it.value("date", std::string());
                    if (it.value("today", false)) ipo += "(今)";
                    shown++;
                }
            ipo_txt = shown ? ipo : "";
        }
        if (auto j = pick(GetHAL()->httpGet(base + "/daily/sentence?offset="
                                          + std::to_string(_sent_offset)).body); !j.is_null()) {
            // 每日一句 (按日期轮换, 服务端缓存; 失败保持上次内容)
            std::string en = j.value("en", std::string());
            std::string cn = j.value("cn", std::string());
            if (!en.empty()) { sent_en = en; sent_cn = cn; }
        }
        // AI 新闻 (hermes /news/ai, 同 AI Passport 数据契约; 30 分钟节流)
        // 条目并入每日一句卡轮播: 每条一行 "【分区】标题", 副行日期
        static std::atomic<uint32_t> s_news_ms{0};
        std::vector<CarItem> news;
        uint32_t wnow = GetHAL()->millis();
        if (s_news_ms.load() == 0 || wnow - s_news_ms.load() >= 30u * 60 * 1000) {
            // 节流只在拿到非空数据时生效 (2026-10-30 修): 请求前先标记会把
            // 一次瞬时失败放大成 30 分钟无新闻
            if (auto j = pick(GetHAL()->httpGet(base + "/news/ai").body); !j.is_null()) {
                std::string ndate = j.value("date", std::string());
                if (j.contains("secs") && j["secs"].is_array())
                    for (auto& sec : j["secs"]) {
                        std::string label = sec.value("label", std::string("新闻"));
                        if (!sec.contains("items") || !sec["items"].is_array()) continue;
                        for (auto& it : sec["items"]) {
                            if (!it.is_string()) continue;
                            std::string t = it.get<std::string>();
                            if (t.empty()) continue;
                            news.push_back({"【" + label + "】" + t, "AI 新闻 · " + ndate});
                            if (news.size() >= 12) break;
                        }
                        if (news.size() >= 12) break;
                    }
                if (!news.empty()) s_news_ms.store(wnow);
            }
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
        // 健康卡 6 指标 (hermes /api/health; 缺类型显示 "-", 拉取失败保持旧值)
        std::string h_txt[kHealthCells];
        std::string h_time = "--:--";
        if (auto j = pick(GetHAL()->httpGet(base + "/api/health").body); !j.is_null()) {
            auto types = (j.contains("types") && j["types"].is_object())
                         ? j["types"] : nlohmann::json::object();
            // {type, 单位, 小数位}: 与卡内 3x2 顺序一致
            struct HM { const char* t; const char* u; int dec; };
            static const HM hm[kHealthCells] = {
                {"step_count", " 步", 0},
                {"walking_running_distance", " km", 2},
                {"heart_rate", " bpm", 0},
                {"blood_oxygen_saturation", " %", 0},
                {"respiratory_rate", " 次/分", 1},
                {"active_energy_burned", " kcal", 0},
                {"flights_climbed", " 层", 0},
                {"sleep_analysis", "", -2},   // 分钟 → "h:mm 小时"
            };
            {
                std::string ua = j.value("updated_at", std::string());
                if (ua.size() >= 16) h_time = ua.substr(11, 5);
            }
            for (int i = 0; i < kHealthCells; i++) {
                h_txt[i] = "-";
                if (!types.contains(hm[i].t) || !types[hm[i].t].is_object()) continue;
                const auto& e = types[hm[i].t];
                if (!e.contains("value")) continue;
                double v = 0;
                if (e["value"].is_number()) v = e["value"].get<double>();
                else if (e["value"].is_string()) v = atof(e["value"].get<std::string>().c_str());
                else continue;
                char buf[24];
                if (hm[i].dec == -2)   // 睡眠: 分钟 → "7:32 小时"
                    snprintf(buf, sizeof(buf), "%d:%02d 小时",
                             (int)(v / 60), ((int)v) % 60);
                else if (hm[i].dec == 0) snprintf(buf, sizeof(buf), "%d%s", (int)llround(v), hm[i].u);
                else snprintf(buf, sizeof(buf), "%.*f%s", hm[i].dec, v, hm[i].u);
                h_txt[i] = buf;
            }
        }

        std::lock_guard<std::mutex> lk(_dash_mutex);
        _dash_glm_txt = glm_txt;
        _dash_glm2_txt = glm_sub;
        if (!sent_en.empty()) { _hero_sent_en_txt = sent_en; _hero_sent_cn_txt = sent_cn; }
        if (!news.empty()) _news_items = news;
        // 轮播 = 纯 AI 新闻 (每日一句移 hero 主卡, 2026-10-30); 越界索引归零
        _carousel = _news_items;
        if (_car_idx >= (int)_carousel.size()) _car_idx = 0;
        _ipo_txt = ipo_txt;   // 空串=无新股, UI 隐藏
        mclog::tagInfo(_tag, "[dbg] ipo='{}'", ipo_txt.c_str());
        _dash_ds_txt  = ds_txt;
        _dash_ag1_txt = ag1.empty() ? "今日无提醒/行程" : ag1;
        _dash_ag2_txt = ag2;
        for (int i = 0; i < kHealthCells; i++)
            if (!h_txt[i].empty()) _dash_h_txt[i] = h_txt[i];   // 空=拉取失败保旧值
        if (h_time != "--:--") _dash_h_time_txt = h_time;
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

    // 温度格: 数值按阈值变色 (风扇状态已去掉, 2026-10-28 用户要求)
    snprintf(v, sizeof(v), "%.0f℃", _opi.temp);
    lv_label_set_text(_opi_temp, v);
    lv_obj_set_style_text_color(_opi_temp, lv_color_hex(tier(_opi.temp, 60, 75)), 0);
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
    _np_lbl       = nullptr;
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
    _qs_ss_chk = _qs_ss_dd = nullptr;
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
    // 卡皮与主页 GLM 卡一致 (2026-10-28 用户要求): card[0] 底 / radius 18 / 1px 边框 70%
    lv_obj_t* card = lv_obj_create(_modal);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[0]), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_text_color(t, lv_color_hex(T_ACCENT()), 0);
    lv_obj_set_style_text_font(t, zh_font_lg(), 0);
    lv_obj_set_style_transform_scale(t, 320, 0);   // 1.25x 与主页卡片平齐
    // 基准点须设中心: LVGL9 默认 pivot=左上角, 1.25x 放大后文字向右下扩张,
    // TOP_MID 对齐看着偏右不居中 (状态栏同款写法 L196)
    lv_obj_set_style_transform_pivot_x(t, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(t, LV_PCT(50), 0);
    return card;
}

// ─── Weather detail popup (weather tap) ─────────────────────────────────────
void AppHome::_openWeatherDialog()
{
    // 固定尺寸 (2026-10-29 用户要求, 不再随内容自适应): 按最坏内容
    // (当前天气 3 行 + 未来几天 4 行) 预留。有上次内容直接显示, 首次
    // 「加载中…」, 数据到手后原地换文字, 卡片永不缩放/展开。
    // 固定尺寸 (2026-10-29 用户三轮调参: 840x580 → 宽-35% → 高再-5%)。
    lv_obj_t* card = _openModal(546, 440, "天气详情");

    _wx_body = lv_label_create(card);
    lv_obj_set_pos(_wx_body, 32, 74);
    lv_obj_set_width(_wx_body, 392);   // (546-32-24)/1.25 换算回名义宽
    lv_obj_set_style_text_color(_wx_body, lv_color_hex(theme::cur().text_hi), 0);
    lv_obj_set_style_text_font(_wx_body, zh_font_sm(), 0);
    lv_obj_set_style_text_line_space(_wx_body, 12, 0);
    lv_obj_set_style_transform_scale(_wx_body, 320, 0);   // 1.25x 与主页卡片平齐
    lv_obj_set_style_transform_pivot_x(_wx_body, 0, 0);
    lv_obj_set_style_transform_pivot_y(_wx_body, 0, 0);
    lv_label_set_text(_wx_body, _wx_shown.empty() ? "加载中…" : _wx_shown.c_str());

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

// ─── Quick-settings dialog (gear tap): volume + brightness + screensaver + theme ──
void AppHome::_openQuickSettings()
{
    lv_obj_t* card = _openModal(680, 440, "设置");

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

    // 屏保行 (2026-10-28 用户需求): 勾选=闲置后进屏保; 不勾=不弹屏保、改为把
    // 亮度降到 5%。下拉选闲置时间 (2/5/10 分钟), 两种模式共用。NVS 持久化,
    // AppIdleScreen 每 3s 刷新配置, 改完即生效。
    lv_obj_t* ss_l = lv_label_create(card);
    lv_label_set_text(ss_l, "屏保");
    lv_obj_set_pos(ss_l, LBL_X, 240);
    lv_obj_set_style_text_color(ss_l, lv_color_hex(T_LBL()), 0);
    lv_obj_set_style_text_font(ss_l, zh_font_lg(), 0);

    _qs_ss_chk = lv_checkbox_create(card);
    lv_checkbox_set_text(_qs_ss_chk, "");
    lv_obj_set_pos(_qs_ss_chk, SLD_X, 236);
    lv_obj_set_style_text_font(_qs_ss_chk, zh_font_lg(), 0);
    lv_obj_set_style_bg_color(_qs_ss_chk, lv_color_hex(theme::cur().track), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_qs_ss_chk, lv_color_hex(T_ACCENT()),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (GetHAL()->getConfig("screensaver_en", "1") != "0")
        lv_obj_add_state(_qs_ss_chk, LV_STATE_CHECKED);
    lv_obj_add_event_cb(_qs_ss_chk, _qsSs_chk_cb, LV_EVENT_VALUE_CHANGED, this);

    _qs_ss_dd = lv_dropdown_create(card);
    lv_dropdown_set_options(_qs_ss_dd, "2 分钟\n5 分钟\n10 分钟");  // set_options 内部复制, 勿用 _static
    int mins = atoi(GetHAL()->getConfig("screensaver_min", "5").c_str());
    lv_dropdown_set_selected(_qs_ss_dd, mins <= 2 ? 0 : (mins <= 5 ? 1 : 2));
    lv_obj_set_pos(_qs_ss_dd, SLD_X + 72, 232);
    lv_obj_set_size(_qs_ss_dd, 220, 44);
    lv_obj_set_style_text_font(_qs_ss_dd, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_qs_ss_dd, lv_color_hex(T_LBL()), 0);
    lv_obj_set_style_bg_color(_qs_ss_dd, lv_color_hex(theme::cur().track), 0);
    lv_dropdown_set_symbol(_qs_ss_dd, nullptr);   // 默认 FA 箭头 zh 字体无字形
    // 列表对象构造时就已存在 (parent=所在 screen), 当场设字体否则中文变 tofu
    lv_obj_t* ss_list = lv_dropdown_get_list(_qs_ss_dd);
    if (ss_list) {
        lv_obj_set_style_text_font(ss_list, zh_font_lg(), 0);
        lv_obj_set_style_text_color(ss_list, lv_color_hex(theme::cur().text_hi), 0);
        lv_obj_set_style_bg_color(ss_list, lv_color_hex(theme::cur().card[1]), 0);
        lv_obj_set_style_bg_opa(ss_list, LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(_qs_ss_dd, _qsSs_dd_cb, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, "勾选：闲置后显示屏保；不勾：闲置后熄屏");
    lv_obj_set_pos(hint, LBL_X, 300);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme::cur().text_dim), 0);
    lv_obj_set_style_text_font(hint, zh_font_sm(), 0);

    // 主题循环切换（松绿→暖橙→海蓝→岩紫…）：立即重建主页，其他 app 下次
    // 打开生效。按钮文字显示当前主题名。
    _qs_theme_lbl = mk_btn(card, "", T_BTN(), 300, 60, _qsTheme_cb, this);
    _refreshThemeBtn();
    lv_obj_align(_qs_theme_lbl, LV_ALIGN_BOTTOM_LEFT, 40, -28);

    lv_obj_t* done = mk_btn(card, "完成", T_ACCENT(), 200, 60,
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

void AppHome::_qsSs_chk_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    bool on = lv_obj_has_state(self->_qs_ss_chk, LV_STATE_CHECKED);
    GetHAL()->setConfig("screensaver_en", on ? "1" : "0");
}

void AppHome::_qsSs_dd_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    if (!self->_qs_ss_dd) return;
    static const char* const kMins[] = {"2", "5", "10"};
    int idx = lv_dropdown_get_selected(self->_qs_ss_dd);
    if (idx < 0 || idx > 2) idx = 1;
    GetHAL()->setConfig("screensaver_min", kMins[idx]);
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
    lv_obj_t* save = mk_btn(card, "保存并重启", T_ACCENT(), 260, 60,
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
    else if (tag == 6) {   // 收音机后台播放中 → 回收音机页
        for (auto& e : self->_apps) {
            if (e.name == "收音机") { mooncake::GetMooncake().openApp(e.app_id); break; }
        }
    }
}

void AppHome::_modalBg_cb(lv_event_t* e)
{
    auto* self = static_cast<AppHome*>(lv_event_get_user_data(e));
    // Only a tap on the backdrop itself (not bubbled from a child) dismisses.
    if ((lv_obj_t*)lv_event_get_target(e) == self->_modal) {
        self->_closeModal();
    }
}
