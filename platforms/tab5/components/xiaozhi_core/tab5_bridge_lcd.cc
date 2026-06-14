#include "tab5_bridge_lcd.h"
#include "application.h"
#include "xiaozhi_ctl.h"
#include <cbin_font.h>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_wifi.h>
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
    lv_obj_set_style_bg_color(scr_, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_pad_all(scr_, 0, 0);
    lv_obj_clear_flag(scr_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_, scr_tap_cb, LV_EVENT_CLICKED, nullptr);

    BuildInfoBar();     // y=0,   h=56
    BuildEmotionArea(); // y=56,  h=124
    BuildChatScroll();  // y=180, h=484
    BuildStatusBar();   // y=664, h=56  → total=720

    info_timer_ = lv_timer_create(info_timer_cb, 1000, this); // 1 s
    // Populate info bar immediately (don't wait 1 s for first tick).
    UpdateInfoBar();
}

void Tab5BridgeLcdDisplay::BuildInfoBar()
{
    lv_obj_t* bar = lv_obj_create(scr_);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, width_, 56);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x091422), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Title (left)
    lv_obj_t* title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, zh_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x5599CC), 0);
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
    lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(0x5599CC), 0);
    lv_label_set_text(wifi_icon_, FONT_AWESOME_WIFI);
    lv_obj_align(wifi_icon_, LV_ALIGN_RIGHT_MID, -150, 0);

    // Battery icon
    batt_icon_ = lv_label_create(bar);
    lv_obj_set_style_text_font(batt_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(batt_icon_, lv_color_hex(0x5599CC), 0);
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
    lv_obj_t* area = lv_obj_create(scr_);
    lv_obj_set_pos(area, 0, 56);
    lv_obj_set_size(area, width_, 124);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_clear_flag(area, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(area, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Font-awesome icon scaled 2x (512 = 2× in LVGL 9, where 256 = 1×)
    emotion_icon_lbl_ = lv_label_create(area);
    lv_obj_set_style_text_font(emotion_icon_lbl_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(emotion_icon_lbl_, lv_color_hex(0xFFD700), 0);
    lv_label_set_text(emotion_icon_lbl_, FONT_AWESOME_NEUTRAL);
    lv_obj_align(emotion_icon_lbl_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_transform_scale_x(emotion_icon_lbl_, 512, 0);
    lv_obj_set_style_transform_scale_y(emotion_icon_lbl_, 512, 0);
}

void Tab5BridgeLcdDisplay::BuildChatScroll()
{
    chat_scroll_ = lv_obj_create(scr_);
    lv_obj_set_pos(chat_scroll_, 0, 180);
    lv_obj_set_size(chat_scroll_, width_, 484);
    lv_obj_set_style_bg_color(chat_scroll_, lv_color_hex(0x0D1B2A), 0);
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
    lv_obj_set_style_text_color(hint_lbl_, lv_color_hex(0x334455), 0);
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
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x091422), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    state_dot_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(state_dot_lbl_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(state_dot_lbl_, lv_color_hex(0x556677), 0);
    lv_label_set_text(state_dot_lbl_, "●");
    lv_obj_align(state_dot_lbl_, LV_ALIGN_LEFT_MID, 36, 0);

    state_text_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(state_text_lbl_, zh_font(), 0);
    lv_obj_set_style_text_color(state_text_lbl_, lv_color_hex(0x778899), 0);
    lv_label_set_text(state_text_lbl_, "待命");
    lv_obj_align(state_text_lbl_, LV_ALIGN_LEFT_MID, 68, 0);
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
    lv_color_t bg = is_user ? lv_color_hex(0x1A4A8A) : lv_color_hex(0x1C2D3E);
    lv_obj_set_style_bg_color(bubble, bg, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 18, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 18, 0);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    // Message label
    lv_obj_t* lbl = lv_label_create(bubble);
    lv_obj_set_style_text_font(lbl, zh_font(), 0);
    lv_color_t tc = is_user ? lv_color_hex(0xCCE4FF) : lv_color_hex(0xEEEEEE);
    lv_obj_set_style_text_color(lbl, tc, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, bubble_max_w - 36, 0);
    lv_label_set_text(lbl, content);

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
        lv_color_t c = rssi ? lv_color_hex(0x5599CC) : lv_color_hex(0xFF4444);
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
                         :            lv_color_hex(0x5599CC);
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
        case State::LISTENING:   dot_color = lv_color_hex(0x3A7BD5); break;
        case State::SPEAKING:    dot_color = lv_color_hex(0x2ECC71); break;
        case State::CONNECTING:  dot_color = lv_color_hex(0xF39C12); break;
        case State::THINKING:    dot_color = lv_color_hex(0x9B59B6); break;
        default:                 dot_color = lv_color_hex(0x556677); break;
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
