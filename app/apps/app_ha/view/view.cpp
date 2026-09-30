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
static constexpr uint32_t C_GREEN       = 0x34A853;
static constexpr uint32_t C_AMBER       = 0xF59E0B;
static constexpr uint32_t C_RED         = 0xEF4444;

// ─── Layout constants ─────────────────────────────────────────────────────────
static constexpr int W       = 1280;
static constexpr int H       = 720;
static constexpr int PAD     = 16;
static constexpr int GAP     = 12;
static constexpr int RADIUS  = 20;

static constexpr int HDR_H   = 80;
static constexpr int CONT_Y  = GAP;
static constexpr int CONT_H  = H - GAP * 2;

static constexpr int CARD_H  = 123;  // standard device card height

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

// ─── Init & skeleton ─────────────────────────────────────────────────────────

uint32_t HaView::_get_millis() const
{
    // GetHAL() resolves to the injected HAL (esp32 or desktop). We avoid
    // including <hal/hal.h> in the header to keep it self-contained.
    return GetHAL()->millis();
}

// Heap context for the deferred swipe-up-to-exit callback: pairs the target
// HaView* with a weak_ptr to its `_alive` flag (see view.h). `view` is only
// dereferenced after confirming `alive` is still true — since HaView::~HaView
// flips the flag to false as its very first statement, "alive" here is a
// sound guarantee that `view` hasn't been (and isn't concurrently being)
// destroyed. This replaces relying on lv_async_call_cancel() in the
// destructor to stop every queued call in time, which was observed in
// production to not always hold (Guru Meditation, Instruction access fault,
// right in this callback, with `view` already dangling).
struct ExitCbCtx {
    HaView* view;
    std::weak_ptr<bool> alive;
};

static void _exit_async_cb(void* udata)
{
    std::unique_ptr<ExitCbCtx> ctx(static_cast<ExitCbCtx*>(udata));
    auto alive = ctx->alive.lock();
    if (!alive || !*alive) return;  // HaView already destroyed — nothing to do

    HaView* view = ctx->view;
    if (!view->_on_action_fn) return;
    auto cb = view->_on_action_fn;
    cb("app", "home", "");
}

HaView::~HaView()
{
    // Signal any in-flight/queued _exit_async_cb that this instance is gone.
    // Must be first: everything below (deleting _scr etc.) can itself pump
    // the LVGL event loop and re-enter async callbacks.
    *_alive = false;
    if (_gesture_indev) {
        // Best-effort: removal isn't guaranteed to beat an event already in
        // flight (see the alive-check in the gesture callback in
        // _build_skeleton, which is what actually makes that case safe).
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // _gesture_ctx is intentionally not freed here — see its allocation site.
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
            // user_data is a heap ExitCbCtx (view + weak_ptr<bool> alive), not a raw
            // `this`: lv_indev_remove_event_cb_with_user_data() in the destructor was
            // observed in production to not always stop this callback before HaView
            // is freed (indev is a persistent global object outliving any one HaView
            // instance). Checking `alive` before ever touching `ctx->view` makes a
            // stale callback firing after destruction a safe no-op instead of a crash
            // (Guru Meditation, Store address misaligned, right on this line).
            // `_gesture_ctx` is intentionally never freed (see ~HaView): a stale
            // callback might still hold this exact pointer, so freeing it would just
            // trade a HaView-sized UAF for an ExitCbCtx-sized one. It's ~24 bytes,
            // leaked once per HA-panel open — not worth chasing.
            _gesture_ctx = new ExitCbCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<ExitCbCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;  // HaView already destroyed
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // Defer to avoid re-entrancy: removing from within indev dispatch
                    // silently fails. Reuses the same ctx contents for the async call.
                    lv_async_call(_exit_async_cb, new ExitCbCtx{ctx->view, ctx->alive});
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
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

void HaView::_update_lights(const std::vector<DeviceCard>& living)
{
    if (_tab_content) lv_obj_delete(_tab_content);

    _tab_content = lv_obj_create(_content_area);
    lv_obj_remove_style_all(_tab_content);
    lv_obj_set_size(_tab_content, W, CONT_H);
    lv_obj_set_scroll_dir(_tab_content, LV_DIR_NONE);

    static constexpr int COLS = 3;
    static constexpr int ROWS = 3;
    int card_w = (W - PAD * 2 - GAP * (COLS - 1)) / COLS;
    int card_h = (CONT_H - GAP * (ROWS + 1)) / ROWS;
    for (size_t i = 0; i < living.size(); ++i) {
        int col = i % COLS;
        int row = i / COLS;
        _build_device_card(_tab_content,
                           PAD + col * (card_w + GAP),
                           GAP + row * (card_h + GAP),
                           card_w, card_h, living[i], this);
    }
}

void HaView::update(const std::vector<DeviceCard>& living,
                    const WeatherInfo& weather,
                    const BatteryInfo& battery,
                    bool connected,
                    const std::string& time_str,
                    const std::string& date_str)
{
    bool data_changed = living != _living_cache;
    _living_cache = living;
    _update_header(weather, battery, connected, time_str, date_str);

    if ((_force_rebuild || _last_tab_rebuild_ms == 0 || data_changed) &&
        !_any_pointer_active()) {
        _update_lights(living);
        _last_tab_rebuild_ms = _get_millis();
        _force_rebuild = false;
    }
}
