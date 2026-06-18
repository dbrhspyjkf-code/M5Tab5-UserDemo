/*
 * Home Assistant Dashboard — iOS-style light theme
 * 1280×720, LVGL 9
 */
#include "view.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <hal/hal.h>
#include <mooncake_log.h>

using namespace ha_view;

// Full-coverage Chinese fonts (same as the Claude app) — GB2312 common ~3500
// glyphs, so HA labels never render as tofu boxes. The old font_zh_18/_36 were
// subset fonts (~135 chars) that boxed any char outside the subset.
//   zh_font_lg() — 30px, replaces the old 36px font_zh_36
//   zh_font_sm() — 20px, replaces the old 18px font_zh_18
// On device these are cbin blobs used in-place from flash (no big RAM copy);
// on the desktop sim we fall back to the linked 20px C-array font.
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
extern const uint8_t font_puhui_common_20_4_bin_start[]
    asm("_binary_font_puhui_common_20_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_sm()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_20_4_bin_start);
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif

// ─── Colour palette ───────────────────────────────────────────────────────────
static constexpr uint32_t C_BG          = 0x0A1A2E;  // light blue background
static constexpr uint32_t C_CARD        = 0x13304E;  // card background
static constexpr uint32_t C_CARD_ON     = 0x1E5288;  // active card
static constexpr uint32_t C_ACCENT      = 0x4FA3FF;  // blue accent
static constexpr uint32_t C_ACCENT_SOFT = 0x1A3A5C;  // soft blue fill
static constexpr uint32_t C_TEXT        = 0xEAF3FB;  // primary text
static constexpr uint32_t C_TEXT2       = 0x8FA8C4;  // secondary text
static constexpr uint32_t C_TAB_BG      = 0x0C2038;  // tab bar bg
static constexpr uint32_t C_TAB_ACTIVE  = 0x1E6FC2;  // active tab pill
static constexpr uint32_t C_GREEN       = 0x34A853;
static constexpr uint32_t C_AMBER       = 0xF59E0B;
static constexpr uint32_t C_RED         = 0xEF4444;
static constexpr uint32_t C_FAN_ACTIVE  = 0x4FA3FF;
static constexpr uint32_t C_SONOS_PURP  = 0x7C3AED;  // Sonos brand purple
static constexpr uint32_t C_SONOS_SOFT  = 0x2A2350;  // soft purple fill
static constexpr uint32_t C_PILL_PLAY   = 0x14402A;  // playing pill background
static constexpr uint32_t C_PILL_IDLE   = 0x1A2C42;  // paused/stopped pill background

// ─── Layout constants ─────────────────────────────────────────────────────────
static constexpr int W       = 1280;
static constexpr int H       = 720;
static constexpr int PAD     = 16;
static constexpr int GAP     = 12;
static constexpr int RADIUS  = 20;

static constexpr int HDR_H   = 80;   // header strip
static constexpr int TAB_H   = 82;   // bottom tab bar (+20%)
static constexpr int CONT_Y  = GAP;
static constexpr int CONT_H  = H - TAB_H - GAP * 2;

// Card sizes inside content area
static constexpr int CARD_H  = 123;  // standard device card height
static constexpr int FAN_H   = 430;  // fan card height (2-col layout: 4 gears left, 3 btns right)
static constexpr int LOCK_H  = 180;  // smart lock card height
static constexpr int SONOS_H    = 292;  // sonos card (btn_h=70: +32 over original 260)
static constexpr int TV_H       = 290;  // TV card (btn_h=70: +32 over original 258)
static constexpr int FISHTANK_H = 210;  // combined fish tank card
static constexpr int VACUUM_H   = 210;  // 扫地机器人 card (state + 4 buttons)
static constexpr int WASHER_H   = 210;  // 洗衣机 status card
static constexpr int PRINTER_H  = 220;  // 拓竹 3D 打印机 detail card

// ─── Forward declarations ────────────────────────────────────────────────────
static lv_obj_t* _make_card(lv_obj_t* parent, int x, int y, int w, int h,
                              uint32_t bg = C_CARD, int radius = RADIUS);
static void _add_shadow(lv_obj_t* obj);

// True while the user is actively pressing or scrolling with any pointer.
// Used to defer tab rebuilds: yanking the content out from under a live touch
// both janks the gesture and is the classic mid-scroll use-after-free window.
static bool _any_pointer_active()
{
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) return true;
            if (lv_indev_get_scroll_obj(indev) != nullptr) return true;
        }
        indev = lv_indev_get_next(indev);
    }
    return false;
}

// ─── Card click data ──────────────────────────────────────────────────────────
struct CardData {
    HaView*     view;
    std::string entity_id;
    std::string action;
    std::string value;
};

static void _action_cb(lv_event_t* e)
{
    CardData* d = (CardData*)lv_event_get_user_data(e);
    if (d && d->view) {
        if (d->view->_on_action_fn)
            d->view->_on_action_fn(d->entity_id, d->action, d->value);
        // Optimistic UI: rebuild on the very next frame instead of waiting
        // up to TAB_REBUILD_INTERVAL_MS.
        d->view->requestRebuild();
    }
}

static void _card_data_delete_cb(lv_event_t* e)
{
    delete (CardData*)lv_event_get_user_data(e);
}

static void _bind_action(lv_obj_t* obj, CardData* data)
{
    lv_obj_add_event_cb(obj, _action_cb, LV_EVENT_CLICKED, data);
    lv_obj_add_event_cb(obj, _card_data_delete_cb, LV_EVENT_DELETE, data);
}

// Tab switch callback data
struct TabData { HaView* view; TabPage tab; };
static void _tab_cb(lv_event_t* e)
{
    TabData* d = (TabData*)lv_event_get_user_data(e);
    if (d && d->view) d->view->_switch_tab(d->tab);
}

static void _tab_data_delete_cb(lv_event_t* e)
{
    delete (TabData*)lv_event_get_user_data(e);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static lv_obj_t* _make_card(lv_obj_t* parent, int x, int y, int w, int h,
                              uint32_t bg, int radius)
{
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void _add_shadow(lv_obj_t* obj)
{
    lv_obj_set_style_shadow_width(obj, 18, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(0x5A7A9C), 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 4, 0);
}

static lv_obj_t* _make_label(lv_obj_t* parent, const char* text,
                               uint32_t color, const lv_font_t* font,
                               lv_align_t align, int ox, int oy)
{
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_align(lbl, align, ox, oy);
    return lbl;
}

static void _add_media_button_content(lv_obj_t* button,
                                      const char* icon,
                                      const char* label,
                                      uint32_t color)
{
    lv_obj_t* ico = lv_label_create(button);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_color(ico, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_28, 0);
    lv_obj_align(ico, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t* txt = lv_label_create(button);
    lv_label_set_text(txt, label);
    lv_obj_set_style_text_font(txt, zh_font_lg(), 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(color), 0);
    lv_obj_align(txt, LV_ALIGN_LEFT_MID, 18 + 38 + 12, 0);
}

static void _add_lightbulb_icon(lv_obj_t* parent, uint32_t color)
{
    lv_color_t c = lv_color_hex(color);

    lv_obj_t* bulb = lv_obj_create(parent);
    lv_obj_set_size(bulb, 30, 34);
    lv_obj_set_style_bg_opa(bulb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bulb, 3, 0);
    lv_obj_set_style_border_color(bulb, c, 0);
    lv_obj_set_style_radius(bulb, 15, 0);
    lv_obj_clear_flag(bulb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bulb, LV_ALIGN_CENTER, 0, -7);

    lv_obj_t* neck = lv_obj_create(parent);
    lv_obj_set_size(neck, 18, 8);
    lv_obj_set_style_bg_color(neck, c, 0);
    lv_obj_set_style_border_width(neck, 0, 0);
    lv_obj_set_style_radius(neck, 3, 0);
    lv_obj_clear_flag(neck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(neck, LV_ALIGN_CENTER, 0, 13);

    lv_obj_t* base = lv_obj_create(parent);
    lv_obj_set_size(base, 14, 5);
    lv_obj_set_style_bg_color(base, c, 0);
    lv_obj_set_style_border_width(base, 0, 0);
    lv_obj_set_style_radius(base, 2, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(base, LV_ALIGN_CENTER, 0, 21);
}

static void _add_tv_icon(lv_obj_t* parent, uint32_t color)
{
    lv_color_t c = lv_color_hex(color);

    // Screen body
    lv_obj_t* screen = lv_obj_create(parent);
    lv_obj_set_size(screen, 30, 20);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screen, 3, 0);
    lv_obj_set_style_border_color(screen, c, 0);
    lv_obj_set_style_radius(screen, 3, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screen, LV_ALIGN_CENTER, 0, -7);

    // Stand neck
    lv_obj_t* neck = lv_obj_create(parent);
    lv_obj_set_size(neck, 4, 7);
    lv_obj_set_style_bg_color(neck, c, 0);
    lv_obj_set_style_border_width(neck, 0, 0);
    lv_obj_set_style_radius(neck, 1, 0);
    lv_obj_clear_flag(neck, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(neck, LV_ALIGN_CENTER, 0, 10);

    // Stand base
    lv_obj_t* stand = lv_obj_create(parent);
    lv_obj_set_size(stand, 18, 4);
    lv_obj_set_style_bg_color(stand, c, 0);
    lv_obj_set_style_border_width(stand, 0, 0);
    lv_obj_set_style_radius(stand, 2, 0);
    lv_obj_clear_flag(stand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(stand, LV_ALIGN_CENTER, 0, 17);
}

// ─── Device icon card (toggle) ────────────────────────────────────────────────
static void _build_device_card(lv_obj_t* parent, int x, int y, int w, int h,
                                 const DeviceCard& card, HaView* view)
{
    lv_obj_t* c = _make_card(parent, x, y, w, h, C_CARD);
    _add_shadow(c);

    // Icon circle (left side, vertically centered)
    int ic_size = 64;
    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(card.is_offline ? 0x16242E
                                  : (card.is_on ? C_ACCENT_SOFT : 0x1A2C42)), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);

    const char* icon_text = card.icon.empty() ? LV_SYMBOL_HOME : card.icon.c_str();
    uint32_t icon_color = card.is_offline ? 0x46586C : (card.is_on ? C_ACCENT : 0x4A6A8C);
    if (strcmp(icon_text, "lightbulb") == 0) {
        _add_lightbulb_icon(ic, icon_color);
    } else {
        lv_obj_t* ic_lbl = lv_label_create(ic);
        lv_label_set_text(ic_lbl, icon_text);
        lv_obj_set_style_text_color(ic_lbl, lv_color_hex(icon_color), 0);
        lv_obj_set_style_text_font(ic_lbl, &lv_font_montserrat_36, 0);
        lv_obj_center(ic_lbl);
    }

    // Device name (right of icon, vertically centered)
    lv_obj_t* lbl = lv_label_create(c);
    lv_label_set_text(lbl, card.label.c_str());
    lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(card.is_offline ? 0x5A6A7C
                                  : (card.is_on ? C_TEXT : C_TEXT2)), 0);
    lv_obj_set_width(lbl, w - 14 - ic_size - 12 - 18);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14 + ic_size + 12, 0);

    // Status dot (top-right): green=on, red=offline, grey=off.
    lv_obj_t* dot = lv_obj_create(c);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(card.is_offline ? C_RED
                                  : (card.is_on ? C_GREEN : 0x3A4A5C)), 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    if (card.is_offline) {
        // Offline: show a "未连接" badge and make the card non-interactive so a
        // tap doesn't pretend to control a device HA can't reach.
        lv_obj_t* off = lv_label_create(c);
        lv_label_set_text(off, "未连接");
        lv_obj_set_style_text_font(off, zh_font_sm(), 0);
        lv_obj_set_style_text_color(off, lv_color_hex(C_RED), 0);
        lv_obj_align(off, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    } else {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        auto* d = new CardData{view, card.entity_id, "toggle", ""};
        _bind_action(c, d);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT_SOFT), LV_STATE_PRESSED);
    }
}

// ─── Sensor card ──────────────────────────────────────────────────────────────
static void _build_sensor_card(lv_obj_t* parent, int x, int y, int w, int h,
                                 const DeviceCard& card)
{
    lv_obj_t* c = _make_card(parent, x, y, w, h, C_CARD);
    _add_shadow(c);

    // Room name (top)
    _make_label(c, card.label.c_str(), C_TEXT2, zh_font_lg(),
                LV_ALIGN_TOP_LEFT, 14, 10);

    // Primary value — smaller than title
    const lv_font_t* val_font = card.is_text_value ? (const lv_font_t*)zh_font_sm() : &lv_font_montserrat_28;
    _make_label(c, card.value.empty() ? "--" : card.value.c_str(),
                C_TEXT, val_font,
                LV_ALIGN_BOTTOM_LEFT, 14, -10);

    // Secondary value — right side
    if (!card.value2.empty()) {
        const lv_font_t* val2_font = card.is_text_value ? (const lv_font_t*)zh_font_sm() : &lv_font_montserrat_22;
        _make_label(c, card.value2.c_str(), C_ACCENT,
                    val2_font,
                    LV_ALIGN_BOTTOM_RIGHT, -14, -10);
    }
}

// ─── Lock card ────────────────────────────────────────────────────────────────
// Format ISO timestamp "2024-01-01T14:32:05.000+00:00" -> local "22:32" (UTC+8).
// HA event sensors carry UTC; previous version returned the raw UTC HH:MM which
// was 8 hours behind the user's clock and looked like a stale event.
static std::string _fmt_lock_time(const std::string& iso)
{
    if (iso.size() == 5 && iso[2] == ':') return iso;  // already "HH:MM"
    if (iso.size() < 19) return "";
    int h = 0, m = 0;
    if (sscanf(iso.c_str() + 11, "%2d:%2d", &h, &m) != 2) return "";
    h = (h + 8) % 24;  // UTC → UTC+8 (中国)
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return buf;
}

static void _build_lock_card(lv_obj_t* parent, int x, int y, int w, int h,
                               const DeviceCard& card, HaView* /*view*/)
{
    bool locked = card.is_on;
    lv_obj_t* c = _make_card(parent, x, y, w, h, C_CARD);
    _add_shadow(c);

    int lx   = 14;
    int rpad = 12;
    int row1_y = 10;
    int row2_y = h / 3 + 6;
    int row3_y = h * 2 / 3 + 6;

    // helper: thin divider at given y
    auto _div = [&](int dy) {
        lv_obj_t* d = lv_obj_create(c);
        lv_obj_set_size(d, w - lx - rpad, 1);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x1C3E62), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, lx, dy);
    };

    // ── Row 1: name (left)  +  battery % (right) ─────────────────────────────
    lv_obj_t* name_lbl = lv_label_create(c);
    lv_label_set_text(name_lbl, card.label.c_str());
    lv_obj_set_style_text_font(name_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, lx, row1_y);

    if (!card.battery.empty()) {
        char bat_buf[16];
        snprintf(bat_buf, sizeof(bat_buf), "%s%%", card.battery.c_str());
        lv_obj_t* bat_lbl = lv_label_create(c);
        lv_label_set_text(bat_lbl, bat_buf);
        lv_obj_set_style_text_font(bat_lbl, &lv_font_montserrat_28, 0);
        int bat_val = atoi(card.battery.c_str());
        uint32_t bat_col = bat_val > 50 ? C_GREEN : (bat_val > 20 ? C_AMBER : C_RED);
        lv_obj_set_style_text_color(bat_lbl, lv_color_hex(bat_col), 0);
        lv_obj_align(bat_lbl, LV_ALIGN_TOP_RIGHT, -rpad, row1_y + 2);
    }

    _div(h / 3);

    // ── Row 2: status (left)  +  update time (right) ─────────────────────────
    lv_obj_t* st_lbl = lv_label_create(c);
    lv_label_set_text(st_lbl, locked ? "已锁定" : "已开锁");
    lv_obj_set_style_text_font(st_lbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(st_lbl, lv_color_hex(locked ? C_ACCENT : C_AMBER), 0);
    lv_obj_align(st_lbl, LV_ALIGN_TOP_LEFT, lx, row2_y);

    if (!card.value2.empty()) {
        // Just "HH:MM" — short. card.value2 is the newest event's ISO time
        // (UTC); _fmt_lock_time converts to local UTC+8.
        std::string upd = _fmt_lock_time(card.value2);
        lv_obj_t* upd_lbl = lv_label_create(c);
        lv_label_set_text(upd_lbl, upd.c_str());
        lv_obj_set_style_text_font(upd_lbl, zh_font_sm(), 0);
        lv_obj_set_style_text_color(upd_lbl, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(upd_lbl, LV_ALIGN_TOP_RIGHT, -rpad, row2_y);
    }

    _div(h * 2 / 3);

    // ── Row 3: two most-recent unlock records ─────────────────────────────────
    // Records: value=HH:MM, lock_user=描述; lock_event2=pre-formatted "HH:MM 描述"
    auto _rec_label = [&](int ry, const std::string& txt, bool primary) {
        if (txt.empty()) return;
        lv_obj_t* lbl = lv_label_create(c);
        lv_label_set_text(lbl, txt.c_str());
        lv_obj_set_style_text_font(lbl, zh_font_sm(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(primary ? C_TEXT : C_TEXT2), 0);
        lv_obj_set_width(lbl, w - lx - rpad);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, lx, ry);
    };

    int rec_start = h * 2 / 3 + 6;
    int rec_step  = (h - rec_start - 8) / 2;

    // Record 1: "HH:MM 描述" — card.value is the raw ISO timestamp from
    // _make_lock_card; _fmt_lock_time converts UTC→local and shortens.
    std::string r1;
    if (!card.value.empty())    r1 += _fmt_lock_time(card.value) + "  ";
    if (!card.lock_user.empty()) r1 += card.lock_user;
    _rec_label(rec_start,             r1,               true);
    // Record 2: already pre-formatted "HH:MM 描述"
    _rec_label(rec_start + rec_step,  card.lock_event2, false);
}

// ─── Fish tank combined card ──────────────────────────────────────────────────
// Layout: [icon] 鱼缸           28.5°C    (water temp top-right)
//         ──────────────────────────
//         [电源][水泵][灯]       (3 buttons in a row)
//         ──────────────────────────
//         滤芯              80%   (bar)
// The fish-tank light moved here from the 灯光 tab — see app_ha.cpp deletion
// of LIGHTS[].
static void _build_fishtank_card(lv_obj_t* parent, int x, int y, int w,
                                  const DeviceCard& card, HaView* view)
{
    int pad = 14;
    int ic_size = 44;

    lv_obj_t* c = _make_card(parent, x, y, w, FISHTANK_H, C_CARD);
    _add_shadow(c);

    // Icon circle
    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(card.is_on ? C_ACCENT_SOFT : 0x1A2C42), 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, pad, 12);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* ic_lbl = lv_label_create(ic);
    lv_label_set_text(ic_lbl, LV_SYMBOL_TINT);
    lv_obj_set_style_text_color(ic_lbl, lv_color_hex(card.is_on ? C_ACCENT : C_TEXT2), 0);
    lv_obj_set_style_text_font(ic_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(ic_lbl);

    // Title
    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "鱼缸");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad + ic_size + 12, 18);

    // Water temperature (right)
    if (!card.water_temp.empty()) {
        char tbuf[24];
        snprintf(tbuf, sizeof(tbuf), "%s °C", card.water_temp.c_str());
        lv_obj_t* tl = lv_label_create(c);
        lv_label_set_text(tl, tbuf);
        lv_obj_set_style_text_font(tl, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -pad, 20);
    }

    // Divider 1
    lv_obj_t* dv1 = lv_obj_create(c);
    lv_obj_set_size(dv1, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv1, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv1, 0, 0);
    lv_obj_align(dv1, LV_ALIGN_TOP_LEFT, pad, 68);
    lv_obj_clear_flag(dv1, LV_OBJ_FLAG_SCROLLABLE);

    // Three control buttons: 电源 / 水泵 / 灯
    static const char* FISH_EID       = "switch.xiaomi_cn_931286672_m200_on_p_2_1";
    static const char* PUMP_EID       = "switch.xiaomi_cn_931286672_m200_water_pump_p_2_2";
    static const char* FISH_LIGHT_EID = "light.xiaomi_cn_931286672_m200_s_3_light";
    struct FBtn { const char* name; const char* eid; bool active; };
    FBtn fbtns[3] = {
        {"电源", FISH_EID,       card.is_on},
        {"水泵", PUMP_EID,       card.pump_on},
        {"灯光", FISH_LIGHT_EID, card.fish_light_on},
    };
    int ctrl_y = 78, ctrl_h = 54, gap = 8;
    int ctrl_w = (w - pad * 2 - gap * 2) / 3;
    // 按钮风格参考落地扇：默认 bg 跟卡片底色同色 + 边框，看起来"只有轮廓"；
    // 激活态 bg=C_CARD_ON（高亮填充）。
    auto _border = [](lv_obj_t* b) {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x2A5080), 0);
    };
    for (int i = 0; i < 3; i++) {
        uint32_t btn_bg = fbtns[i].active ? C_CARD_ON : C_CARD;
        uint32_t btn_fg = fbtns[i].active ? 0xFFFFFF : C_TEXT2;
        lv_obj_t* b = _make_card(c, pad + i * (ctrl_w + gap), ctrl_y,
                                  ctrl_w, ctrl_h, btn_bg, 10);
        _border(b);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // Centered name label only — three buttons too narrow for icon + name side-by-side
        lv_obj_t* nlbl = lv_label_create(b);
        lv_label_set_text(nlbl, fbtns[i].name);
        lv_obj_set_style_text_color(nlbl, lv_color_hex(btn_fg), 0);
        lv_obj_set_style_text_font(nlbl, zh_font_sm(), 0);
        lv_obj_center(nlbl);
        auto* d = new CardData{view, fbtns[i].eid, "toggle", ""};
        _bind_action(b, d);
    }

    // Divider 2
    lv_obj_t* dv2 = lv_obj_create(c);
    lv_obj_set_size(dv2, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv2, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv2, 0, 0);
    lv_obj_align(dv2, LV_ALIGN_TOP_LEFT, pad, 142);
    lv_obj_clear_flag(dv2, LV_OBJ_FLAG_SCROLLABLE);

    // Filter life bar
    if (!card.filter_life.empty()) {
        int life_pct = atoi(card.filter_life.c_str());

        lv_obj_t* fl = lv_label_create(c);
        lv_label_set_text(fl, "滤芯");
        lv_obj_set_style_text_font(fl, zh_font_sm(), 0);
        lv_obj_set_style_text_color(fl, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(fl, LV_ALIGN_TOP_LEFT, pad, 154);

        char life_buf[16];
        snprintf(life_buf, sizeof(life_buf), "%d%%", life_pct);
        lv_obj_t* life_lbl = lv_label_create(c);
        lv_label_set_text(life_lbl, life_buf);
        lv_obj_set_style_text_font(life_lbl, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(life_lbl, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(life_lbl, LV_ALIGN_TOP_RIGHT, -pad, 150);

        int bar_x = pad + 52, bar_y = 158;
        int bar_w = w - bar_x - pad - 70, bar_h = 10;
        lv_obj_t* bar_bg = lv_obj_create(c);
        lv_obj_set_pos(bar_bg, bar_x, bar_y);
        lv_obj_set_size(bar_bg, bar_w, bar_h);
        lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x16304C), 0);
        lv_obj_set_style_border_width(bar_bg, 0, 0);
        lv_obj_set_style_radius(bar_bg, 5, 0);
        lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);

        int fill_w = (bar_w * life_pct) / 100;
        if (fill_w > 0) {
            uint32_t bar_color = life_pct > 50 ? C_GREEN
                               : (life_pct > 20 ? C_AMBER : C_RED);
            lv_obj_t* bar_fg = lv_obj_create(c);
            lv_obj_set_pos(bar_fg, bar_x, bar_y);
            lv_obj_set_size(bar_fg, fill_w, bar_h);
            lv_obj_set_style_bg_color(bar_fg, lv_color_hex(bar_color), 0);
            lv_obj_set_style_border_width(bar_fg, 0, 0);
            lv_obj_set_style_radius(bar_fg, 5, 0);
            lv_obj_clear_flag(bar_fg, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

// ─── Vacuum card (扫地机器人) ───────────────────────────────────────────────────
static void _build_vacuum_card(lv_obj_t* parent, int x, int y, int w,
                                const DeviceCard& card, HaView* view)
{
    int pad = 14;
    lv_obj_t* c = _make_card(parent, x, y, w, VACUUM_H, C_CARD);
    _add_shadow(c);

    // Title
    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, card.label.c_str());  // "云鲸逍遥002 Max"
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad, 14);

    // 右上角：充电状态 + 电池百分比
    if (!card.charge_status.empty() || card.vac_battery_pct >= 0) {
        char info[32];
        if (!card.charge_status.empty() && card.vac_battery_pct >= 0)
            snprintf(info, sizeof(info), "%s · %d%%", card.charge_status.c_str(), card.vac_battery_pct);
        else if (card.vac_battery_pct >= 0)
            snprintf(info, sizeof(info), "%d%%", card.vac_battery_pct);
        else
            snprintf(info, sizeof(info), "%s", card.charge_status.c_str());
        lv_obj_t* info_lbl = lv_label_create(c);
        lv_label_set_text(info_lbl, info);
        lv_obj_set_style_text_font(info_lbl, zh_font_sm(), 0);
        uint32_t info_col = (card.charge_status == "已充满" || card.is_on) ? C_GREEN : C_TEXT2;
        lv_obj_set_style_text_color(info_lbl, lv_color_hex(info_col), 0);
        lv_obj_align(info_lbl, LV_ALIGN_TOP_RIGHT, -pad, 18);
    }

    // Divider
    lv_obj_t* dv = lv_obj_create(c);
    lv_obj_set_size(dv, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv, 0, 0);
    lv_obj_align(dv, LV_ALIGN_TOP_LEFT, pad, 62);
    lv_obj_clear_flag(dv, LV_OBJ_FLAG_SCROLLABLE);

    // 4 buttons in 2×2 grid: 开始 / 暂停 / 召回 / 童锁
    // 召回 = vacuum.return_to_base；童锁 = switch.toggle (米家 switch entity)
    struct VBtn { const char* name; const char* action; bool active; };
    VBtn vbtns[4] = {
        {"开始", "vac_start",      card.is_on},
        {"暂停", "vac_pause",      card.value == "已暂停"},
        {"召回", "vac_dock",       false},  // 一次性动作，无 active 状态
        {"童锁", "vac_child_lock", card.child_lock_on},
    };
    int gap = 8, ctrl_h = 54;
    int ctrl_y0 = 78, ctrl_y1 = ctrl_y0 + ctrl_h + gap;
    int ctrl_w = (w - pad * 2 - gap) / 2;
    // 按钮风格参考落地扇：默认只有边框（bg 跟卡片底色同色），激活时填充高亮
    auto _border = [](lv_obj_t* b) {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x2A5080), 0);
    };
    for (int i = 0; i < 4; i++) {
        int row = i / 2, col = i % 2;
        int by  = (row == 0) ? ctrl_y0 : ctrl_y1;
        uint32_t btn_bg = vbtns[i].active ? C_CARD_ON : C_CARD;
        uint32_t btn_fg = vbtns[i].active ? 0xFFFFFF : C_TEXT2;
        lv_obj_t* b = _make_card(c, pad + col * (ctrl_w + gap), by,
                                  ctrl_w, ctrl_h, btn_bg, 10);
        _border(b);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* nlbl = lv_label_create(b);
        lv_label_set_text(nlbl, vbtns[i].name);
        lv_obj_set_style_text_color(nlbl, lv_color_hex(btn_fg), 0);
        lv_obj_set_style_text_font(nlbl, zh_font_sm(), 0);
        lv_obj_center(nlbl);
        auto* d = new CardData{view, card.entity_id, vbtns[i].action, ""};
        _bind_action(b, d);
    }
}

// ─── Washer card (洗衣机, read-only status) ──────────────────────────────────────
static void _build_washer_card(lv_obj_t* parent, int x, int y, int w,
                                const DeviceCard& card, HaView* /*view*/)
{
    int pad = 14;
    lv_obj_t* c = _make_card(parent, x, y, w, WASHER_H, C_CARD);
    _add_shadow(c);

    // Title
    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "洗衣机");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad, 14);

    // Divider
    lv_obj_t* dv = lv_obj_create(c);
    lv_obj_set_size(dv, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv, 0, 0);
    lv_obj_align(dv, LV_ALIGN_TOP_LEFT, pad, 50);
    lv_obj_clear_flag(dv, LV_OBJ_FLAG_SCROLLABLE);

    if (card.is_offline) {
        lv_obj_t* off = lv_label_create(c);
        lv_label_set_text(off, "未连接");
        lv_obj_set_style_text_font(off, zh_font_lg(), 0);
        lv_obj_set_style_text_color(off, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(off, LV_ALIGN_CENTER, 0, 20);
        return;
    }

    // Running state (big)
    lv_obj_t* state = lv_label_create(c);
    lv_label_set_text(state, card.value.empty() ? "待机" : card.value.c_str());
    lv_obj_set_style_text_font(state, zh_font_lg(), 0);
    lv_obj_set_style_text_color(state, lv_color_hex(C_ACCENT), 0);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, pad, 66);

    // Secondary line (remaining time / program)
    if (!card.value2.empty()) {
        lv_obj_t* sub = lv_label_create(c);
        lv_label_set_text(sub, card.value2.c_str());
        lv_obj_set_style_text_font(sub, zh_font_sm(), 0);
        lv_obj_set_style_text_color(sub, lv_color_hex(C_TEXT2), 0);
        lv_obj_set_width(sub, w - pad * 2);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, pad, 116);
    }
}

// ─── Printer card (拓竹 X1C, detailed status) ────────────────────────────────────
static void _build_printer_card(lv_obj_t* parent, int x, int y, int w,
                                 const DeviceCard& card)
{
    int pad = 14;
    lv_obj_t* c = _make_card(parent, x, y, w, PRINTER_H, C_CARD);
    _add_shadow(c);

    // Title
    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "拓竹3D打印机-X1CC");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad, 14);

    // Status (top-right, colored)
    lv_obj_t* st = lv_label_create(c);
    lv_label_set_text(st, card.value.empty() ? "--" : card.value.c_str());
    lv_obj_set_style_text_font(st, zh_font_lg(), 0);
    lv_obj_set_style_text_color(st, lv_color_hex(card.is_offline ? C_RED : C_ACCENT), 0);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -pad, 14);

    // Divider
    lv_obj_t* dv = lv_obj_create(c);
    lv_obj_set_size(dv, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv, 0, 0);
    lv_obj_align(dv, LV_ALIGN_TOP_LEFT, pad, 50);
    lv_obj_clear_flag(dv, LV_OBJ_FLAG_SCROLLABLE);

    if (card.is_offline) {
        lv_obj_t* off = lv_label_create(c);
        lv_label_set_text(off, "未连接");
        lv_obj_set_style_text_font(off, zh_font_lg(), 0);
        lv_obj_set_style_text_color(off, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(off, LV_ALIGN_CENTER, 0, 20);
        return;
    }

    // Progress bar + percentage
    int bar_w = w - pad * 2 - 80;
    lv_obj_t* bar = lv_bar_create(c);
    lv_obj_set_size(bar, bar_w, 16);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, pad, 70);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A3A5C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 8, LV_PART_INDICATOR);
    lv_bar_set_value(bar, card.percentage, LV_ANIM_OFF);

    lv_obj_t* pct = lv_label_create(c);
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d%%", card.percentage);
    lv_label_set_text(pct, pbuf);
    lv_obj_set_style_text_font(pct, zh_font_sm(), 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(C_TEXT), 0);
    lv_obj_align(pct, LV_ALIGN_TOP_RIGHT, -pad, 68);

    // Multi-line detail (temps / layer / remaining)
    if (!card.value2.empty()) {
        lv_obj_t* det = lv_label_create(c);
        lv_label_set_text(det, card.value2.c_str());
        lv_obj_set_style_text_font(det, zh_font_sm(), 0);
        lv_obj_set_style_text_color(det, lv_color_hex(C_TEXT2), 0);
        lv_obj_set_style_text_line_space(det, 8, 0);
        lv_obj_set_width(det, w - pad * 2);
        lv_label_set_long_mode(det, LV_LABEL_LONG_WRAP);
        lv_obj_align(det, LV_ALIGN_TOP_LEFT, pad, 104);
    }
}

// ─── Lenovo L100DW printer (compact status + cartridge) ──────────────────────
// HA only exposes 2 entities for this printer: sensor.lenovo_l100dw
// (device_class=enum, state: idle/printing/stopped) and
// sensor.lenovo_l100dw_black_cartridge (single black toner %). No colors.
// Compact 1-row layout sized to match sensor cards.
static void _build_lenovo_printer_card(lv_obj_t* parent, int x, int y, int w,
                                        const DeviceCard& card)
{
    int pad = 14;
    lv_obj_t* c = _make_card(parent, x, y, w, CARD_H, C_CARD);
    _add_shadow(c);

    // Title
    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "Lenovo L100DW");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad, 14);

    // Status (right) — card.value already holds the Chinese label from app_ha.cpp
    if (card.is_offline) {
        lv_obj_t* off = lv_label_create(c);
        lv_label_set_text(off, "未连接");
        lv_obj_set_style_text_font(off, zh_font_lg(), 0);
        lv_obj_set_style_text_color(off, lv_color_hex(C_RED), 0);
        lv_obj_align(off, LV_ALIGN_TOP_RIGHT, -pad, 14);
        return;
    }

    lv_obj_t* st = lv_label_create(c);
    lv_label_set_text(st, card.value.c_str());
    lv_obj_set_style_text_font(st, zh_font_lg(), 0);
    lv_obj_set_style_text_color(st, lv_color_hex(C_ACCENT), 0);
    lv_obj_align(st, LV_ALIGN_TOP_RIGHT, -pad, 14);

    // Divider
    lv_obj_t* dv = lv_obj_create(c);
    lv_obj_set_size(dv, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv, 0, 0);
    lv_obj_align(dv, LV_ALIGN_TOP_LEFT, pad, 50);
    lv_obj_clear_flag(dv, LV_OBJ_FLAG_SCROLLABLE);

    // Cartridge percent (right) — same line as the (removed) "黑色墨盒" label
    // would have been, so it sits clearly above the progress bar.
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d%%", card.cartridge_pct);
    lv_obj_t* pct = lv_label_create(c);
    lv_label_set_text(pct, pbuf);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_18, 0);
    uint32_t pct_col = card.cartridge_pct > 50 ? C_GREEN
                     : (card.cartridge_pct > 20 ? C_AMBER : C_RED);
    lv_obj_set_style_text_color(pct, lv_color_hex(pct_col), 0);
    lv_obj_align(pct, LV_ALIGN_TOP_RIGHT, -pad, 66);

    // Progress bar (full width, below pct). Removed the "黑色墨盒" label —
    // cbin 最小字号是 20px，用户嫌大；纯 pct + bar 更简洁。
    int bar_x = pad, bar_y = 86;
    int bar_w = w - pad * 2, bar_h = 8;
    lv_obj_t* bar_bg = lv_obj_create(c);
    lv_obj_set_pos(bar_bg, bar_x, bar_y);
    lv_obj_set_size(bar_bg, bar_w, bar_h);
    lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x16304C), 0);
    lv_obj_set_style_border_width(bar_bg, 0, 0);
    lv_obj_set_style_radius(bar_bg, 5, 0);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);

    if (card.cartridge_pct > 0) {
        int fill_w = (bar_w * card.cartridge_pct) / 100;
        lv_obj_t* bar_fg = lv_obj_create(c);
        lv_obj_set_pos(bar_fg, bar_x, bar_y);
        lv_obj_set_size(bar_fg, fill_w, bar_h);
        lv_obj_set_style_bg_color(bar_fg, lv_color_hex(pct_col), 0);
        lv_obj_set_style_border_width(bar_fg, 0, 0);
        lv_obj_set_style_radius(bar_fg, 5, 0);
        lv_obj_clear_flag(bar_fg, LV_OBJ_FLAG_SCROLLABLE);
    }
}

// ─── Fan card ─────────────────────────────────────────────────────────────────
static void _build_fan_card(lv_obj_t* parent, int x, int y, int w,
                              const DeviceCard& card, HaView* view)
{
    lv_obj_t* c = _make_card(parent, x, y, w, FAN_H, C_CARD);
    _add_shadow(c);

    // Title row (no separate power button — power lives in right column row 0)
    lv_obj_t* name_lbl = lv_label_create(c);
    lv_label_set_text(name_lbl, card.label.c_str());
    lv_obj_set_style_text_font(name_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 14, 12);

    // Two-column layout: left = gear (1-4档), right = power + 自然风 + 摇头
    static const struct { const char* lbl; int pct; } GEARS[4] = {
        {"1档", 25}, {"2档", 50}, {"3档", 75}, {"4档", 100}
    };

    int btn_h   = 80;
    int btn_gap = 10;
    int col_gap = 12;
    int col_w   = (w - 2 * PAD - col_gap) / 2;
    int left_x  = PAD;
    int right_x = PAD + col_w + col_gap;
    int start_y = 60;  // below title

    // ── Left column: gear buttons ─────────────────────────────────────────────
    int active = -1;
    for (int i = 0; i < 4; i++)
        if (card.percentage >= GEARS[i].pct - 12) active = i;

    auto _fan_btn_border = [](lv_obj_t* b) {
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x2A5080), 0);
    };
    for (int i = 0; i < 4; i++) {
        bool hi  = (i == active) && card.is_on;
        uint32_t bbg = hi ? C_CARD_ON : 0x16304C;
        int by = start_y + i * (btn_h + btn_gap);
        lv_obj_t* b = _make_card(c, left_x, by, col_w, btn_h, bbg, 10);
        _fan_btn_border(b);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* bl = lv_label_create(b);
        lv_label_set_text(bl, GEARS[i].lbl);
        lv_obj_set_style_text_font(bl, zh_font_lg(), 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(hi ? 0xFFFFFF : C_TEXT2), 0);
        lv_obj_center(bl);
        auto* dg = new CardData{view, card.entity_id, "set_percentage",
                                 std::to_string(GEARS[i].pct)};
        _bind_action(b, dg);
    }

    // ── Right column row 0: power toggle ─────────────────────────────────────
    {
        uint32_t pbg = card.is_on ? C_CARD_ON : 0x16304C;
        lv_obj_t* pw = _make_card(c, right_x, start_y, col_w, btn_h, pbg, 10);
        _fan_btn_border(pw);
        lv_obj_add_flag(pw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* pl = lv_label_create(pw);
        // Show current state (not the action): running → highlighted "ON".
        lv_label_set_text(pl, card.is_on ? "ON" : "OFF");
        lv_obj_set_style_text_font(pl, zh_font_lg(), 0);
        lv_obj_set_style_text_color(pl, lv_color_hex(card.is_on ? 0xFFFFFF : C_TEXT2), 0);
        lv_obj_center(pl);
        auto* dpower = new CardData{view, card.entity_id, "toggle", ""};
        _bind_action(pw, dpower);
    }

    // ── Right column row 1: 自然风 ────────────────────────────────────────────
    {
        bool hi = (card.preset_mode == "\xe8\x87\xaa\xe7\x84\xb6\xe9\xa3\x8e");  // 自然风
        uint32_t mbg = hi ? C_CARD_ON : 0x16304C;
        int by = start_y + (btn_h + btn_gap);
        lv_obj_t* mb = _make_card(c, right_x, by, col_w, btn_h, mbg, 10);
        _fan_btn_border(mb);
        lv_obj_add_flag(mb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* ml = lv_label_create(mb);
        lv_label_set_text(ml, "自然风");
        lv_obj_set_style_text_font(ml, zh_font_lg(), 0);
        lv_obj_set_style_text_color(ml, lv_color_hex(hi ? 0xFFFFFF : C_TEXT2), 0);
        lv_obj_center(ml);
        // Toggle: if already in 自然风, switch back to 直吹风
        const char* target_mode = hi
            ? "\xe7\x9b\xb4\xe5\x90\xb9\xe9\xa3\x8e"   // 直吹风
            : "\xe8\x87\xaa\xe7\x84\xb6\xe9\xa3\x8e";  // 自然风
        auto* dm = new CardData{view, card.entity_id, "set_preset_mode", target_mode};
        _bind_action(mb, dm);
    }

    // ── Right column row 2: oscillation ──────────────────────────────────────
    {
        uint32_t osc_bg = card.oscillating ? C_CARD_ON : 0x16304C;
        int osc_y = start_y + 2 * (btn_h + btn_gap);
        lv_obj_t* ob = _make_card(c, right_x, osc_y, col_w, btn_h, osc_bg, 10);
        _fan_btn_border(ob);
        lv_obj_add_flag(ob, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* ol = lv_label_create(ob);
        lv_label_set_text(ol, card.oscillating ? "摇头  ●" : "摇头  ○");
        lv_obj_set_style_text_font(ol, zh_font_lg(), 0);
        lv_obj_set_style_text_color(ol, lv_color_hex(card.oscillating ? 0xFFFFFF : C_TEXT2), 0);
        lv_obj_center(ol);
        std::string new_osc = card.oscillating ? "false" : "true";
        auto* dosc = new CardData{view, card.entity_id, "oscillate", new_osc};
        _bind_action(ob, dosc);
    }
}

// ─── Sonos card ───────────────────────────────────────────────────────────────
static void _build_sonos_card(lv_obj_t* parent, int x, int y, int w,
                               const DeviceCard& card, HaView* view)
{
    lv_obj_t* c = _make_card(parent, x, y, w, SONOS_H, C_CARD);
    _add_shadow(c);

    bool playing = card.is_on;
    int  pad     = 14;
    int  ref_w   = 44;

    // ── Row 1: icon + title + volume% + refresh (y=0..64) ─────────────────────
    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ref_w, ref_w);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(C_SONOS_SOFT), 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, pad, 12);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ic_lbl = lv_label_create(ic);
    lv_label_set_text(ic_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(ic_lbl, lv_color_hex(C_SONOS_PURP), 0);
    lv_obj_set_style_text_font(ic_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(ic_lbl);

    // Title
    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "SONOS 客厅");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad + ref_w + 12, 18);

    // Volume% — right-aligned to sit between the mute icon and the refresh
    // button. Refresh button occupies the rightmost 38px of the card.
    char vol_buf[16];
    snprintf(vol_buf, sizeof(vol_buf), "%d%%", card.percentage);
    lv_obj_t* vol_lbl = lv_label_create(c);
    lv_label_set_text(vol_lbl, vol_buf);
    lv_obj_set_style_text_font(vol_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(vol_lbl,
        lv_color_hex(card.muted ? C_AMBER : C_TEXT2), 0);
    lv_obj_align(vol_lbl, LV_ALIGN_TOP_RIGHT, -88, 20);

    // Mute indicator (only when muted) — placed left of vol%
    if (card.muted) {
        lv_obj_t* mi = lv_label_create(c);
        lv_label_set_text(mi, LV_SYMBOL_MUTE);
        lv_obj_set_style_text_color(mi, lv_color_hex(C_AMBER), 0);
        lv_obj_set_style_text_font(mi, &lv_font_montserrat_22, 0);
        lv_obj_align(mi, LV_ALIGN_TOP_RIGHT, -150, 22);
    }

    // Refresh button (top-right corner)
    int btn_r = 38;
    lv_obj_t* refresh = _make_card(c, w - pad - btn_r, 14,
                                    btn_r, btn_r, 0x16304C, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(refresh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* refresh_lbl = lv_label_create(refresh);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_style_text_font(refresh_lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(refresh_lbl);
    auto* dref = new CardData{view, "sonos", "sonos_refresh", ""};
    _bind_action(refresh, dref);

    // ── Row 2: state pill + content line + album (y=66..124) ──────────────────
    uint32_t pill_bg = playing ? C_PILL_PLAY : C_PILL_IDLE;
    uint32_t pill_fg = playing ? C_GREEN    : C_TEXT2;
    const char* state_lbl = card.sonos_state.empty()
                            ? (playing ? "播放中" : "已停止")
                            : card.sonos_state.c_str();

    int pill_h   = 26;
    int pill_w   = 70;
    lv_obj_t* pill = _make_card(c, pad, 70, pill_w, pill_h, pill_bg, pill_h / 2);
    lv_obj_t* pill_text = lv_label_create(pill);
    lv_label_set_text(pill_text, state_lbl);
    lv_obj_set_style_text_font(pill_text, zh_font_sm(), 0);
    lv_obj_set_style_text_color(pill_text, lv_color_hex(pill_fg), 0);
    lv_obj_center(pill_text);

    // Content line (single line): "artist · title · album" or "电视输入"
    std::string content;
    if (card.is_tv) {
        content = "电视输入";
    } else {
        if (!card.value2.empty())   content = card.value2;       // artist
        if (!card.battery.empty()) {
            if (!content.empty()) content += " · ";
            content += card.battery;                            // title
        }
        if (!card.lock_user.empty()) {
            if (!content.empty()) content += " · ";
            content += card.lock_user;                          // album
        }
    }
    if (!content.empty()) {
        lv_obj_t* cl = lv_label_create(c);
        lv_label_set_text(cl, content.c_str());
        lv_obj_set_style_text_font(cl, zh_font_sm(), 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_width(cl, w - pad * 2 - pill_w - 12);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_CLIP);
        lv_obj_align(cl, LV_ALIGN_TOP_LEFT, pad + pill_w + 12, 72);
    }

    // ── Divider 1 ─────────────────────────────────────────────────────────────
    lv_obj_t* dv1 = lv_obj_create(c);
    lv_obj_set_size(dv1, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv1, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv1, 0, 0);
    lv_obj_align(dv1, LV_ALIGN_TOP_LEFT, pad, 108);

    // ── Row 3: transport 4 buttons — icon-left label-right
    // 上一首 / 播放 / 暂停 / 下一首
    static const struct { const char* icon; const char* lbl; const char* act; } TRANS[4] = {
        {LV_SYMBOL_PREV,  "上一首", "sonos_prev"},
        {LV_SYMBOL_PLAY,  "播放",   "sonos_play"},
        {LV_SYMBOL_PAUSE, "暂停",   "sonos_pause"},
        {LV_SYMBOL_NEXT,  "下一首", "sonos_next"},
    };
    int trans_y  = 118;
    int trans_h  = 70;
    int trans_gap = 8;
    int trans_w  = (w - pad * 2 - trans_gap * 3) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t* b = _make_card(c,
            pad + i * (trans_w + trans_gap), trans_y,
            trans_w, trans_h, 0x16304C, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        _add_media_button_content(b, TRANS[i].icon, TRANS[i].lbl, C_TEXT2);
        auto* d = new CardData{view, "sonos", TRANS[i].act, ""};
        _bind_action(b, d);
    }

    // ── Divider 2 ─────────────────────────────────────────────────────────────
    lv_obj_t* dv2 = lv_obj_create(c);
    lv_obj_set_size(dv2, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv2, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv2, 0, 0);
    lv_obj_align(dv2, LV_ALIGN_TOP_LEFT, pad, 200);

    // ── Row 4: 4 control buttons — same horizontal style
    // 音量- / 音量+ / 静音 / TV输入
    struct CtrlBtn { const char* icon; const char* lbl; const char* act;
                     uint32_t fg; uint32_t bg; };
    CtrlBtn CTRL[4] = {
        {LV_SYMBOL_VOLUME_MID, "音量-", "sonos_vol_down", C_TEXT2,  0x16304C},
        {LV_SYMBOL_VOLUME_MAX, "音量+", "sonos_vol_up",   C_TEXT2,  0x16304C},
        {LV_SYMBOL_MUTE,       "静音",  "sonos_mute",     0xFFFFFF, C_AMBER },
        {LV_SYMBOL_VIDEO,      "TV输入","sonos_tv",       C_TEXT2,  0x16304C},
    };
    if (!card.muted) {
        CTRL[2].fg = C_TEXT2;
        CTRL[2].bg = 0x16304C;
    }

    int ctrl_y  = 210;
    int ctrl_h  = 70;
    int ctrl_gap = 8;
    int ctrl_w  = (w - pad * 2 - ctrl_gap * 3) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t* b = _make_card(c,
            pad + i * (ctrl_w + ctrl_gap), ctrl_y,
            ctrl_w, ctrl_h, CTRL[i].bg, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        _add_media_button_content(b, CTRL[i].icon, CTRL[i].lbl, CTRL[i].fg);
        auto* d = new CardData{view, "sonos", CTRL[i].act, ""};
        _bind_action(b, d);
    }
}

// ─── TV card ─────────────────────────────────────────────────────────────────
static void _build_tv_card(lv_obj_t* parent, int x, int y, int w,
                           const DeviceCard& card, HaView* view)
{
    lv_obj_t* c = _make_card(parent, x, y, w, TV_H, C_CARD);
    _add_shadow(c);

    int pad = 14;
    int ic_size = 44;

    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(card.is_on ? C_ACCENT_SOFT : 0x1A2C42), 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, pad, 12);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);

    _add_tv_icon(ic, card.is_on ? C_ACCENT : C_TEXT2);

    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "电视");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, pad + ic_size + 12, 18);

    char vol_buf[24];
    snprintf(vol_buf, sizeof(vol_buf), "%d%%", card.percentage);
    lv_obj_t* vol = lv_label_create(c);
    lv_label_set_text(vol, vol_buf);
    lv_obj_set_style_text_font(vol, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(vol, lv_color_hex(card.muted ? C_AMBER : C_TEXT2), 0);
    lv_obj_align(vol, LV_ALIGN_TOP_RIGHT, -pad, 20);

    uint32_t pill_bg = card.is_on ? C_PILL_PLAY : C_PILL_IDLE;
    uint32_t pill_fg = card.is_on ? C_GREEN : C_TEXT2;
    lv_obj_t* pill = _make_card(c, pad, 70, 70, 26, pill_bg, 13);
    lv_obj_t* pill_text = lv_label_create(pill);
    lv_label_set_text(pill_text, card.is_on ? "开" : "关");
    lv_obj_set_style_text_font(pill_text, zh_font_sm(), 0);
    lv_obj_set_style_text_color(pill_text, lv_color_hex(pill_fg), 0);
    lv_obj_center(pill_text);

    // 输入源 + 正在播放（同一行："输入: ATV   CIBN酷喵"）
    std::string source = card.value.empty() ? "未选择输入源" : ("输入: " + card.value);
    if (!card.value2.empty()) {
        source += "   " + card.value2;  // app_name 紧跟输入源
    }
    lv_obj_t* source_lbl = lv_label_create(c);
    lv_label_set_text(source_lbl, source.c_str());
    lv_obj_set_style_text_font(source_lbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(source_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(source_lbl, w - pad * 2 - 90);
    lv_label_set_long_mode(source_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(source_lbl, LV_ALIGN_TOP_LEFT, pad + 82, 72);

    lv_obj_t* dv1 = lv_obj_create(c);
    lv_obj_set_size(dv1, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv1, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv1, 0, 0);
    lv_obj_align(dv1, LV_ALIGN_TOP_LEFT, pad, 104);

    struct TvBtn { const char* icon; const char* lbl; const char* act; const char* value;
                   uint32_t fg; uint32_t bg; };
    TvBtn controls[4] = {
        {"I/O",                card.is_on ? "关" : "开", "tv_power",   "", C_TEXT2, 0x16304C},
        {LV_SYMBOL_VOLUME_MID, "音量-",                  "tv_vol_down", "", C_TEXT2, 0x16304C},
        {LV_SYMBOL_VOLUME_MAX, "音量+",                  "tv_vol_up",   "", C_TEXT2, 0x16304C},
        {LV_SYMBOL_MUTE,       "静音",                 "tv_mute",     card.muted ? "false" : "true",
                                                        card.muted ? 0xFFFFFF : C_TEXT2,
                                                        card.muted ? C_AMBER : 0x16304C},
    };

    int ctrl_y = 114;
    int ctrl_h = 70;
    int gap = 8;
    int ctrl_w = (w - pad * 2 - gap * 3) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t* b = _make_card(c, pad + i * (ctrl_w + gap), ctrl_y,
                                 ctrl_w, ctrl_h, controls[i].bg, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        _add_media_button_content(b, controls[i].icon, controls[i].lbl, controls[i].fg);
        auto* d = new CardData{view, card.entity_id, controls[i].act, controls[i].value};
        _bind_action(b, d);
    }

    lv_obj_t* dv2 = lv_obj_create(c);
    lv_obj_set_size(dv2, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv2, lv_color_hex(0x1C3E62), 0);
    lv_obj_set_style_border_width(dv2, 0, 0);
    lv_obj_align(dv2, LV_ALIGN_TOP_LEFT, pad, 194);

    static const char* SOURCES[3] = {"HDMI 1", "HDMI 2", "HDMI 3"};
    int src_y = 204;
    int src_h = 70;
    int src_gap = 8;
    int src_w = (w - pad * 2 - src_gap * 2) / 3;
    for (int i = 0; i < 3; i++) {
        bool active = card.value == SOURCES[i];
        lv_obj_t* b = _make_card(c,
            pad + i * (src_w + src_gap), src_y,
            src_w, src_h, active ? C_CARD_ON : 0x16304C, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        _add_media_button_content(b, LV_SYMBOL_VIDEO, SOURCES[i],
                                  active ? 0xFFFFFF : C_TEXT2);
        auto* d = new CardData{view, card.entity_id, "tv_source", SOURCES[i]};
        _bind_action(b, d);
    }
}

// ─── Build tab content ────────────────────────────────────────────────────────

static void _layout_cards(lv_obj_t* cont, const std::vector<DeviceCard>& cards,
                            HaView* view,
                            const std::vector<DeviceCard>& sensors = {},
                            int cols = 4,
                            int small_card_h = CARD_H,
                            int start_y = 0,
                            int row_gap = GAP)
{
    int avail_w = W - 2 * PAD;
    int card_w  = (avail_w - GAP * (cols - 1)) / cols;
    int y = start_y, col = 0;

    int half_w = (avail_w - GAP) / 2;  // fan half-width

    for (size_t i = 0; i < cards.size(); i++) {
        const auto& card = cards[i];
        if (card.is_sonos) {
            if (col > 0) { y += small_card_h + GAP; col = 0; }
            _build_sonos_card(cont, PAD, y, avail_w, card, view);
            y += SONOS_H + row_gap;
        } else if (card.is_tv_player) {
            if (col > 0) { y += small_card_h + GAP; col = 0; }
            _build_tv_card(cont, PAD, y, avail_w, card, view);
            y += TV_H + row_gap;
        } else if (card.is_fan) {
            if (col > 0) { y += small_card_h + GAP; col = 0; }
            _build_fan_card(cont, PAD, y, half_w, card, view);
            // If next card is a lock, place it side-by-side on the right
            if (i + 1 < cards.size() && cards[i + 1].is_lock) {
                int lock_x = PAD + half_w + GAP;
                int lock_w = avail_w - half_w - GAP;
                _build_lock_card(cont, lock_x, y, lock_w, FAN_H, cards[i + 1], nullptr);
                i++;
            }
            y += FAN_H + GAP;
        } else if (card.is_lock) {
            if (col > 0) { y += small_card_h + GAP; col = 0; }
            _build_lock_card(cont, PAD, y, avail_w, LOCK_H, card, nullptr);
            y += LOCK_H + GAP;
        } else if (card.is_sensor) {
            int x = PAD + col * (card_w + GAP);
            _build_sensor_card(cont, x, y, card_w, small_card_h, card);
            col++;
            if (col >= cols) { col = 0; y += small_card_h + GAP; }
        } else {
            int x = PAD + col * (card_w + GAP);
            _build_device_card(cont, x, y, card_w, small_card_h, card, view);
            col++;
            if (col >= cols) { col = 0; y += small_card_h + GAP; }
        }
    }

    // Sensors section
    if (!sensors.empty()) {
        if (col > 0) { y += small_card_h + GAP; col = 0; }
        y += 4; // small separator gap
        for (size_t i = 0; i < sensors.size(); i++) {
            const auto& s = sensors[i];
            if (s.is_printer) {
                // 拓竹独占左半行；如果下一个是 lenovo，联想占右半（并排）
                if (col > 0) { y += small_card_h + GAP; col = 0; }
                int pw = card_w;
                _build_printer_card(cont, PAD, y, pw, s);
                if (i + 1 < sensors.size() && sensors[i + 1].is_lenovo_printer) {
                    int rx_x = PAD + pw + GAP;
                    _build_lenovo_printer_card(cont, rx_x, y, pw, sensors[i + 1]);
                    i++;  // consume lenovo
                }
                y += PRINTER_H + GAP;
                continue;
            }
            int x = PAD + col * (card_w + GAP);
            if (s.is_sensor) {
                _build_sensor_card(cont, x, y, card_w, small_card_h, s);
            } else {
                // Controllable switch (e.g. 插排) mixed into the grid.
                _build_device_card(cont, x, y, card_w, small_card_h, s, view);
            }
            col++;
            if (col >= cols) { col = 0; y += small_card_h + GAP; }
        }
    }
}

static void _layout_device_cards(lv_obj_t* cont, const std::vector<DeviceCard>& cards,
                                  HaView* view)
{
    int avail_w = W - 2 * PAD;
    int half_w  = (avail_w - GAP) / 2;
    int y = 0;

    const DeviceCard* fan = nullptr;
    std::vector<const DeviceCard*> small;

    for (const auto& card : cards) {
        if      (card.is_fan)                          fan = &card;
        else if (!card.is_fishtank && !card.is_lock)   small.push_back(&card);
        // fishtank and lock are rendered on page 1 instead
    }

    // Fan at top (full width)
    if (fan) {
        _build_fan_card(cont, PAD, y, avail_w, *fan, view);
        y += FAN_H + GAP;
    }

    // Small switches below fan (paired, half_w each)
    int col = 0;
    for (auto* c : small) {
        int x = PAD + col * (half_w + GAP);
        _build_device_card(cont, x, y, half_w, CARD_H, *c, view);
        col++;
        if (col >= 2) { col = 0; y += CARD_H + GAP; }
    }
}

// ─── Device tab (设备): 单页 — sensors grid（含拓竹|联想并排）───────────────────
// 鱼缸 + 门锁 已移至 家电 tab（不再在设备 tab 渲染，避免重复）。
// Was a 2-page horizontal layout; page 0 was 落地扇 (now moved to 家电 tab) and is
// gone, so the device tab collapses to a single vertically scrolling page.
static void _layout_device_tab(lv_obj_t* cont,
                                const std::vector<DeviceCard>& /*kitchen*/,
                                HaView* view,
                                const std::vector<DeviceCard>& sensors,
                                const std::vector<DeviceCard>& /*appliance*/)
{
    static constexpr int SENSOR_COLS = 3;
    if (!sensors.empty()) {
        _layout_cards(cont, {}, view, sensors, SENSOR_COLS, CARD_H);
    }
}

// ─── Appliance tab (家电): 落地扇 (full-width top) / 鱼缸 | 门锁 / 扫地机 | 洗衣机 ──
static void _layout_appliance(lv_obj_t* cont, const std::vector<DeviceCard>& cards,
                               HaView* view)
{
    int avail_w = W - 2 * PAD;
    int half_w  = (avail_w - GAP) / 2;
    int row_h   = FISHTANK_H;            // 2×2 cards share this height

    const DeviceCard* fan      = nullptr;
    const DeviceCard* fishtank = nullptr;
    const DeviceCard* lock     = nullptr;
    const DeviceCard* vacuum   = nullptr;
    const DeviceCard* washer   = nullptr;
    for (const auto& c : cards) {
        if      (c.is_fan)      fan      = &c;
        else if (c.is_fishtank) fishtank = &c;
        else if (c.is_lock)     lock     = &c;
        else if (c.is_vacuum)   vacuum   = &c;
        else if (c.is_washer)   washer   = &c;
    }

    // 落地扇独占顶部左半（宽 half_w、高 FAN_H=430）
    int fan_y = GAP;
    if (fan) _build_fan_card(cont, PAD, fan_y, half_w, *fan, view);

    // 鱼缸 + 门锁 叠放在落地扇右边（两卡总高 210+8+210=428 ≈ FAN_H 430，与落地扇视觉对齐）
    int rx = PAD + half_w + GAP;
    if (fishtank) _build_fishtank_card(cont, rx, fan_y, half_w, *fishtank, view);
    if (lock)     _build_lock_card(cont, rx, fan_y + row_h + GAP, half_w, row_h, *lock, nullptr);

    // 扫地机 + 洗衣机 在落地扇下方（家电 tab 垂直滚动）
    int y0 = GAP + FAN_H + GAP;
    int y1 = y0 + row_h + GAP;
    int lx = PAD;
    if (vacuum)   _build_vacuum_card(cont, lx, y0, half_w, *vacuum, view);
    if (washer)   _build_washer_card(cont, rx, y0, half_w, *washer, view);
}

// ─── Init & skeleton ─────────────────────────────────────────────────────────

uint32_t HaView::_get_millis() const
{
    // GetHAL() resolves to the injected HAL (esp32 or desktop). We avoid
    // including <hal/hal.h> in the header to keep it self-contained.
    return GetHAL()->millis();
}

// Deferred swipe-up-to-exit handler. Named (not a lambda) so the destructor can
// pass the exact same function pointer to lv_async_call_cancel — otherwise a
// second swipe queued just before close would fire after HaView is freed and
// dereference a dangling pointer.
static void _exit_async_cb(void* udata)
{
    auto* view = static_cast<HaView*>(udata);
    if (view->_on_action_fn) view->_on_action_fn("app", "home", "");
}

HaView::~HaView()
{
    // Drop any swipe-up exit callback queued but not yet dispatched, so it can't
    // run against this freed object.
    lv_async_call_cancel(_exit_async_cb, this);
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
}

void HaView::init(ActionCb on_action)
{
    _on_action    = on_action;
    _on_action_fn = on_action;
    _build_skeleton();
}

void HaView::_build_skeleton()
{
    _scr = lv_obj_create(NULL);
    lv_screen_load(_scr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // _build_header(); removed — top status/weather bar dropped
    _build_tab_bar();

    // Content area (scrollable)
    _content_area = lv_obj_create(_scr);
    lv_obj_set_pos(_content_area, 0, CONT_Y);
    lv_obj_set_size(_content_area, W, CONT_H);
    lv_obj_set_style_bg_opa(_content_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_content_area, 0, 0);
    lv_obj_set_style_pad_all(_content_area, 0, 0);
    lv_obj_clear_flag(_content_area, LV_OBJ_FLAG_SCROLLABLE);

    // Swipe-up-to-exit: register a gesture callback directly on the pointer indev.
    // lv_indev_send_event fires on the indev for every gesture regardless of which
    // LVGL object is being touched, so this works even when cards/tabs capture the touch.
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // Defer to avoid re-entrancy: removing from within indev dispatch
                    // silently fails, leaving a dangling callback after HaView is freed.
                    // Cancellable in ~HaView via the named _exit_async_cb pointer.
                    lv_async_call(_exit_async_cb, lv_event_get_user_data(e));
                }
            }, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void HaView::_build_header()
{
    lv_obj_t* hdr = _make_card(_scr, 0, 0, W, HDR_H, 0xFFFFFFFF, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_70, 0);

    // Time (large, left) — digits, use Latin font
    _lbl_time = lv_label_create(hdr);
    lv_label_set_text(_lbl_time, "--:--");
    lv_obj_set_style_text_font(_lbl_time, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_lbl_time, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_lbl_time, LV_ALIGN_LEFT_MID, PAD + 4, 0);

    // Date — to the right of the time, same 36px height as time, black
    _lbl_date = lv_label_create(hdr);
    lv_label_set_text(_lbl_date, "");
    lv_obj_set_style_text_font(_lbl_date, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_lbl_date, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_lbl_date, LV_ALIGN_LEFT_MID, PAD + 4 + 110, 0);

    _lbl_temp = lv_label_create(hdr);
    lv_label_set_text(_lbl_temp, "--°C");
    lv_obj_set_style_text_font(_lbl_temp, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_lbl_temp, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_lbl_temp, LV_ALIGN_LEFT_MID, 500, 0);

    _lbl_weather = lv_label_create(hdr);
    lv_label_set_text(_lbl_weather, "");
    lv_obj_set_style_text_font(_lbl_weather, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_lbl_weather, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_width(_lbl_weather, 360);
    lv_label_set_long_mode(_lbl_weather, LV_LABEL_LONG_CLIP);
    lv_obj_align(_lbl_weather, LV_ALIGN_LEFT_MID, 610, 0);

    _lbl_battery = lv_label_create(hdr);
    lv_label_set_text(_lbl_battery, LV_SYMBOL_BATTERY_EMPTY " --%");
    lv_obj_set_style_text_font(_lbl_battery, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_lbl_battery, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_lbl_battery, LV_ALIGN_RIGHT_MID, -PAD - 4, 0);
}

void HaView::_build_tab_bar()
{
    lv_obj_t* bar = _make_card(_scr, 0, H - TAB_H, W, TAB_H, C_TAB_BG, 0);
    _tab_bar = bar;

    static const char* TAB_LABELS[4] = {"灯光", "设备", "影音", "家电"};
    int tab_w = W / 4;

    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = _make_card(bar, i * tab_w + 8, 8,
                                    tab_w - 16, TAB_H - 16,
                                    i == 0 ? C_TAB_ACTIVE : C_TAB_BG, 14);
        if (i == 0) _add_shadow(btn);
        _tab_btns[i] = btn;

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, TAB_LABELS[i]);
        lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(i == 0 ? C_ACCENT : C_TEXT2), 0);
        lv_obj_center(lbl);

        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        auto* td = new TabData{this, (TabPage)i};
        lv_obj_add_event_cb(btn, _tab_cb, LV_EVENT_CLICKED, td);
        lv_obj_add_event_cb(btn, _tab_data_delete_cb, LV_EVENT_DELETE, td);
    }
}

void HaView::_switch_tab(TabPage tab)
{
    if (_active_tab == tab) return;

    // Update tab button styles
    for (int i = 0; i < 4; i++) {
        bool active = (i == (int)tab);
        lv_obj_set_style_bg_color(_tab_btns[i],
            lv_color_hex(active ? C_TAB_ACTIVE : C_TAB_BG), 0);
        // Shadow only on active pill; remove it when inactive
        if (active) {
            _add_shadow(_tab_btns[i]);
        } else {
            lv_obj_set_style_shadow_width(_tab_btns[i], 0, 0);
        }
        lv_obj_t* lbl = lv_obj_get_child(_tab_btns[i], 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(active ? C_ACCENT : C_TEXT2), 0);
    }

    _active_tab = tab;

    // Destroy old content
    if (_tab_content) {
        lv_obj_delete(_tab_content);
        _tab_content = nullptr;
    }

    // Rebuild synchronously using the last cached card snapshot, so the new
    // tab is visible immediately instead of after the next onRunning tick.
    // _switch_tab is called from an LVGL event handler which holds the LVGL
    // lock, so no LvglLockGuard is needed here.
    _update_tab(_active_tab,
                _living_cache, _kitchen_cache, _media_cache, _sensors_cache,
                _appliance_cache);
}

// ─── Update ───────────────────────────────────────────────────────────────────

static const char* _battery_icon_for_percent(int percentage)
{
    if (percentage >= 88) return LV_SYMBOL_BATTERY_FULL;
    if (percentage >= 63) return LV_SYMBOL_BATTERY_3;
    if (percentage >= 38) return LV_SYMBOL_BATTERY_2;
    if (percentage >= 13) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

void HaView::_update_header(const WeatherInfo& w, const BatteryInfo& battery, bool connected,
                              const std::string& time_str,
                              const std::string& date_str)
{
    if (!_lbl_time) return;  // header removed: skip status updates
    lv_label_set_text(_lbl_time, time_str.c_str());
    lv_label_set_text(_lbl_date, date_str.c_str());

    if (!w.temp.empty()) {
        lv_label_set_text(_lbl_temp, w.temp.c_str());
        std::string wd = w.description;
        if (!w.humidity.empty()) wd += "  " + w.humidity;
        lv_label_set_text(_lbl_weather, wd.c_str());
    }

    char battery_text[32];
    if (battery.percentage >= 0) {
        snprintf(battery_text, sizeof(battery_text), "%s %d%%",
                 _battery_icon_for_percent(battery.percentage), battery.percentage);
    } else {
        snprintf(battery_text, sizeof(battery_text), "%s --%%", LV_SYMBOL_BATTERY_EMPTY);
    }
    lv_label_set_text(_lbl_battery, battery_text);
    uint32_t battery_color = battery.charging ? C_GREEN :
        (battery.percentage < 0 ? C_TEXT2 :
         (battery.percentage > 50 ? C_TEXT2 : (battery.percentage > 20 ? C_AMBER : C_RED)));
    lv_obj_set_style_text_color(_lbl_battery, lv_color_hex(battery_color), 0);
}

void HaView::_update_tab(TabPage tab,
                          const std::vector<DeviceCard>& living,
                          const std::vector<DeviceCard>& kitchen,
                          const std::vector<DeviceCard>& media,
                          const std::vector<DeviceCard>& sensors,
                          const std::vector<DeviceCard>& appliance)
{
    if (_active_tab != tab) return;

    // Reset all indevs before deleting objects to clear dangling scroll_obj/act_obj
    // pointers that cause use-after-free crashes when the user is mid-scroll.
    {
        lv_indev_t* indev = lv_indev_get_next(nullptr);
        while (indev) {
            lv_indev_reset(indev, nullptr);
            indev = lv_indev_get_next(indev);
        }
    }

    // Rebuild content for active tab
    if (_tab_content) {
        lv_obj_delete(_tab_content);
        _tab_content = nullptr;
    }

    _tab_content = lv_obj_create(_content_area);
    lv_obj_remove_style_all(_tab_content);
    lv_obj_set_pos(_tab_content, 0, 0);
    lv_obj_set_size(_tab_content, W, CONT_H);
    lv_obj_set_style_bg_opa(_tab_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tab_content, 0, 0);
    lv_obj_set_style_pad_all(_tab_content, 0, 0);
    lv_obj_set_style_pad_bottom(_tab_content, GAP, 0);
    lv_obj_set_scrollbar_mode(_tab_content, LV_SCROLLBAR_MODE_OFF);

    switch (tab) {
        case TabPage::LIVING: {
            // Distribute all available height evenly across 4 rows (3 cols × 4 rows = 12 cards max).
            // 5 equal gaps (top + 3 between rows + bottom) + 4 card rows = CONT_H
            static constexpr int LIVING_ROWS = 4;
            int living_card_h = (CONT_H - (LIVING_ROWS + 1) * GAP) / LIVING_ROWS;
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_NONE);
            _layout_cards(_tab_content, living, this, {}, 3, living_card_h, GAP);
            break;
        }
        case TabPage::KITCHEN_BATH: {
            // 单页：sensors 3 列 + 鱼缸 | 门锁 下一行，垂直滚动
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_VER);
            _layout_device_tab(_tab_content, kitchen, this, sensors, appliance);
            break;
        }
        case TabPage::MEDIA: {
            // Distribute available height evenly: top gap + Sonos + mid gap + TV + bottom gap
            int media_gap = (CONT_H - SONOS_H - TV_H) / 3;  // = (614-260-258)/3 = 32
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_NONE);
            _layout_cards(_tab_content, media, this, {}, 4, CARD_H, media_gap, media_gap);
            break;
        }
        case TabPage::APPLIANCE: {
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_VER);
            _layout_appliance(_tab_content, appliance, this);
            break;
        }
    }
}

void HaView::update(const std::vector<DeviceCard>& living,
                    const std::vector<DeviceCard>& kitchen,
                    const std::vector<DeviceCard>& media,
                    const std::vector<DeviceCard>& sensors,
                    const std::vector<DeviceCard>& appliance,
                    const WeatherInfo& weather,
                    const BatteryInfo& battery,
                    bool connected,
                    const std::string& time_str,
                    const std::string& date_str)
{
    // Has the data the *active* tab renders actually changed since the last
    // build? Compare before overwriting the cache. This is the key to not
    // destroying/recreating ~30–50 LVGL objects on every frame: a rebuild only
    // happens when something the user can see has changed (or they tapped).
    bool data_changed = false;
    switch (_active_tab) {
        case TabPage::LIVING:
            data_changed = (living != _living_cache);
            break;
        case TabPage::KITCHEN_BATH:
            data_changed = (kitchen != _kitchen_cache) || (sensors != _sensors_cache);
            break;
        case TabPage::MEDIA:
            data_changed = (media != _media_cache);
            break;
        case TabPage::APPLIANCE:
            data_changed = (appliance != _appliance_cache);
            break;
    }

    // Refresh the cache so a tab tap between updates can rebuild synchronously
    // using the latest known data instead of stale or empty content.
    _living_cache    = living;
    _kitchen_cache   = kitchen;
    _media_cache     = media;
    _sensors_cache   = sensors;
    _appliance_cache = appliance;

    // App::Update() already holds the LVGL lock while running Mooncake apps.
    // Taking it again here deadlocks on the desktop simulator because the
    // desktop HAL uses a non-recursive mutex.
    _update_header(weather, battery, connected, time_str, date_str);

    // Rebuild the tab only on first build, an explicit tap (requestRebuild), or
    // a real data change — never on a fixed timer. Each rebuild destroys and
    // recreates ~30–50 LVGL objects, so doing it every frame fragmented PSRAM
    // and opened use-after-free windows. Defer while a touch/scroll is live:
    // pulling the content out mid-gesture both janks and crashes.
    uint32_t now = _get_millis();
    bool should_rebuild = _force_rebuild || _last_tab_rebuild_ms == 0 || data_changed;
    if (should_rebuild && !_any_pointer_active()) {
        _update_tab(_active_tab, living, kitchen, media, sensors, appliance);
        _last_tab_rebuild_ms = now;
        _force_rebuild = false;
    }
}
