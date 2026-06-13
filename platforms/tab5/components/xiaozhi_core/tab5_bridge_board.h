#pragma once

#include "boards/common/wifi_board.h"
#include "tab5_bridge_lcd.h"
#include <driver/i2c_master.h>

/**
 * Tab5BridgeBoard - xiaozhi Board implementation that reuses hardware
 * already initialized by Tab5-UserDemo's BSP.
 *
 * - Display:  Tab5BridgeLcdDisplay wrapping bsp_display_get_lvgl_disp()
 * - I2C:      bsp_i2c_get_handle() (already initialized by BSP)
 * - Audio:    Real ES8388/ES7210 codec on BSP I2C bus (I2S_NUM_0 free)
 * - WiFi:     Reuses Tab5-UserDemo's already-started hosted WiFi
 * - Touch:    Re-uses touch indev initialized by Tab5-UserDemo BSP
 */
class Tab5BridgeBoard : public WifiBoard {
public:
    Tab5BridgeBoard();
    ~Tab5BridgeBoard() override;

    std::string GetBoardType() override { return "m5stack-tab5"; }

    void StartNetwork() override;
    AudioCodec*  GetAudioCodec() override;
    Display*     GetDisplay()    override;
    Backlight*   GetBacklight()  override;

private:
    Tab5BridgeLcdDisplay* display_     = nullptr;
    i2c_master_bus_handle_t i2c_bus_   = nullptr;
};
