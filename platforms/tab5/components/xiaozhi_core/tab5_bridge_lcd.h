#pragma once
#include "display/lcd_display.h"
#include <lvgl.h>
#include <deque>

/**
 * Tab5BridgeLcdDisplay - wraps the lv_display_t* already created by
 * Tab5-UserDemo's BSP.  Does NOT reinitialize any LCD hardware or LVGL.
 *
 * Provides a rich UI with:
 *   - Info bar (clock, WiFi RSSI icon, battery %)
 *   - Emotion area (2x font_awesome icon)
 *   - Scrollable chat bubble history (user = right/blue, AI = left/dark)
 *   - Status bar (colored dot + Chinese state text)
 *   - Tap-to-talk on the full screen
 */
class Tab5BridgeLcdDisplay : public LcdDisplay {
public:
    Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height);
    ~Tab5BridgeLcdDisplay() override;

    void ActivateScreen();
    lv_obj_t* GetScreen() const { return scr_; }

    // ── Overrides ──────────────────────────────────────────────────────────────
    void SetupUI()       override;
    void SetStatus(const char* status)  override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void SetTheme(Theme* theme) override;

    // ── Called by LVGL timer (no extra locking needed) ─────────────────────────
    void UpdateInfoBar();

    // Idle screensaver: drift the emotion face to random spots while idle.
    static void EmojiWanderTimerCb(lv_timer_t* t);

private:
    // ── Layout helpers ──────────────────────────────────────────────────────────
    void BuildRichUi();
    void BuildInfoBar();
    void BuildEmotionArea();
    void BuildChatScroll();
    void BuildStatusBar();
    void AddChatBubble(const char* role, const char* content);

    // ── State ───────────────────────────────────────────────────────────────────
    enum class State { IDLE, CONNECTING, LISTENING, THINKING, SPEAKING };
    State state_ = State::IDLE;

    static constexpr int MAX_BUBBLES = 8;

    // ── LVGL objects ────────────────────────────────────────────────────────────
    lv_obj_t* scr_            = nullptr;

    // Info bar
    lv_obj_t* time_lbl_       = nullptr;
    lv_obj_t* wifi_icon_      = nullptr;
    lv_obj_t* batt_icon_      = nullptr;
    lv_obj_t* batt_pct_lbl_   = nullptr;

    // Emotion area
    lv_obj_t* emotion_icon_lbl_ = nullptr;

    // Chat
    lv_obj_t* chat_scroll_    = nullptr;
    lv_obj_t* hint_lbl_       = nullptr;
    std::deque<lv_obj_t*> chat_bubbles_;

    // Status bar
    lv_obj_t* state_dot_lbl_  = nullptr;
    lv_obj_t* state_text_lbl_ = nullptr;

    // ── Timers ─────────────────────────────────────────────────────────────────
    lv_timer_t* info_timer_   = nullptr;
};
