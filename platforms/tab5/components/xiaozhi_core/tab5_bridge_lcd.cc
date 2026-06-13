#include "tab5_bridge_lcd.h"
#include "application.h"
#include <esp_log.h>

static const char* TAG = "Tab5BridgeLcd";

// Chinese-capable font shipped by the 78/xiaozhi-fonts component (C linkage).
extern "C" const lv_font_t font_puhui_basic_30_4;

// Tap-to-talk: there is no wake word (CONFIG_USE_AUDIO_PROCESSOR=0) and no
// physical button on the bridge, so tapping the screen toggles the
// conversation (idle -> listening -> speaking).
static void scr_tap_cb(lv_event_t* /*e*/)
{
    ESP_LOGI(TAG, "screen tapped -> ToggleChatState");
    Application::GetInstance().ToggleChatState();
}

Tab5BridgeLcdDisplay::Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height)
    : LcdDisplay(nullptr, nullptr, width, height)
{
    // Borrow the already-initialized LVGL display handle.
    // display_ is protected in LvglDisplay, so we can set it directly here.
    display_ = existing_disp;

    BuildSimpleUi();
    ESP_LOGI(TAG, "bridge display ready, w=%d h=%d scr=%p", width, height, scr_);
}

void Tab5BridgeLcdDisplay::BuildSimpleUi()
{
    DisplayLockGuard lock(this);

    scr_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_, lv_color_hex(0x0D1B2A), 0);
    lv_obj_clear_flag(scr_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_, scr_tap_cb, LV_EVENT_CLICKED, nullptr);

    // Status line (top).
    status_lbl_ = lv_label_create(scr_);
    lv_obj_set_style_text_font(status_lbl_, &font_puhui_basic_30_4, 0);
    lv_obj_set_style_text_color(status_lbl_, lv_color_hex(0x7FB2E5), 0);
    lv_label_set_text(status_lbl_, "小智");
    lv_obj_align(status_lbl_, LV_ALIGN_TOP_MID, 0, 24);

    // Chat / hint area (center, wraps).
    chat_lbl_ = lv_label_create(scr_);
    lv_obj_set_style_text_font(chat_lbl_, &font_puhui_basic_30_4, 0);
    lv_obj_set_style_text_color(chat_lbl_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(chat_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(chat_lbl_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(chat_lbl_, 1100);
    lv_label_set_text(chat_lbl_, "点击屏幕开始对话\nTap screen to talk");
    lv_obj_center(chat_lbl_);
}

Tab5BridgeLcdDisplay::~Tab5BridgeLcdDisplay()
{
    // Null out the handles we don't own BEFORE ~LcdDisplay() runs.
    // ~LcdDisplay() checks for non-null before calling lv_display_delete /
    // esp_lcd_panel_del / esp_lcd_panel_io_del, so nulling prevents double-free.
    display_  = nullptr;
    panel_    = nullptr;
    panel_io_ = nullptr;
}

void Tab5BridgeLcdDisplay::ActivateScreen()
{
    if (scr_) {
        DisplayLockGuard lock(this);
        lv_screen_load(scr_);
    }
}

// ── Overrides: keep the simple UI, never touch the heavy null widgets ──

void Tab5BridgeLcdDisplay::SetupUI()
{
    // Simple UI is already built in the constructor. Do NOT build xiaozhi's
    // heavy UI (it fails to allocate under the tight RAM budget -> black).
    setup_ui_called_ = true;
}

void Tab5BridgeLcdDisplay::SetStatus(const char* status)
{
    if (!status_lbl_ || !status) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(status_lbl_, status);
    lv_obj_align(status_lbl_, LV_ALIGN_TOP_MID, 0, 24);
}

void Tab5BridgeLcdDisplay::SetEmotion(const char* /*emotion*/)
{
    // No emoji UI in the lightweight display.
}

void Tab5BridgeLcdDisplay::SetChatMessage(const char* /*role*/, const char* content)
{
    if (!chat_lbl_ || !content) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(chat_lbl_, content);
    lv_obj_center(chat_lbl_);
}

void Tab5BridgeLcdDisplay::ClearChatMessages()
{
    if (!chat_lbl_) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(chat_lbl_, "");
}

void Tab5BridgeLcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> /*image*/)
{
    // No preview image support in the lightweight display.
}

void Tab5BridgeLcdDisplay::SetTheme(Theme* theme)
{
    // Persist the theme selection without touching the heavy widgets.
    Display::SetTheme(theme);
}
