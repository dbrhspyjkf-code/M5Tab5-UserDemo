#include "screensaver.h"
#include "bing_client.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <lvgl.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <ctime>

LV_FONT_DECLARE(font_zh_36);

static const std::string _tag = "screensaver";

namespace {

constexpr const char* WP_PATH = "/spiffs/bing.jpg";
// Idle timeout before the wallpaper appears. Tunable via NVS key "ss_idle_s".
constexpr int DEFAULT_IDLE_S = 60;

// ── Resident decoded wallpaper (PSRAM) ───────────────────────────────────────
std::mutex   g_mutex;
uint8_t*     g_wp_buf   = nullptr;   // currently-usable RGB565 frame (owned)
int          g_wp_w     = 0;
int          g_wp_h     = 0;
std::string  g_have_date;            // day of g_wp_buf, "" if none

uint8_t*     g_stage_buf = nullptr;  // freshly decoded, awaiting adoption
int          g_stage_w   = 0;
int          g_stage_h   = 0;
std::string  g_stage_date;

std::atomic<bool> g_ensuring{false};

// ── LVGL state (touched only on the UI loop) ─────────────────────────────────
// The screensaver is a full-screen object on lv_layer_top(), which sits above
// whatever app screen is active — so it covers every app (home / 智能家居 /
// 小智 / 设置) without fighting over screen ownership.
bool          g_active        = false;
bool          g_exit_requested = false;  // set by the overlay's press cb
lv_obj_t*     g_overlay      = nullptr;  // full-screen cover on the top layer
lv_obj_t*     g_time_label   = nullptr;
lv_obj_t*     g_date_label   = nullptr;
lv_image_dsc_t g_img_dsc     = {};
std::string   g_shown_time;          // last time string pushed to the label
uint32_t      g_idle_ms      = (uint32_t)DEFAULT_IDLE_S * 1000;
bool          g_enabled      = true;  // false when the user picked "关闭"
bool          g_inhibited    = false; // true while xiaozhi is open (no cover)

const char* const WDAYS[] = {"日", "一", "二", "三", "四", "五", "六"};

void format_now(std::string& time_str, std::string& date_str)
{
    time_t t  = time(nullptr);
    tm*    lt = localtime(&t);
    char tb[8], db[32];
    strftime(tb, sizeof(tb), "%H:%M", lt);
    strftime(db, sizeof(db), "%m月%d日 周", lt);
    time_str = tb;
    date_str = std::string(db) + WDAYS[lt->tm_wday];
}

// Adopt a freshly decoded frame (UI loop, while NOT showing it). Frees the old.
void adopt_staging_locked()
{
    if (!g_stage_buf) return;
    if (g_wp_buf) free(g_wp_buf);
    g_wp_buf   = g_stage_buf;
    g_wp_w     = g_stage_w;
    g_wp_h     = g_stage_h;
    g_have_date = g_stage_date;
    g_stage_buf = nullptr;
}

// Background: download today's wallpaper and hardware-decode it into PSRAM.
void ensure_async(const std::string& want_date)
{
    bool expected = false;
    if (!g_ensuring.compare_exchange_strong(expected, true)) return;  // already running

    GetHAL()->tryRunDetached([want_date]() {
        bool ok = screensaver::fetchTodayWallpaper(WP_PATH);
        if (ok) {
            int w = 0, h = 0;
            uint8_t* buf = GetHAL()->decodeJpegToRGB565(WP_PATH, w, h);
            if (buf) {
                std::lock_guard<std::mutex> lk(g_mutex);
                if (g_stage_buf) free(g_stage_buf);  // discard an older staged frame
                g_stage_buf  = buf;
                g_stage_w    = w;
                g_stage_h    = h;
                g_stage_date = want_date;
                mclog::tagInfo(_tag, "wallpaper ready {}x{} for {}", w, h, want_date.c_str());
            } else {
                mclog::tagWarn(_tag, "decode failed");
            }
        }
        g_ensuring.store(false);
    });
}

void enter()
{
    g_exit_requested = false;

    // Full-screen cover on the top layer — above every app screen.
    g_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(g_overlay, 1280, 720);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0x0A1A2E), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_set_style_radius(g_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_overlay, 0, 0);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Any press dismisses. Deleting from inside the object's own event would be
    // a use-after-free, so just flag it and let the next tick() tear it down.
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_overlay, [](lv_event_t*) { g_exit_requested = true; },
                        LV_EVENT_PRESSED, nullptr);

    // Full-screen wallpaper image from the resident RGB565 buffer (if ready).
    if (g_wp_buf && g_wp_w > 0 && g_wp_h > 0) {
        g_img_dsc = {};
        g_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        g_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        g_img_dsc.header.w      = g_wp_w;
        g_img_dsc.header.h      = g_wp_h;
        g_img_dsc.header.stride = g_wp_w * 2;
        g_img_dsc.data          = g_wp_buf;
        g_img_dsc.data_size     = (uint32_t)g_wp_w * g_wp_h * 2;
        lv_obj_t* img = lv_image_create(g_overlay);
        lv_image_set_src(img, &g_img_dsc);
        lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    // Legibility panel behind the clock (semi-transparent dark, bottom-left).
    lv_obj_t* panel = lv_obj_create(g_overlay);
    lv_obj_set_size(panel, 320, 132);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_LEFT, 36, -36);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_40, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    std::string ts, ds;
    format_now(ts, ds);

    // Time (montserrat_44, top) and date (zh, below) stacked with a clear gap —
    // no transform scaling, which would render past its layout box and overlap.
    g_time_label = lv_label_create(panel);
    lv_label_set_text(g_time_label, ts.c_str());
    lv_obj_set_style_text_font(g_time_label, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(g_time_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_time_label, LV_ALIGN_TOP_LEFT, 22, 16);
    g_shown_time = ts;

    g_date_label = lv_label_create(panel);
    lv_label_set_text(g_date_label, ds.c_str());
    lv_obj_set_style_text_font(g_date_label, &font_zh_36, 0);
    lv_obj_set_style_text_color(g_date_label, lv_color_hex(0xEAF3FB), 0);
    lv_obj_align(g_date_label, LV_ALIGN_TOP_LEFT, 22, 78);

    g_active = true;
    mclog::tagInfo(_tag, "enter");
}

void exit_ss()
{
    if (g_overlay) {
        lv_obj_delete(g_overlay);
        g_overlay    = nullptr;
        g_time_label = nullptr;
        g_date_label = nullptr;
    }
    g_active = false;

    // The overlay was the pressed/active object; clear every indev's dangling
    // references to it so the very next swipe (e.g. to exit the app underneath)
    // is detected normally instead of being swallowed.
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev;
         indev = lv_indev_get_next(indev)) {
        lv_indev_reset(indev, nullptr);
    }
    mclog::tagInfo(_tag, "exit");

    // Bring in a freshly decoded frame (e.g. day rolled over) for next time.
    std::lock_guard<std::mutex> lk(g_mutex);
    adopt_staging_locked();
}

}  // namespace

void screensaver::init()
{
    // Idle timeout is set in the Settings app (NVS key "ss_idle_s", seconds).
    // 0 (or negative) means the user turned the screensaver off.
    std::string s = GetHAL()->getConfig("ss_idle_s", std::to_string(DEFAULT_IDLE_S));
    int secs = DEFAULT_IDLE_S;
    try { secs = std::stoi(s); } catch (...) {}
    if (secs <= 0) {
        g_enabled = false;
        mclog::tagInfo(_tag, "disabled by settings");
        return;
    }
    if (secs < 5) secs = 5;
    g_idle_ms = (uint32_t)secs * 1000;

    ensure_async(screensaver::todayKey());
}

void screensaver::tick(uint32_t inactive_ms)
{
    if (!g_enabled) return;  // turned off in Settings

    // Inhibited (e.g. xiaozhi open): never cover it; dismiss if already showing.
    if (g_inhibited) {
        if (g_active) exit_ss();
        return;
    }

    // Adopt any freshly decoded frame while we're not displaying it.
    if (!g_active) {
        std::lock_guard<std::mutex> lk(g_mutex);
        adopt_staging_locked();
    }

    // Once per day: refresh the wallpaper in the background.
    std::string today = screensaver::todayKey();
    if (g_have_date != today && !g_ensuring.load()) {
        ensure_async(today);
    }

    if (!g_active) {
        // Enter on idle whether or not the wallpaper is ready: with an image we
        // show it, otherwise just the clock on a dark background. (This also
        // makes it obvious the screensaver triggered even if the download
        // hasn't finished yet.)
        if (inactive_ms >= g_idle_ms) {
            enter();
        }
        return;
    }

    // Active: a press on the screen, or any touch resetting the inactive
    // timer, dismisses the screensaver.
    if (g_exit_requested || inactive_ms < g_idle_ms) {
        exit_ss();
        return;
    }

    // Keep the clock current (only rewrite when the minute string changes).
    std::string ts, ds;
    format_now(ts, ds);
    if (ts != g_shown_time && g_time_label) {
        lv_label_set_text(g_time_label, ts.c_str());
        if (g_date_label) lv_label_set_text(g_date_label, ds.c_str());
        g_shown_time = ts;
    }
}

bool screensaver::isActive()
{
    return g_active;
}

void screensaver::wake()
{
    // Flag for tick() to tear down the overlay (deleting it here could race the
    // UI loop). If not active this is harmless — enter() clears it.
    g_exit_requested = true;
}

void screensaver::setInhibited(bool inhibited)
{
    g_inhibited = inhibited;
}
