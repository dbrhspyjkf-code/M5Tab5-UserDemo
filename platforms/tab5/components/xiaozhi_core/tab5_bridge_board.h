#pragma once

#include "boards/common/wifi_board.h"
#include "tab5_bridge_lcd.h"
#include "boards/m5stack-tab5/tab5_audio_codec.h"
#include <driver/i2c_master.h>

/**
 * Tab5BridgeBoard - xiaozhi Board implementation that reuses hardware
 * already initialized by Tab5-UserDemo's BSP.
 *
 * - Display:  Tab5BridgeLcdDisplay wrapping bsp_display_get_lvgl_disp()
 * - I2C:      bsp_i2c_get_handle() (already initialized by BSP)
 * - Audio:    Tab5AudioCodec using the BSP I2C handle
 * - WiFi:     Inherited from WifiBoard (manages WiFi via NVS credentials)
 * - Touch:    Re-uses touch indev initialized by Tab5-UserDemo BSP
 */
class Tab5BridgeBoard : public WifiBoard {
public:
    Tab5BridgeBoard();
    ~Tab5BridgeBoard() override;

    std::string GetBoardType() override { return "m5stack-tab5"; }

    AudioCodec*  GetAudioCodec() override;
    Display*     GetDisplay()    override;
    Backlight*   GetBacklight()  override;

private:
    Tab5BridgeLcdDisplay* display_     = nullptr;
    i2c_master_bus_handle_t i2c_bus_   = nullptr;
};
