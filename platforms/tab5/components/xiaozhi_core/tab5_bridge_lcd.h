#pragma once
#include "display/lcd_display.h"
#include <lvgl.h>

/**
 * Tab5BridgeLcdDisplay - wraps the lv_display_t* already created by
 * Tab5-UserDemo's BSP.  Does NOT reinitialize any LCD hardware or LVGL.
 *
 * Instead of xiaozhi's heavy LcdDisplay UI (status bar + emoji + chat
 * container — dozens of LVGL objects that fail to build under the tight
 * internal-RAM budget when the HA dashboard is also running, leaving a black
 * screen), this builds a tiny, robust UI: one status line + one wrapping chat
 * label, plus tap-to-talk on the whole screen. The LcdDisplay methods that
 * touch the heavy widgets are overridden so they update these simple labels
 * (or no-op) and never dereference the null base-class widgets.
 */
class Tab5BridgeLcdDisplay : public LcdDisplay {
public:
    Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height);
    ~Tab5BridgeLcdDisplay() override;

    /** Load xiaozhi's screen as the active LVGL screen. */
    void ActivateScreen();

    /** Return xiaozhi's dedicated screen object. */
    lv_obj_t* GetScreen() const { return scr_; }

    // ── Lightweight UI overrides (avoid the heavy/black LcdDisplay UI) ──
    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void SetTheme(Theme* theme) override;

private:
    void BuildSimpleUi();   // builds scr_ + labels (called from ctor)

    lv_obj_t* scr_        = nullptr;
    lv_obj_t* status_lbl_ = nullptr;
    lv_obj_t* chat_lbl_   = nullptr;
};
