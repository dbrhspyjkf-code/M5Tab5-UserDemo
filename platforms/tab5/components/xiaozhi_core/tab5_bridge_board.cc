#include "tab5_bridge_board.h"

// config.h uses these types but doesn't include their headers
#include <esp_lcd_ili9881c.h>       // ili9881c_lcd_init_cmd_t
#include "esp_lcd_st7123.h"         // st7123_lcd_init_cmd_t (m5stack_tab5 priv_include)

#include "boards/m5stack-tab5/config.h"
#include "codecs/dummy_audio_codec.h"

#include <bsp/m5stack_tab5.h>
#include <esp_log.h>

static const char* TAG = "Tab5BridgeBoard";

Tab5BridgeBoard::Tab5BridgeBoard()
{
    ESP_LOGI(TAG, "init — reusing BSP handles");

    // Borrow the I2C bus that Tab5-UserDemo's BSP already initialized.
    i2c_bus_ = bsp_i2c_get_handle();

    // Borrow the LVGL display that the BSP already registered.
    lv_display_t* existing_disp = bsp_display_get_lvgl_disp();
    // Tab5-UserDemo rotates 90°; xiaozhi expects portrait 720×1280.
    // The lv_display resolution after rotation is 1280×720 (landscape),
    // so pass width=1280, height=720 to match what LVGL reports.
    display_ = new Tab5BridgeLcdDisplay(existing_disp, DISPLAY_HEIGHT, DISPLAY_WIDTH);
}

Tab5BridgeBoard::~Tab5BridgeBoard()
{
    delete display_;
    display_ = nullptr;
}

void Tab5BridgeBoard::StartNetwork()
{
    ESP_LOGI(TAG, "reuse Tab5 hosted WiFi network");
    OnNetworkEvent(NetworkEvent::Connected, "hosted-wifi");
}

AudioCodec* Tab5BridgeBoard::GetAudioCodec()
{
    static DummyAudioCodec dummy_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE);
    return &dummy_codec;
}

Display* Tab5BridgeBoard::GetDisplay()
{
    return display_;
}

Backlight* Tab5BridgeBoard::GetBacklight()
{
    // Backlight is already initialized and on.  Return a static instance
    // so xiaozhi can adjust brightness without re-initializing the pin.
    static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    return &backlight;
}

// Register this as the board used by xiaozhi's Application singleton.
DECLARE_BOARD(Tab5BridgeBoard);
