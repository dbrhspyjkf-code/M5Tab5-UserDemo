#include "tab5_bridge_lcd.h"
#include <esp_log.h>

static const char* TAG = "Tab5BridgeLcd";

Tab5BridgeLcdDisplay::Tab5BridgeLcdDisplay(lv_display_t* existing_disp, int width, int height)
    : LcdDisplay(nullptr, nullptr, width, height)
{
    // Borrow the already-initialized LVGL display handle.
    // display_ is protected in LvglDisplay, so we can set it directly here.
    display_ = existing_disp;

    // Create xiaozhi's own screen so its UI is isolated from AppHome/AppHA.
    scr_ = lv_obj_create(NULL);
    lv_screen_load(scr_);  // make it active so SetupUI() puts objects here

    ESP_LOGI(TAG, "bridge display ready, w=%d h=%d scr=%p", width, height, scr_);
}

Tab5BridgeLcdDisplay::~Tab5BridgeLcdDisplay()
{
    // Null out the handles we don't own BEFORE ~LcdDisplay() runs.
    // ~LcdDisplay() checks for non-null before calling lv_display_delete /
    // esp_lcd_panel_del / esp_lcd_panel_io_del, so nulling prevents double-free.
    display_  = nullptr;
    panel_    = nullptr;
    panel_io_ = nullptr;
    // The LVGL UI objects (container_, status_bar_, etc.) are still deleted
    // by ~LcdDisplay() via lv_obj_del – that is correct and safe.
}

void Tab5BridgeLcdDisplay::ActivateScreen()
{
    if (scr_) {
        lv_screen_load(scr_);
    }
}
