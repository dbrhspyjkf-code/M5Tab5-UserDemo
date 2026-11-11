#pragma once
#include "display/lcd_display.h"
#include <lvgl.h>

/**
 * Tab5BridgeLcdDisplay - wraps the lv_display_t* already created by
 * Tab5-UserDemo's BSP.  Does NOT reinitialize any LCD hardware or LVGL.
 *
 * Layout (2026-11-11 final, overlay architecture):
 *   - UI lives on a TRANSPARENT full-screen container on lv_layer_top()
 *     (same mechanism as the photo screensaver) — the underlying app page
 *     stays visible and untouched everywhere except the bottom strip.
 *     Activate/Deactivate = show/hide the overlay; the LVGL active screen
 *     never changes, so there is nothing to restore on close.
 *   - Bottom 1/4: OPAQUE strip (theme track color) showing ONLY the current
 *     message (each new message replaces the previous one, no bubbles).
 *   - Middle-lower: emotion face (官方 Twemoji64, fixed position, no wander).
 *   - Tap anywhere / swipe-up-from-bottom / 5s rules (AppXiaoZhi) dismiss it.
 */
class Tab5BridgeLcdDisplay : public LcdDisplay {
public:
    Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height);
    ~Tab5BridgeLcdDisplay() override;

    void ActivateScreen();    // show the transparent overlay (LVGL active screen untouched)
    void DeactivateScreen();  // hide the overlay; underlying page reappears as-is
    bool IsOverlayShown() const {
        return overlay_ && !lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    }

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
    // 透明全屏悬浮容器 (lv_layer_top): 触摸拦截层 + 表情 + 底部实底对话条。
    // 隐藏时原 app 页面原样可见可操作。
    lv_obj_t* overlay_        = nullptr;

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
