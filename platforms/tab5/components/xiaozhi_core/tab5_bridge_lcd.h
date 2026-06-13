#pragma once
#include "display/lcd_display.h"
#include <lvgl.h>

/**
 * Tab5BridgeLcdDisplay - wraps the lv_display_t* already created by
 * Tab5-UserDemo's BSP.  Does NOT reinitialize any LCD hardware or LVGL.
 *
 * Creates its own LVGL screen object so xiaozhi's UI lives on a dedicated
 * screen that AppXiaoZhi can show/hide via ActivateScreen().
 */
class Tab5BridgeLcdDisplay : public LcdDisplay {
public:
    Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height);
    ~Tab5BridgeLcdDisplay() override;

    /** Load xiaozhi's screen as the active LVGL screen. */
    void ActivateScreen();

    /** Return xiaozhi's dedicated screen object. */
    lv_obj_t* GetScreen() const { return scr_; }

private:
    lv_obj_t* scr_ = nullptr;
};
