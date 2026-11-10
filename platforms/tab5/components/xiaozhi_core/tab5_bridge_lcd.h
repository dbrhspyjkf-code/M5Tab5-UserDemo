#pragma once
#include "display/lcd_display.h"
#include <lvgl.h>

/**
 * Tab5BridgeLcdDisplay - wraps the lv_display_t* already created by
 * Tab5-UserDemo's BSP.  Does NOT reinitialize any LCD hardware or LVGL.
 *
 * Layout (2026-11-10 redesign):
 *   - Top 3/4: blurred snapshot of the screen that was visible before
 *     xiaozhi was invoked (captured on every ActivateScreen)
 *   - Middle: emotion face (4x font_awesome icon, wanders while idle)
 *   - Bottom 1/4: semi-transparent scrim showing ONLY the current message
 *     (each new message replaces the previous one, no bubble history)
 *   - Status bar (colored dot + Chinese state text), tap-to-talk anywhere
 */
class Tab5BridgeLcdDisplay : public LcdDisplay {
public:
    Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height);
    ~Tab5BridgeLcdDisplay() override;

    void ActivateScreen();
    void DeactivateScreen();  // restores control to Mooncake (screen restored by AppXiaoZhi close callback)
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
    void BuildBottomStrip();
    void BuildStatusBar();

    // Snapshot the outgoing screen into bg_buf_, blur it, and show it as the
    // fullscreen backdrop. Called from ActivateScreen BEFORE lv_screen_load,
    // while the previous screen is still the active one.
    void CaptureBlurredBackdrop();

    // ── State ───────────────────────────────────────────────────────────────────
    enum class State { IDLE, CONNECTING, LISTENING, THINKING, SPEAKING };
    State state_ = State::IDLE;

    // ── LVGL objects ────────────────────────────────────────────────────────────
    lv_obj_t* scr_            = nullptr;

    // Info bar
    lv_obj_t* time_lbl_       = nullptr;
    lv_obj_t* wifi_icon_      = nullptr;
    lv_obj_t* batt_icon_      = nullptr;
    lv_obj_t* batt_pct_lbl_   = nullptr;

    // Blurred backdrop (fullscreen image; hidden until first snapshot)
    lv_obj_t* bg_img_         = nullptr;
    lv_draw_buf_t* bg_buf_    = nullptr;   // persistent 1280x720 RGB565 snapshot buffer
    lv_image_dsc_t bg_dsc_    = {};        // header pointing into bg_buf_
    void* blur_tmp_           = nullptr;   // downsample scratch (allocated with bg_buf_)

    // Emotion area
    lv_obj_t* emotion_icon_lbl_ = nullptr;

    // Bottom strip: current message (single label, no bubbles)
    lv_obj_t* msg_lbl_        = nullptr;
    lv_obj_t* hint_lbl_       = nullptr;

    // Status bar
    lv_obj_t* state_dot_lbl_  = nullptr;
    lv_obj_t* state_text_lbl_ = nullptr;

    // ── Timers ─────────────────────────────────────────────────────────────────
    lv_timer_t* info_timer_   = nullptr;
};
