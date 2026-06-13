#include "tab5_bridge_board.h"

// config.h uses these types but doesn't include their headers
#include <esp_lcd_ili9881c.h>       // ili9881c_lcd_init_cmd_t
#include "esp_lcd_st7123.h"         // st7123_lcd_init_cmd_t (m5stack_tab5 priv_include)

#include "boards/m5stack-tab5/config.h"
#include "tab5_audio_codec.h"       // real ES8388/ES7210 codec on Tab5
#include <es8388_codec.h>           // ES8388_CODEC_DEFAULT_ADDR (0x20)

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
    // Real ES8388 (DAC/speaker) + ES7210 (ADC/mic) on the BSP I2C bus.
    // BSP does NOT init the codec (bsp_codec_init is disabled), so I2S_NUM_0
    // is free for xiaozhi. Chip is ES8388 @ 0x20 — config.h's ES8311 addr is wrong.
    static Tab5AudioCodec codec(
        i2c_bus_,
        AUDIO_INPUT_SAMPLE_RATE,
        AUDIO_OUTPUT_SAMPLE_RATE,
        AUDIO_I2S_GPIO_MCLK,
        AUDIO_I2S_GPIO_BCLK,
        AUDIO_I2S_GPIO_WS,
        AUDIO_I2S_GPIO_DOUT,
        AUDIO_I2S_GPIO_DIN,
        AUDIO_CODEC_PA_PIN,
        ES8388_CODEC_DEFAULT_ADDR,   // 0x20 (live I2C scan confirmed 0x10 7-bit)
        AUDIO_CODEC_ES7210_ADDR,     // ES7210_CODEC_DEFAULT_ADDR 0x80
        AUDIO_INPUT_REFERENCE
    );
    return &codec;
}

Display* Tab5BridgeBoard::GetDisplay()
{
    return display_;
}

Backlight* Tab5BridgeBoard::GetBacklight()
{
    // Return nullptr: the BSP already drives the backlight on GPIO22 and keeps
    // it on. Constructing xiaozhi's PwmBacklight here re-inits LEDC on the same
    // pin ("ledc: GPIO 22 is not usable" conflict) and ends up turning the
    // screen OFF (black) shortly after MCP init calls GetBacklight(). All
    // xiaozhi callers null-check GetBacklight(), so nullptr is safe.
    return nullptr;
}

// Register this as the board used by xiaozhi's Application singleton.
DECLARE_BOARD(Tab5BridgeBoard);
