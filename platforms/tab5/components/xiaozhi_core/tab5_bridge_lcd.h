#pragma once
#include "display/lcd_display.h"
#include <lvgl.h>

/**
 * Tab5BridgeLcdDisplay - wraps the lv_display_t* already created by
 * Tab5-UserDemo's BSP.  Does NOT reinitialize any LCD hardware or LVGL.
 *
 * Layout (2026-11-11 final):
 *   - Fullscreen plain background (no snapshot/blur — 用户定稿: 不虚化不透明)
 *   - Middle: emotion face (官方 Twemoji64, fixed at center-lower, no wander)
 *   - Bottom 1/4: opaque strip showing ONLY the current message
 *     (each new message replaces the previous one, no bubble history)
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

private:
    // ── Layout helpers ──────────────────────────────────────────────────────────
    void BuildRichUi();
    void BuildInfoBar();
    void BuildEmotionArea();
    void BuildBottomStrip();

    // ── LVGL objects ────────────────────────────────────────────────────────────
    lv_obj_t* scr_            = nullptr;

    // Info bar
    lv_obj_t* time_lbl_       = nullptr;
    lv_obj_t* wifi_icon_      = nullptr;
    lv_obj_t* batt_icon_      = nullptr;
    lv_obj_t* batt_pct_lbl_   = nullptr;

    // Emotion area (官方 Twemoji64 表情包, 2026-11-10 替换 font_awesome 字形)
    lv_obj_t* emotion_img_ = nullptr;

    // Bottom strip: current message (single label, no bubbles, no status text —
    // 2026-11-10 语音直聊定稿: 无「聆听中」状态行, 无「点击屏幕开始对话」提示)
    lv_obj_t* msg_lbl_        = nullptr;

    // ── Timers ─────────────────────────────────────────────────────────────────
    lv_timer_t* info_timer_   = nullptr;
};
