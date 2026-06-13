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

LV_FONT_DECLARE(font_zh_18);
LV_FONT_DECLARE(font_zh_36);

// ─── Colour palette ───────────────────────────────────────────────────────────
static constexpr uint32_t C_BG          = 0xC8DFEF;  // light blue background
static constexpr uint32_t C_CARD        = 0xF0F7FC;  // card background
static constexpr uint32_t C_CARD_ON     = 0xFFFFFF;  // active card
static constexpr uint32_t C_ACCENT      = 0x3B9EDB;  // blue accent
static constexpr uint32_t C_ACCENT_SOFT = 0xD0EAFB;  // soft blue fill
static constexpr uint32_t C_TEXT        = 0x1A2535;  // primary text
static constexpr uint32_t C_TEXT2       = 0x6B7A8D;  // secondary text
static constexpr uint32_t C_TAB_BG      = 0xEAF3FA;  // tab bar bg
static constexpr uint32_t C_TAB_ACTIVE  = 0xFFFFFF;  // active tab pill
static constexpr uint32_t C_GREEN       = 0x34A853;
static constexpr uint32_t C_AMBER       = 0xF59E0B;
static constexpr uint32_t C_RED         = 0xEF4444;
static constexpr uint32_t C_FAN_ACTIVE  = 0x3B9EDB;
static constexpr uint32_t C_SONOS_PURP  = 0x7C3AED;  // Sonos brand purple
static constexpr uint32_t C_SONOS_SOFT  = 0xEDE7F6;  // soft purple fill
static constexpr uint32_t C_PILL_PLAY   = 0xDCFCE7;  // playing pill background
static constexpr uint32_t C_PILL_IDLE   = 0xE8EDF2;  // paused/stopped pill background

// ─── Layout constants ─────────────────────────────────────────────────────────
static constexpr int W       = 1280;
static constexpr int H       = 720;
static constexpr int PAD     = 16;
static constexpr int GAP     = 12;
static constexpr int RADIUS  = 20;

static constexpr int HDR_H   = 80;   // header strip
static constexpr int TAB_H   = 68;   // bottom tab bar
static constexpr int CONT_Y  = HDR_H + GAP;
static constexpr int CONT_H  = H - HDR_H - TAB_H - GAP * 2;

// Card sizes inside content area
static constexpr int CARD_H  = 123;  // standard device card height
static constexpr int FAN_H   = 260;  // fan card height (4 rows + padding)
static constexpr int LOCK_H  = 180;  // smart lock card height
static constexpr int SONOS_H    = 260;  // compact sonos card, fits media tab without scroll
static constexpr int TV_H       = 258;  // compact TV card, fits media tab without scroll
static constexpr int FISHTANK_H = 210;  // combined fish tank card

// ─── Forward declarations ────────────────────────────────────────────────────
static lv_obj_t* _make_card(lv_obj_t* parent, int x, int y, int w, int h,
                              uint32_t bg = C_CARD, int radius = RADIUS);
static void _add_shadow(lv_obj_t* obj);

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
    lv_obj_set_style_shadow_color(obj, lv_color_hex(0xA0B8CC), 0);
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
    lv_obj_set_style_text_font(txt, &font_zh_36, 0);
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
    uint32_t bg = card.is_on ? C_CARD_ON : C_CARD;
    lv_obj_t* c = _make_card(parent, x, y, w, h, bg);
    _add_shadow(c);

    // Icon circle (left side, vertically centered)
    int ic_size = 64;
    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(card.is_on ? C_ACCENT_SOFT : 0xE8EDF2), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);

    const char* icon_text = card.icon.empty() ? LV_SYMBOL_HOME : card.icon.c_str();
    uint32_t icon_color = card.is_on ? C_ACCENT : 0x8899AA;
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
    lv_obj_set_style_text_font(lbl, &font_zh_36, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(card.is_on ? C_TEXT : C_TEXT2), 0);
    lv_obj_set_width(lbl, w - 14 - ic_size - 12 - 18);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14 + ic_size + 12, 0);

    // Status dot (top-right)
    lv_obj_t* dot = lv_obj_create(c);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(card.is_on ? C_GREEN : 0xCCCCCC), 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    // Clickable
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    auto* d = new CardData{view, card.entity_id, "toggle", ""};
    _bind_action(c, d);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT_SOFT), LV_STATE_PRESSED);
}

// ─── Sensor card ──────────────────────────────────────────────────────────────
static void _build_sensor_card(lv_obj_t* parent, int x, int y, int w, int h,
                                 const DeviceCard& card)
{
    lv_obj_t* c = _make_card(parent, x, y, w, h, C_CARD);
    _add_shadow(c);

    // Room name (top)
    _make_label(c, card.label.c_str(), C_TEXT2, &font_zh_36,
                LV_ALIGN_TOP_LEFT, 14, 10);

    // Primary value — big (zh font when value is Chinese text)
    const lv_font_t* val_font = card.is_text_value ? (const lv_font_t*)&font_zh_36 : &lv_font_montserrat_44;
    _make_label(c, card.value.empty() ? "--" : card.value.c_str(),
                C_TEXT, val_font,
                LV_ALIGN_BOTTOM_LEFT, 14, -10);

    // Secondary value — right side
    if (!card.value2.empty()) {
        const lv_font_t* val2_font = card.is_text_value ? (const lv_font_t*)&font_zh_18 : &lv_font_montserrat_36;
        _make_label(c, card.value2.c_str(), C_ACCENT,
                    val2_font,
                    LV_ALIGN_BOTTOM_RIGHT, -14, -10);
    }
}

// ─── Lock card ────────────────────────────────────────────────────────────────
// Format ISO timestamp "2024-01-01T14:32:05.000+00:00" -> "14:32"
static std::string _fmt_lock_time(const std::string& iso)
{
    if (iso.size() == 5 && iso[2] == ':') return iso;
    if (iso.size() < 16) return "";
    return iso.substr(11, 5);
}

static void _build_lock_card(lv_obj_t* parent, int x, int y, int w, int h,
                               const DeviceCard& card, HaView* /*view*/)
{
    bool locked = card.is_on;
    lv_obj_t* c = _make_card(parent, x, y, w, h, C_CARD_ON);
    _add_shadow(c);

    // ── Left: big icon circle ─────────────────────────────────────────────────
    int ic_size = 52;
    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(locked ? C_ACCENT_SOFT : 0xFFF3DC), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t* ic_lbl = lv_label_create(ic);
    lv_label_set_text(ic_lbl, locked ? LV_SYMBOL_CLOSE : LV_SYMBOL_OK);
    lv_obj_set_style_text_color(ic_lbl, lv_color_hex(locked ? C_ACCENT : C_AMBER), 0);
    lv_obj_set_style_text_font(ic_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(ic_lbl);

    int lx   = 14 + ic_size + 12;
    int rpad = 12;
    int row1_y = 10;
    int row2_y = h / 3 + 6;
    int row3_y = h * 2 / 3 + 6;

    // helper: thin divider at given y
    auto _div = [&](int dy) {
        lv_obj_t* d = lv_obj_create(c);
        lv_obj_set_size(d, w - lx - rpad, 1);
        lv_obj_set_style_bg_color(d, lv_color_hex(0xD0E8F4), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, lx, dy);
    };

    // ── Row 1: name (left)  +  battery % (right) ─────────────────────────────
    lv_obj_t* name_lbl = lv_label_create(c);
    lv_label_set_text(name_lbl, card.label.c_str());
    lv_obj_set_style_text_font(name_lbl, &font_zh_36, 0);
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
    lv_obj_set_style_text_font(st_lbl, &font_zh_18, 0);
    lv_obj_set_style_text_color(st_lbl, lv_color_hex(locked ? C_ACCENT : C_AMBER), 0);
    lv_obj_align(st_lbl, LV_ALIGN_TOP_LEFT, lx, row2_y);

    if (!card.value2.empty()) {
        std::string upd = "更新: " + _fmt_lock_time(card.value2);
        lv_obj_t* upd_lbl = lv_label_create(c);
        lv_label_set_text(upd_lbl, upd.c_str());
        lv_obj_set_style_text_font(upd_lbl, &font_zh_18, 0);
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
        lv_obj_set_style_text_font(lbl, &font_zh_18, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(primary ? C_TEXT : C_TEXT2), 0);
        lv_obj_set_width(lbl, w - lx - rpad);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, lx, ry);
    };

    int rec_start = h * 2 / 3 + 6;
    int rec_step  = (h - rec_start - 8) / 2;

    // Record 1: "HH:MM 描述"
    std::string r1;
    if (!card.value.empty())    r1 += card.value + "  ";
    if (!card.lock_user.empty()) r1 += card.lock_user;
    _rec_label(rec_start,             r1,               true);
    // Record 2: already pre-formatted "HH:MM 描述"
    _rec_label(rec_start + rec_step,  card.lock_event2, false);
}

// ─── Fish tank combined card ──────────────────────────────────────────────────
static void _build_fishtank_card(lv_obj_t* parent, int x, int y, int w,
                                  const DeviceCard& card, HaView* view)
{
    int pad = 14;
    int ic_size = 44;

    lv_obj_t* c = _make_card(parent, x, y, w, FISHTANK_H, C_CARD_ON);
    _add_shadow(c);

    // Icon circle
    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(card.is_on ? C_ACCENT_SOFT : 0xE8EDF2), 0);
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
    lv_obj_set_style_text_font(title, &font_zh_36, 0);
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
    lv_obj_set_style_bg_color(dv1, lv_color_hex(0xD0E8F4), 0);
    lv_obj_set_style_border_width(dv1, 0, 0);
    lv_obj_align(dv1, LV_ALIGN_TOP_LEFT, pad, 68);
    lv_obj_clear_flag(dv1, LV_OBJ_FLAG_SCROLLABLE);

    // Control buttons: 电源 + 水泵
    static const char* FISH_EID  = "switch.xiaomi_cn_931286672_m200_on_p_2_1";
    static const char* PUMP_EID  = "switch.xiaomi_cn_931286672_m200_water_pump_p_2_2";
    struct FBtn { const char* icon; const char* lbl; const char* eid; bool active; };
    FBtn fbtns[2] = {
        {"I/O",           card.is_on   ? "关" : "开", FISH_EID, card.is_on},
        {LV_SYMBOL_POWER, card.pump_on ? "关" : "开", PUMP_EID, card.pump_on},
    };
    // Override first button label to show name + state
    int ctrl_y = 78, ctrl_h = 54, gap = 8;
    int ctrl_w = (w - pad * 2 - gap) / 2;
    for (int i = 0; i < 2; i++) {
        uint32_t btn_bg = fbtns[i].active ? C_ACCENT : 0xDDE8F2;
        uint32_t btn_fg = fbtns[i].active ? 0xFFFFFF : C_TEXT2;
        lv_obj_t* b = _make_card(c, pad + i * (ctrl_w + gap), ctrl_y,
                                  ctrl_w, ctrl_h, btn_bg, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        const char* name = (i == 0) ? "电源" : "水泵";
        // icon label (montserrat, top)
        lv_obj_t* ico = lv_label_create(b);
        lv_label_set_text(ico, fbtns[i].icon);
        lv_obj_set_style_text_color(ico, lv_color_hex(btn_fg), 0);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_18, 0);
        lv_obj_align(ico, LV_ALIGN_CENTER, -20, 0);
        // name label (zh font)
        lv_obj_t* nlbl = lv_label_create(b);
        lv_label_set_text(nlbl, name);
        lv_obj_set_style_text_color(nlbl, lv_color_hex(btn_fg), 0);
        lv_obj_set_style_text_font(nlbl, &font_zh_18, 0);
        lv_obj_align(nlbl, LV_ALIGN_CENTER, 10, 0);
        auto* d = new CardData{view, fbtns[i].eid, "toggle", ""};
        _bind_action(b, d);
    }

    // Divider 2
    lv_obj_t* dv2 = lv_obj_create(c);
    lv_obj_set_size(dv2, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv2, lv_color_hex(0xD0E8F4), 0);
    lv_obj_set_style_border_width(dv2, 0, 0);
    lv_obj_align(dv2, LV_ALIGN_TOP_LEFT, pad, 142);
    lv_obj_clear_flag(dv2, LV_OBJ_FLAG_SCROLLABLE);

    // Filter life bar
    if (!card.filter_life.empty()) {
        int life_pct = atoi(card.filter_life.c_str());

        lv_obj_t* fl = lv_label_create(c);
        lv_label_set_text(fl, "滤芯");
        lv_obj_set_style_text_font(fl, &font_zh_18, 0);
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
        lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0xDDE8F2), 0);
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

// ─── Fan card ─────────────────────────────────────────────────────────────────
static void _build_fan_card(lv_obj_t* parent, int x, int y, int w,
                              const DeviceCard& card, HaView* view)
{
    lv_obj_t* c = _make_card(parent, x, y, w, FAN_H, C_CARD_ON);
    _add_shadow(c);

    // Title row
    lv_obj_t* name_lbl = lv_label_create(c);
    lv_label_set_text(name_lbl, card.label.c_str());
    lv_obj_set_style_text_font(name_lbl, &font_zh_36, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 14, 12);

    lv_obj_t* power = _make_card(c, w - 130, 10, 112, 46,
                                  card.is_on ? C_ACCENT : 0xDDE8F2, 10);
    lv_obj_add_flag(power, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* power_lbl = lv_label_create(power);
    lv_label_set_text(power_lbl, card.is_on ? "OFF" : "ON");
    lv_obj_set_style_text_font(power_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(power_lbl, lv_color_hex(card.is_on ? 0xFFFFFF : C_TEXT2), 0);
    lv_obj_center(power_lbl);
    auto* dpower = new CardData{view, card.entity_id, "toggle", ""};
    _bind_action(power, dpower);

    // ── Gear buttons ─────────────────────────────────────────────────────────
    static const struct { const char* lbl; int pct; } GEARS[4] = {
        {"1档", 25}, {"2档", 50}, {"3档", 75}, {"4档", 100}
    };
    int btn_gap = 8, gear_y = 60;
    int gear_w  = (w - 2 * PAD - btn_gap * 3) / 4;
    int gear_h  = 54;

    int active = -1;
    for (int i = 0; i < 4; i++)
        if (card.percentage >= GEARS[i].pct - 12) active = i;

    for (int i = 0; i < 4; i++) {
        bool hi  = (i == active) && card.is_on;
        uint32_t bbg = hi ? C_ACCENT : 0xDDE8F2;
        lv_obj_t* b = _make_card(c, PAD + i * (gear_w + btn_gap), gear_y,
                                   gear_w, gear_h, bbg, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* bl = lv_label_create(b);
        lv_label_set_text(bl, GEARS[i].lbl);
        lv_obj_set_style_text_font(bl, &font_zh_36, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(hi ? 0xFFFFFF : C_TEXT2), 0);
        lv_obj_center(bl);
        auto* dg = new CardData{view, card.entity_id, "set_percentage",
                                 std::to_string(GEARS[i].pct)};
        _bind_action(b, dg);
    }

    // ── Mode buttons ──────────────────────────────────────────────────────────
    static const struct { const char* lbl; const char* mode; } MODES[2] = {
        {"直吹风", "normal"}, {"自然风", "natural"}
    };
    int mode_y = gear_y + gear_h + 10;
    int mode_w = (w - 2 * PAD - btn_gap) / 2;
    for (int i = 0; i < 2; i++) {
        bool hi = (card.preset_mode == MODES[i].mode);
        uint32_t mbg = hi ? C_ACCENT : 0xDDE8F2;
        lv_obj_t* mb = _make_card(c, PAD + i * (mode_w + btn_gap), mode_y,
                                    mode_w, gear_h, mbg, 10);
        lv_obj_add_flag(mb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* ml = lv_label_create(mb);
        lv_label_set_text(ml, MODES[i].lbl);
        lv_obj_set_style_text_font(ml, &font_zh_36, 0);
        lv_obj_set_style_text_color(ml, lv_color_hex(hi ? 0xFFFFFF : C_TEXT2), 0);
        lv_obj_center(ml);
        auto* dm = new CardData{view, card.entity_id, "set_preset_mode", MODES[i].mode};
        _bind_action(mb, dm);
    }

    // ── Oscillation toggle ────────────────────────────────────────────────────
    int osc_y = mode_y + gear_h + 10;
    uint32_t osc_bg = card.oscillating ? C_ACCENT : 0xDDE8F2;
    lv_obj_t* ob = _make_card(c, PAD, osc_y, w - 2 * PAD, gear_h, osc_bg, 10);
    lv_obj_add_flag(ob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* ol = lv_label_create(ob);
    lv_label_set_text(ol, card.oscillating ? "摇头  ●" : "摇头  ○");
    lv_obj_set_style_text_font(ol, &font_zh_36, 0);
    lv_obj_set_style_text_color(ol, lv_color_hex(card.oscillating ? 0xFFFFFF : C_TEXT2), 0);
    lv_obj_center(ol);
    std::string new_osc = card.oscillating ? "false" : "true";
    auto* dosc = new CardData{view, card.entity_id, "oscillate", new_osc};
    _bind_action(ob, dosc);
}

// ─── Sonos card ───────────────────────────────────────────────────────────────
static void _build_sonos_card(lv_obj_t* parent, int x, int y, int w,
                               const DeviceCard& card, HaView* view)
{
    lv_obj_t* c = _make_card(parent, x, y, w, SONOS_H, C_CARD_ON);
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
    lv_obj_set_style_text_font(title, &font_zh_36, 0);
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
                                    btn_r, btn_r, 0xDDE8F2, LV_RADIUS_CIRCLE);
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
    lv_obj_set_style_text_font(pill_text, &font_zh_18, 0);
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
        lv_obj_set_style_text_font(cl, &font_zh_18, 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_width(cl, w - pad * 2 - pill_w - 12);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_CLIP);
        lv_obj_align(cl, LV_ALIGN_TOP_LEFT, pad + pill_w + 12, 72);
    }

    // ── Divider 1 ─────────────────────────────────────────────────────────────
    lv_obj_t* dv1 = lv_obj_create(c);
    lv_obj_set_size(dv1, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv1, lv_color_hex(0xD0E8F4), 0);
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
    int trans_h  = 54;
    int trans_gap = 8;
    int trans_w  = (w - pad * 2 - trans_gap * 3) / 4;
    for (int i = 0; i < 4; i++) {
        lv_obj_t* b = _make_card(c,
            pad + i * (trans_w + trans_gap), trans_y,
            trans_w, trans_h, 0xDDE8F2, 10);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        _add_media_button_content(b, TRANS[i].icon, TRANS[i].lbl, C_TEXT2);
        auto* d = new CardData{view, "sonos", TRANS[i].act, ""};
        _bind_action(b, d);
    }

    // ── Divider 2 ─────────────────────────────────────────────────────────────
    lv_obj_t* dv2 = lv_obj_create(c);
    lv_obj_set_size(dv2, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv2, lv_color_hex(0xD0E8F4), 0);
    lv_obj_set_style_border_width(dv2, 0, 0);
    lv_obj_align(dv2, LV_ALIGN_TOP_LEFT, pad, 184);

    // ── Row 4: 4 control buttons — same horizontal style
    // 音量- / 音量+ / 静音 / TV输入
    struct CtrlBtn { const char* icon; const char* lbl; const char* act;
                     uint32_t fg; uint32_t bg; };
    CtrlBtn CTRL[4] = {
        {LV_SYMBOL_VOLUME_MID, "音量-", "sonos_vol_down", C_TEXT2,  0xDDE8F2},
        {LV_SYMBOL_VOLUME_MAX, "音量+", "sonos_vol_up",   C_TEXT2,  0xDDE8F2},
        {LV_SYMBOL_MUTE,       "静音",  "sonos_mute",     0xFFFFFF, C_AMBER },
        {LV_SYMBOL_VIDEO,      "TV输入","sonos_tv",       0xFFFFFF, 0x6366F1},
    };
    if (!card.muted) {
        CTRL[2].fg = C_TEXT2;
        CTRL[2].bg = 0xDDE8F2;
    }

    int ctrl_y  = 194;
    int ctrl_h  = 54;
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
    lv_obj_t* c = _make_card(parent, x, y, w, TV_H, C_CARD_ON);
    _add_shadow(c);

    int pad = 14;
    int ic_size = 44;

    lv_obj_t* ic = lv_obj_create(c);
    lv_obj_set_size(ic, ic_size, ic_size);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(card.is_on ? C_ACCENT_SOFT : 0xE8EDF2), 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, pad, 12);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);

    _add_tv_icon(ic, card.is_on ? C_ACCENT : C_TEXT2);

    lv_obj_t* title = lv_label_create(c);
    lv_label_set_text(title, "电视");
    lv_obj_set_style_text_font(title, &font_zh_36, 0);
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
    lv_obj_set_style_text_font(pill_text, &font_zh_18, 0);
    lv_obj_set_style_text_color(pill_text, lv_color_hex(pill_fg), 0);
    lv_obj_center(pill_text);

    std::string source = card.value.empty() ? "未选择输入源" : ("输入: " + card.value);
    lv_obj_t* source_lbl = lv_label_create(c);
    lv_label_set_text(source_lbl, source.c_str());
    lv_obj_set_style_text_font(source_lbl, &font_zh_18, 0);
    lv_obj_set_style_text_color(source_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(source_lbl, w - pad * 2 - 90);
    lv_label_set_long_mode(source_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(source_lbl, LV_ALIGN_TOP_LEFT, pad + 82, 72);

    lv_obj_t* dv1 = lv_obj_create(c);
    lv_obj_set_size(dv1, w - pad * 2, 1);
    lv_obj_set_style_bg_color(dv1, lv_color_hex(0xD0E8F4), 0);
    lv_obj_set_style_border_width(dv1, 0, 0);
    lv_obj_align(dv1, LV_ALIGN_TOP_LEFT, pad, 104);

    struct TvBtn { const char* icon; const char* lbl; const char* act; const char* value;
                   uint32_t fg; uint32_t bg; };
    TvBtn controls[4] = {
        {"I/O",                card.is_on ? "关" : "开", "tv_power",   "", C_TEXT2, 0xDDE8F2},
        {LV_SYMBOL_VOLUME_MID, "音量-",                  "tv_vol_down", "", C_TEXT2, 0xDDE8F2},
        {LV_SYMBOL_VOLUME_MAX, "音量+",                  "tv_vol_up",   "", C_TEXT2, 0xDDE8F2},
        {LV_SYMBOL_MUTE,       "静音",                 "tv_mute",     card.muted ? "false" : "true",
                                                        card.muted ? 0xFFFFFF : C_TEXT2,
                                                        card.muted ? C_AMBER : 0xDDE8F2},
    };

    int ctrl_y = 114;
    int ctrl_h = 54;
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
    lv_obj_set_style_bg_color(dv2, lv_color_hex(0xD0E8F4), 0);
    lv_obj_set_style_border_width(dv2, 0, 0);
    lv_obj_align(dv2, LV_ALIGN_TOP_LEFT, pad, 178);

    static const char* SOURCES[3] = {"HDMI 1", "HDMI 2", "HDMI 3"};
    int src_y = 188;
    int src_h = 54;
    int src_gap = 8;
    int src_w = (w - pad * 2 - src_gap * 2) / 3;
    for (int i = 0; i < 3; i++) {
        bool active = card.value == SOURCES[i];
        lv_obj_t* b = _make_card(c,
            pad + i * (src_w + src_gap), src_y,
            src_w, src_h, active ? C_ACCENT : 0xDDE8F2, 10);
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
                            int small_card_h = CARD_H)
{
    int avail_w = W - 2 * PAD;
    int card_w  = (avail_w - GAP * (cols - 1)) / cols;
    int y = 0, col = 0;

    int half_w = (avail_w - GAP) / 2;  // fan half-width

    for (size_t i = 0; i < cards.size(); i++) {
        const auto& card = cards[i];
        if (card.is_sonos) {
            if (col > 0) { y += small_card_h + GAP; col = 0; }
            _build_sonos_card(cont, PAD, y, avail_w, card, view);
            y += SONOS_H + GAP;
        } else if (card.is_tv_player) {
            if (col > 0) { y += small_card_h + GAP; col = 0; }
            _build_tv_card(cont, PAD, y, avail_w, card, view);
            y += TV_H + GAP;
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
        for (auto& s : sensors) {
            int x = PAD + col * (card_w + GAP);
            _build_sensor_card(cont, x, y, card_w, small_card_h, s);
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

// ─── Device tab horizontal pages ─────────────────────────────────────────────
static lv_obj_t* _make_page(lv_obj_t* parent, int page_idx)
{
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_pos(p, page_idx * W, 0);
    lv_obj_set_size(p, W, CONT_H);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_pad_bottom(p, GAP, 0);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    return p;
}

static void _layout_device_pages(lv_obj_t* cont,
                                   const std::vector<DeviceCard>& kitchen,
                                   HaView* view,
                                   const std::vector<DeviceCard>& sensors)
{
    // Page 0: fan (top) + small switches
    lv_obj_t* p0 = _make_page(cont, 0);
    _layout_device_cards(p0, kitchen, view);

    // Page 1: sensors + fishtank (half) + lock (half) below sensors
    lv_obj_t* p1 = _make_page(cont, 1);

    static constexpr int SENSOR_COLS = 3;
    if (!sensors.empty()) {
        _layout_cards(p1, {}, view, sensors, SENSOR_COLS, CARD_H);
    }

    // y position below the last sensor row
    // _layout_cards with empty cards: sensors start at y=4, each row = CARD_H+GAP
    int n_sensor_rows = sensors.empty() ? 0
                      : ((int)sensors.size() + SENSOR_COLS - 1) / SENSOR_COLS;
    int below_sensors = sensors.empty() ? 0 : 4 + n_sensor_rows * (CARD_H + GAP);

    const DeviceCard* fishtank = nullptr;
    const DeviceCard* lock     = nullptr;
    for (const auto& c : kitchen) {
        if (c.is_fishtank) fishtank = &c;
        if (c.is_lock)     lock     = &c;
    }
    int avail_w = W - 2 * PAD;
    int half_w  = (avail_w - GAP) / 2;
    if (fishtank) {
        _build_fishtank_card(p1, PAD, below_sensors, half_w, *fishtank, view);
    }
    if (lock) {
        _build_lock_card(p1, PAD + half_w + GAP, below_sensors, half_w, FISHTANK_H, *lock, nullptr);
    }
}

static void _device_page_scroll_cb(lv_event_t* e)
{
    HaView* self = (HaView*)lv_event_get_user_data(e);
    lv_obj_t* cont = lv_event_get_target_obj(e);
    int32_t sx = lv_obj_get_scroll_x(cont);
    int page = (int)((sx + W / 2) / W);
    self->_device_tab_page = page;
    self->_set_dot_page(page);
}

void HaView::_create_page_dots(int n_pages)
{
    static constexpr int DOT_W  = 16;
    static constexpr int DOT_H  = 8;
    static constexpr int DOT_GAP = 8;

    int total_w = n_pages * DOT_W + (n_pages - 1) * DOT_GAP;
    int dot_x   = (W - total_w) / 2;
    int dot_y   = CONT_H - DOT_H - 10;

    _dot_container = lv_obj_create(_content_area);
    lv_obj_set_pos(_dot_container, 0, 0);
    lv_obj_set_size(_dot_container, W, CONT_H);
    lv_obj_set_style_bg_opa(_dot_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_dot_container, 0, 0);
    lv_obj_clear_flag(_dot_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_dot_container, LV_OBJ_FLAG_CLICKABLE);

    _n_page_dots = n_pages;
    for (int i = 0; i < n_pages && i < MAX_PAGE_DOTS; i++) {
        lv_obj_t* dot = lv_obj_create(_dot_container);
        lv_obj_set_size(dot, DOT_W, DOT_H);
        lv_obj_set_pos(dot, dot_x + i * (DOT_W + DOT_GAP), dot_y);
        lv_obj_set_style_radius(dot, DOT_H / 2, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xCCCCCC), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        _page_dots[i] = dot;
    }
}

void HaView::_set_dot_page(int page)
{
    static constexpr int DOT_W_ACTIVE = 24;
    static constexpr int DOT_W_IDLE   = 16;
    static constexpr int DOT_H       = 8;
    static constexpr int DOT_GAP     = 8;

    // Recompute x positions so active (wider) dot doesn't shift siblings
    // Base layout uses DOT_W_IDLE for all; active dot expands in place.
    int total_w = _n_page_dots * DOT_W_IDLE + (_n_page_dots - 1) * DOT_GAP;
    int base_x  = (W - total_w) / 2;

    for (int i = 0; i < _n_page_dots && i < MAX_PAGE_DOTS; i++) {
        if (!_page_dots[i]) continue;
        bool active = (i == page);
        int w = active ? DOT_W_ACTIVE : DOT_W_IDLE;
        int x = base_x + i * (DOT_W_IDLE + DOT_GAP);
        lv_obj_set_size(_page_dots[i], w, DOT_H);
        lv_obj_set_pos(_page_dots[i], x, CONT_H - DOT_H - 10);
        lv_obj_set_style_bg_color(_page_dots[i],
            lv_color_hex(active ? C_ACCENT : 0xBBCCDD), 0);
    }
}

// ─── Init & skeleton ─────────────────────────────────────────────────────────

uint32_t HaView::_get_millis() const
{
    // GetHAL() resolves to the injected HAL (esp32 or desktop). We avoid
    // including <hal/hal.h> in the header to keep it self-contained.
    return GetHAL()->millis();
}

HaView::~HaView()
{
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

    _build_header();
    _build_tab_bar();

    // Invisible swipe-up zone at the bottom — triggers home navigation
    lv_obj_t* swipe_zone = lv_obj_create(_scr);
    lv_obj_set_size(swipe_zone, W, 40);
    lv_obj_set_pos(swipe_zone, 0, H - 40);
    lv_obj_set_style_bg_opa(swipe_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(swipe_zone, 0, 0);
    lv_obj_clear_flag(swipe_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swipe_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swipe_zone, [](lv_event_t* e) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP) {
            auto* view = static_cast<HaView*>(lv_event_get_user_data(e));
            if (view->_on_action_fn) view->_on_action_fn("app", "home", "");
        }
    }, LV_EVENT_GESTURE, this);

    // Content area (scrollable)
    _content_area = lv_obj_create(_scr);
    lv_obj_set_pos(_content_area, 0, CONT_Y);
    lv_obj_set_size(_content_area, W, CONT_H);
    lv_obj_set_style_bg_opa(_content_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_content_area, 0, 0);
    lv_obj_set_style_pad_all(_content_area, 0, 0);
    lv_obj_clear_flag(_content_area, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_set_style_text_font(_lbl_date, &font_zh_36, 0);
    lv_obj_set_style_text_color(_lbl_date, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_lbl_date, LV_ALIGN_LEFT_MID, PAD + 4 + 110, 0);

    _lbl_temp = lv_label_create(hdr);
    lv_label_set_text(_lbl_temp, "--°C");
    lv_obj_set_style_text_font(_lbl_temp, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_lbl_temp, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_lbl_temp, LV_ALIGN_LEFT_MID, 500, 0);

    _lbl_weather = lv_label_create(hdr);
    lv_label_set_text(_lbl_weather, "");
    lv_obj_set_style_text_font(_lbl_weather, &font_zh_36, 0);
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

    static const char* TAB_LABELS[3] = {"灯光", "设备", "影音"};
    int tab_w = W / 3;

    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = _make_card(bar, i * tab_w + 8, 8,
                                    tab_w - 16, TAB_H - 16,
                                    i == 0 ? C_TAB_ACTIVE : C_TAB_BG, 14);
        if (i == 0) _add_shadow(btn);
        _tab_btns[i] = btn;

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, TAB_LABELS[i]);
        lv_obj_set_style_text_font(lbl, &font_zh_36, 0);
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
    for (int i = 0; i < 3; i++) {
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
                _living_cache, _kitchen_cache, _media_cache, _sensors_cache);
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
                          const std::vector<DeviceCard>& sensors)
{
    if (_active_tab != tab) return;

    // Save device-tab page before destroying old content
    if (tab == TabPage::KITCHEN_BATH && _tab_content) {
        int32_t sx = lv_obj_get_scroll_x(_tab_content);
        _device_tab_page = (int)((sx + W / 2) / W);
    }

    // Rebuild content for active tab
    if (_tab_content) {
        lv_obj_delete(_tab_content);
        _tab_content = nullptr;
    }

    // Dots live on _content_area; delete them when leaving device tab
    // (they'll be recreated below when on KITCHEN_BATH)
    if (tab != TabPage::KITCHEN_BATH && _dot_container) {
        lv_obj_delete(_dot_container);
        _dot_container = nullptr;
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
        case TabPage::LIVING:
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_VER);
            _layout_cards(_tab_content, living, this, {}, 3, CARD_H);
            break;
        case TabPage::KITCHEN_BATH: {
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_HOR);
            lv_obj_set_scroll_snap_x(_tab_content, LV_SCROLL_SNAP_START);
            _layout_device_pages(_tab_content, kitchen, this, sensors);
            // Register scroll callback to track current page
            lv_obj_add_event_cb(_tab_content, _device_page_scroll_cb,
                                LV_EVENT_SCROLL, this);
            // Restore page position (force layout first so scroll takes effect)
            lv_obj_update_layout(_tab_content);
            {
                int n_pages = 2;  // p0: fan+switches, p1: sensors+fishtank+lock
                _device_tab_page = std::min(_device_tab_page, n_pages - 1);
                if (_device_tab_page > 0) {
                    lv_obj_scroll_to_x(_tab_content, _device_tab_page * W, LV_ANIM_OFF);
                }
                // Keep dots alive across rebuilds; just restore z-order (last child = on top)
                if (!_dot_container) {
                    _create_page_dots(n_pages);
                } else {
                    lv_obj_move_to_index(_dot_container, -1);
                }
                _set_dot_page(_device_tab_page);
            }
            break;
        }
        case TabPage::MEDIA:
            lv_obj_set_scroll_dir(_tab_content, LV_DIR_VER);
            _layout_cards(_tab_content, media, this);
            break;
    }
}

void HaView::update(const std::vector<DeviceCard>& living,
                    const std::vector<DeviceCard>& kitchen,
                    const std::vector<DeviceCard>& media,
                    const std::vector<DeviceCard>& sensors,
                    const WeatherInfo& weather,
                    const BatteryInfo& battery,
                    bool connected,
                    const std::string& time_str,
                    const std::string& date_str)
{
    // Refresh the cache so a tab tap between updates can rebuild synchronously
    // using the latest known data instead of stale or empty content.
    _living_cache  = living;
    _kitchen_cache = kitchen;
    _media_cache   = media;
    _sensors_cache = sensors;

    // App::Update() already holds the LVGL lock while running Mooncake apps.
    // Taking it again here deadlocks on the desktop simulator because the
    // desktop HAL uses a non-recursive mutex.
    _update_header(weather, battery, connected, time_str, date_str);

    // Tab content rebuilds destroy + recreate ~30–50 LVGL objects; throttled
    // to avoid PSRAM fragmentation. Action callbacks call requestRebuild() to
    // force the next frame so user taps feel immediate.
    uint32_t now = _get_millis();
    bool should_rebuild = _force_rebuild ||
        _last_tab_rebuild_ms == 0 ||
        now - _last_tab_rebuild_ms >= TAB_REBUILD_INTERVAL_MS;
    if (should_rebuild) {
        _update_tab(_active_tab, living, kitchen, media, sensors);
        _last_tab_rebuild_ms = now;
        _force_rebuild = false;
    }
}
