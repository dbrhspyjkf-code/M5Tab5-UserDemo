#include "tab5_bridge_lcd.h"
#include "application.h"
#include "xiaozhi_ctl.h"
#include <cbin_font.h>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <ctime>
#include <cstring>
static const char* TAG = "Tab5BridgeLcd";

LV_FONT_DECLARE(font_awesome_30_4);

// ─── Fonts ────────────────────────────────────────────────────────────────────
// GB2312 common ~3500 glyphs, referenced in-place from flash (no big RAM copy).
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");

static const lv_font_t* zh_font()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}

// ─── Helper mappings ──────────────────────────────────────────────────────────

static const char* emotion_to_icon(const char* e)
{
    if (!e)                        return FONT_AWESOME_NEUTRAL;
    if (!strcmp(e, "happy"))       return FONT_AWESOME_HAPPY;
    if (!strcmp(e, "laughing"))    return FONT_AWESOME_LAUGHING;
    if (!strcmp(e, "funny"))       return FONT_AWESOME_FUNNY;
    if (!strcmp(e, "sad"))         return FONT_AWESOME_SAD;
    if (!strcmp(e, "angry"))       return FONT_AWESOME_ANGRY;
    if (!strcmp(e, "crying"))      return FONT_AWESOME_CRYING;
    if (!strcmp(e, "loving"))      return FONT_AWESOME_LOVING;
    if (!strcmp(e, "thinking"))    return FONT_AWESOME_THINKING;
    if (!strcmp(e, "confused"))    return FONT_AWESOME_CONFUSED;
    if (!strcmp(e, "surprised"))   return FONT_AWESOME_SURPRISED;
    if (!strcmp(e, "cool"))        return FONT_AWESOME_COOL;
    if (!strcmp(e, "excited"))     return FONT_AWESOME_LAUGHING;
    if (!strcmp(e, "winking"))     return FONT_AWESOME_WINKING;
    if (!strcmp(e, "microchip_ai")) return FONT_AWESOME_MICROCHIP_AI;
    return FONT_AWESOME_NEUTRAL;
}

static const char* rssi_to_icon(int rssi)
{
    if (rssi == 0)   return FONT_AWESOME_WIFI_SLASH;
    if (rssi >= -60) return FONT_AWESOME_WIFI;
    if (rssi >= -75) return FONT_AWESOME_WIFI_FAIR;
    return FONT_AWESOME_WIFI_WEAK;
}

static const char* pct_to_batt_icon(int pct)
{
    if (pct < 0)   return FONT_AWESOME_BATTERY_SLASH;
    if (pct >= 75) return FONT_AWESOME_BATTERY_FULL;
    if (pct >= 50) return FONT_AWESOME_BATTERY_THREE_QUARTERS;
    if (pct >= 25) return FONT_AWESOME_BATTERY_HALF;
    if (pct >= 10) return FONT_AWESOME_BATTERY_QUARTER;
    return FONT_AWESOME_BATTERY_EMPTY;
}

// ─── LVGL timer callbacks (run inside LVGL lock — no DisplayLockGuard) ────────

static void info_timer_cb(lv_timer_t* t)
{
    static_cast<Tab5BridgeLcdDisplay*>(lv_timer_get_user_data(t))->UpdateInfoBar();
}

static void scr_tap_cb(lv_event_t* /*e*/)
{
    Application::GetInstance().ToggleChatState();
}

// ─── Construction / destruction ───────────────────────────────────────────────

Tab5BridgeLcdDisplay::Tab5BridgeLcdDisplay(lv_display_t* existing_disp,
                                            int width, int height)
    : LcdDisplay(nullptr, nullptr, width, height)
{
    display_ = existing_disp;
    BuildRichUi();
    ESP_LOGI(TAG, "rich display ready %dx%d scr=%p", width, height, scr_);
}

Tab5BridgeLcdDisplay::~Tab5BridgeLcdDisplay()
{
    // Delete timer before LVGL objects (it references 'this').
    // Must be within LVGL lock since the timer runs in LVGL context.
    DisplayLockGuard lock(this);
    if (info_timer_) { lv_timer_delete(info_timer_); info_timer_ = nullptr; }
    // Null out borrowed handles so ~LcdDisplay() doesn't double-free them.
    display_  = nullptr;
    panel_    = nullptr;
    panel_io_ = nullptr;
}

void Tab5BridgeLcdDisplay::ActivateScreen()
{
    if (!scr_) return;
    DisplayLockGuard lock(this);
    lv_screen_load(scr_);
}

// ─── UI construction ──────────────────────────────────────────────────────────

void Tab5BridgeLcdDisplay::BuildRichUi()
{
    DisplayLockGuard lock(this);

    scr_ = lv_obj_create(NULL);
    lv_obj_set_size(scr_, width_, height_);
    lv_obj_set_style_bg_color(scr_, lv_color_hex(0x0A1A2E), 0);
    lv_obj_set_style_pad_all(scr_, 0, 0);
    lv_obj_clear_flag(scr_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_, scr_tap_cb, LV_EVENT_CLICKED, nullptr);

    // The top info bar (clock/WiFi/battery) now lives on the home screen, so
    // xiaozhi runs full-screen with no top bar. The bottom status bar (the
    // listening/speaking state dot) stays.
    BuildEmotionArea(); // y=12,  h=136
    BuildChatScroll();  // y=152, h=512
    BuildStatusBar();   // y=664, h=56  → total=720
}

void Tab5BridgeLcdDisplay::BuildInfoBar()
{
    lv_obj_t* bar = lv_obj_create(scr_);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, width_, 56);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x07111E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Title (left)
    lv_obj_t* title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, zh_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(title, "小智 AI");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    // Clock (center)
    time_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(time_lbl_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(time_lbl_, lv_color_hex(0xBBCCDD), 0);
    lv_label_set_text(time_lbl_, "00:00");
    lv_obj_align(time_lbl_, LV_ALIGN_CENTER, 0, 0);

    // WiFi icon
    wifi_icon_ = lv_label_create(bar);
    lv_obj_set_style_text_font(wifi_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(wifi_icon_, FONT_AWESOME_WIFI);
    lv_obj_align(wifi_icon_, LV_ALIGN_RIGHT_MID, -150, 0);

    // Battery icon
    batt_icon_ = lv_label_create(bar);
    lv_obj_set_style_text_font(batt_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(batt_icon_, lv_color_hex(0x4FA3FF), 0);
    lv_label_set_text(batt_icon_, FONT_AWESOME_BATTERY_FULL);
    lv_obj_align(batt_icon_, LV_ALIGN_RIGHT_MID, -80, 0);

    // Battery %
    batt_pct_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(batt_pct_lbl_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(batt_pct_lbl_, lv_color_hex(0xBBCCDD), 0);
    lv_label_set_text(batt_pct_lbl_, "--%");
    lv_obj_align(batt_pct_lbl_, LV_ALIGN_RIGHT_MID, -20, 0);
}

void Tab5BridgeLcdDisplay::BuildEmotionArea()
{
    // The face lives directly on the screen (not a small band) so it can wander
    // the WHOLE screen while idle. Resting position is the top band, above chat.
    // Font-awesome icon scaled ~2× (512 ≈ 2× in LVGL 9, 256 = 1×). Was 768/3×
    // but the icon's glyph padding made the bottom get clipped by the chat
    // scroll area starting at y=152. y=30 keeps the icon fully within 0..152.
    emotion_icon_lbl_ = lv_label_create(scr_);
    lv_obj_set_style_text_font(emotion_icon_lbl_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_icon_lbl_, lv_color_hex(0xFFD700), 0);
    lv_label_set_text(emotion_icon_lbl_, FONT_AWESOME_NEUTRAL);
    lv_obj_align(emotion_icon_lbl_, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_transform_scale_x(emotion_icon_lbl_, 512, 0);
    lv_obj_set_style_transform_scale_y(emotion_icon_lbl_, 512, 0);

    // Idle "screensaver" — drift the face to random spots across the whole
    // screen while idle, and glide back to the top band when a conversation
    // starts. translate styles shift it visually without changing its layout
    // anchor, so SetEmotion / recenter stay simple.
    // Slow cadence + instant moves (below). This device is resource-tight: the
    // always-on wake-word AFE shares CPU with display rendering, so a continuous
    // animation here starved the audio pipeline (couldn't wake / no response).
    lv_timer_create(EmojiWanderTimerCb, 5000, this);
}

void Tab5BridgeLcdDisplay::EmojiWanderTimerCb(lv_timer_t* t)
{
    auto* self = static_cast<Tab5BridgeLcdDisplay*>(lv_timer_get_user_data(t));
    if (!self || !self->emotion_icon_lbl_) return;

    lv_obj_t* e = self->emotion_icon_lbl_;
    bool idle = (Application::GetInstance().GetDeviceState() == kDeviceStateIdle);

    if (idle) {
        // Float above chat/status ONCE (move_foreground reorders → full redraw;
        // doing it every tick was part of what starved the audio). After that
        // the label stays topmost.
        static bool fg_done = false;
        if (!fg_done) { lv_obj_move_foreground(e); fg_done = true; }

        // Instant hop to a random spot — a single small redraw every 5 s,
        // negligible load, instead of 90 animated frames. Base anchor is
        // top-mid (x centered, y=70).
        int max_x = self->width_ / 2 - 90;
        int min_y = -50;
        int max_y = self->height_ - 70 - 110;
        if (max_x < 1) max_x = 1;
        if (max_y < min_y + 1) max_y = min_y + 1;
        int tx = (int)(esp_random() % (uint32_t)(2 * max_x + 1)) - max_x;
        int ty = min_y + (int)(esp_random() % (uint32_t)(max_y - min_y + 1));
        lv_obj_set_style_translate_x(e, tx, 0);
        lv_obj_set_style_translate_y(e, ty, 0);
    } else if (lv_obj_get_style_translate_x(e, 0) != 0 ||
               lv_obj_get_style_translate_y(e, 0) != 0) {
        // Snap back to the top band for the conversation.
        lv_obj_set_style_translate_x(e, 0, 0);
        lv_obj_set_style_translate_y(e, 0, 0);
    }
}

void Tab5BridgeLcdDisplay::BuildChatScroll()
{
    chat_scroll_ = lv_obj_create(scr_);
    lv_obj_set_pos(chat_scroll_, 0, 152);
    lv_obj_set_size(chat_scroll_, width_, 512);
    lv_obj_set_style_bg_color(chat_scroll_, lv_color_hex(0x0A1A2E), 0);
    lv_obj_set_style_bg_opa(chat_scroll_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chat_scroll_, 0, 0);
    lv_obj_set_style_pad_left(chat_scroll_, 40, 0);
    lv_obj_set_style_pad_right(chat_scroll_, 40, 0);
    lv_obj_set_style_pad_top(chat_scroll_, 20, 0);
    lv_obj_set_style_pad_bottom(chat_scroll_, 20, 0);
    lv_obj_set_style_pad_row(chat_scroll_, 12, 0);
    lv_obj_set_flex_flow(chat_scroll_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_scroll_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(chat_scroll_, LV_DIR_VER);

    // Placeholder hint
    hint_lbl_ = lv_label_create(chat_scroll_);
    lv_obj_set_style_text_font(hint_lbl_, zh_font(), 0);
    lv_obj_set_style_text_color(hint_lbl_, lv_color_hex(0x4A6A8C), 0);
    lv_obj_set_style_text_align(hint_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint_lbl_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint_lbl_, width_ - 80);
    lv_label_set_text(hint_lbl_, "点击屏幕开始对话\nTap screen to talk");
    lv_obj_set_style_margin_top(hint_lbl_, 140, 0);
}

void Tab5BridgeLcdDisplay::BuildStatusBar()
{
    lv_obj_t* bar = lv_obj_create(scr_);
    lv_obj_set_pos(bar, 0, 664);
    lv_obj_set_size(bar, width_, 56);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x07111E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    state_dot_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(state_dot_lbl_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(state_dot_lbl_, lv_color_hex(0x6A8CA8), 0);
    lv_label_set_text(state_dot_lbl_, "●");
    lv_obj_align(state_dot_lbl_, LV_ALIGN_LEFT_MID, 36, 0);

    state_text_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(state_text_lbl_, zh_font(), 0);
    lv_obj_set_style_text_color(state_text_lbl_, lv_color_hex(0x9DB4CC), 0);
    lv_label_set_text(state_text_lbl_, "待命");
    lv_obj_align(state_text_lbl_, LV_ALIGN_LEFT_MID, 68, 0);
}

// Strip emoji / symbol code-points that the bundled cbin font (puhui_common)
// doesn't carry. cbin has NO emoji glyphs at all, so even whitelisted
// emoji would render as blank + spam 'glyph dsc. not found' warnings. To
// keep the chat readable and the warning log quiet, EVERY emoji / dingbat
// / misc-technical code-point is replaced with a space. CJK / Latin /
// punctuation pass through unchanged. The emoji_table.h whitelist is
// kept for future use (e.g. when an emoji font is added or bitmap
// assets are embedded) but the render path no longer consults it.
#include "emoji_table.h"

static std::string _strip_unsupported_glyphs(const char* in)
{
    if (!in) return "";
    std::string s;
    const unsigned char* p = (const unsigned char*)in;
    while (*p) {
        uint32_t cp = 0;
        int n = 0;
        if      ((*p & 0x80) == 0x00) { cp =  *p       & 0x7F;       n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp =  *p       & 0x1F;       n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp =  *p       & 0x0F;       n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp =  *p       & 0x07;       n = 4; }
        else                           { p++; continue; }
        bool valid = true;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }
        // Drop ALL emoji / dingbats / misc-technical code-points. The cbin
        // font carries no emoji glyphs, so rendering any of them only
        // produces 'glyph dsc. not found' warnings and risks stack
        // protection faults on long text (see 2026-06-19 logs).
        bool drop =
            (cp >= 0x1F000 && cp < 0x1FFFF) ||  // emoji blocks
            (cp >= 0x2700  && cp < 0x27BF)  ||  // dingbats
            (cp >= 0x2300  && cp < 0x24FF);     // misc technical / control pics
        if (drop) s += ' ';
        else      s.append((const char*)p, n);
        p += n;
    }
    return s;
}

// ─── Chat bubble helper ───────────────────────────────────────────────────────

void Tab5BridgeLcdDisplay::AddChatBubble(const char* role, const char* content)
{
    bool is_user = (role && strcmp(role, "user") == 0);

    // Remove hint on first message
    if (hint_lbl_) {
        lv_obj_delete(hint_lbl_);
        hint_lbl_ = nullptr;
    }

    // Evict oldest if at cap
    if ((int)chat_bubbles_.size() >= MAX_BUBBLES) {
        lv_obj_delete(chat_bubbles_.front());
        chat_bubbles_.pop_front();
    }

    int bubble_max_w = width_ * 3 / 4; // 75 % of screen

    // Outer row (full width flex item) — holds the bubble aligned left or right
    lv_obj_t* row = lv_obj_create(chat_scroll_);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Bubble background
    lv_obj_t* bubble = lv_obj_create(row);
    lv_color_t bg = is_user ? lv_color_hex(0x1E6FC2) : lv_color_hex(0x13304E);
    lv_obj_set_style_bg_color(bubble, bg, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 18, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 18, 0);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    // Drop emoji / symbol code-points the font doesn't carry (avoids LVGL
    // 'glyph dsc. not found' warning storms that tripped Stack protection
    // fault on 2026-06-19). The emoji_table.h whitelist is preserved but
    // not consulted — see header comment on _strip_unsupported_glyphs.
    std::string filtered = _strip_unsupported_glyphs(content);

    // Message label
    lv_obj_t* lbl = lv_label_create(bubble);
    lv_obj_set_style_text_font(lbl, zh_font(), 0);
    lv_color_t tc = is_user ? lv_color_hex(0xCCE4FF) : lv_color_hex(0xEEEEEE);
    lv_obj_set_style_text_color(lbl, tc, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, bubble_max_w - 36, 0);
    lv_label_set_text(lbl, filtered.c_str());

    // Align bubble within row
    lv_obj_align(bubble, is_user ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);

    chat_bubbles_.push_back(row);

    // Auto-scroll to bottom
    lv_obj_scroll_to_y(chat_scroll_, LV_COORD_MAX, LV_ANIM_ON);
}

// ─── LVGL timer: info bar refresh ─────────────────────────────────────────────

void Tab5BridgeLcdDisplay::UpdateInfoBar()
{
    // Clock
    if (time_lbl_) {
        time_t now;
        time(&now);
        struct tm t = {};
        localtime_r(&now, &t);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        lv_label_set_text(time_lbl_, buf);
    }

    // WiFi RSSI icon
    if (wifi_icon_) {
        wifi_ap_record_t ap = {};
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
        lv_label_set_text(wifi_icon_, rssi_to_icon(rssi));
        lv_color_t c = rssi ? lv_color_hex(0x4FA3FF) : lv_color_hex(0xFF4444);
        lv_obj_set_style_text_color(wifi_icon_, c, 0);
    }

    // Battery
    if (batt_icon_ && batt_pct_lbl_) {
        int pct = xiaozhi_get_battery_percent();
        lv_label_set_text(batt_icon_, pct_to_batt_icon(pct));
        if (pct >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_label_set_text(batt_pct_lbl_, buf);
            lv_color_t c = pct < 20 ? lv_color_hex(0xFF4444)
                         : pct < 40 ? lv_color_hex(0xF39C12)
                         :            lv_color_hex(0x4FA3FF);
            lv_obj_set_style_text_color(batt_icon_, c, 0);
            lv_obj_set_style_text_color(batt_pct_lbl_, c, 0);
        } else {
            lv_label_set_text(batt_pct_lbl_, "--%");
        }
    }
}

// ─── LcdDisplay overrides ─────────────────────────────────────────────────────

void Tab5BridgeLcdDisplay::SetupUI()
{
    setup_ui_called_ = true; // suppress xiaozhi's heavy default UI
}

void Tab5BridgeLcdDisplay::SetStatus(const char* status)
{
    if (!status) return;
    DisplayLockGuard lock(this);

    // Derive state from status string (zh-CN)
    State new_state = State::THINKING;
    if (strstr(status, "聆听"))  new_state = State::LISTENING;
    else if (strstr(status, "说话"))  new_state = State::SPEAKING;
    else if (strstr(status, "待命"))  new_state = State::IDLE;
    else if (strstr(status, "连接"))  new_state = State::CONNECTING;
    state_ = new_state;

    // Status bar dot color
    lv_color_t dot_color;
    switch (state_) {
        case State::LISTENING:   dot_color = lv_color_hex(0x4FA3FF); break;
        case State::SPEAKING:    dot_color = lv_color_hex(0x2ECC71); break;
        case State::CONNECTING:  dot_color = lv_color_hex(0xF39C12); break;
        case State::THINKING:    dot_color = lv_color_hex(0x9B59B6); break;
        default:                 dot_color = lv_color_hex(0x6A8CA8); break;
    }
    if (state_dot_lbl_)  lv_obj_set_style_text_color(state_dot_lbl_, dot_color, 0);
    if (state_text_lbl_) lv_label_set_text(state_text_lbl_, status);
}

void Tab5BridgeLcdDisplay::SetEmotion(const char* emotion)
{
    if (!emotion) return;
    DisplayLockGuard lock(this);
    if (emotion_icon_lbl_) lv_label_set_text(emotion_icon_lbl_, emotion_to_icon(emotion));
}

void Tab5BridgeLcdDisplay::SetChatMessage(const char* role, const char* content)
{
    if (!content || content[0] == '\0') return;
    DisplayLockGuard lock(this);
    AddChatBubble(role ? role : "assistant", content);
}

void Tab5BridgeLcdDisplay::ClearChatMessages()
{
    DisplayLockGuard lock(this);
    for (auto* obj : chat_bubbles_) {
        lv_obj_delete(obj);
    }
    chat_bubbles_.clear();
}

void Tab5BridgeLcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> /*image*/)
{
    // No preview support in the lightweight display.
}

void Tab5BridgeLcdDisplay::SetTheme(Theme* theme)
{
    Display::SetTheme(theme);
}
