/*
 * ESP32-P4 hardware JPEG decode → RGB565 buffer (PSRAM).
 * Used by the wallpaper screensaver to turn a downloaded Bing JPEG into an
 * LVGL-displayable image without a slow software decoder.
 */
#include "../hal_esp32.h"
#include <mooncake_log.h>
#include <driver/jpeg_decode.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const std::string _tag = "hal-jpeg";

uint8_t* HalEsp32::decodeJpegToRGB565(const std::string& filePath, int& outW, int& outH)
{
    outW = outH = 0;

    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
        mclog::tagWarn(_tag, "open {} failed", filePath);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long jpeg_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (jpeg_size <= 0) { fclose(f); return nullptr; }

    jpeg_decoder_handle_t engine = nullptr;
    uint8_t* tx_buf  = nullptr;  // compressed input (DMA)
    uint8_t* rx_buf  = nullptr;  // decoded RGB565 output (DMA/PSRAM)
    uint8_t* result  = nullptr;  // returned to caller

    // Input buffer: aligned DMA memory holding the raw JPEG bitstream.
    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
    size_t tx_buf_size = 0;
    tx_buf = (uint8_t*)jpeg_alloc_decoder_mem((size_t)jpeg_size, &tx_mem_cfg, &tx_buf_size);
    if (!tx_buf) { mclog::tagWarn(_tag, "tx alloc {} failed", jpeg_size); fclose(f); return nullptr; }
    if (fread(tx_buf, 1, (size_t)jpeg_size, f) != (size_t)jpeg_size) {
        mclog::tagWarn(_tag, "read failed");
        fclose(f);
        free(tx_buf);
        return nullptr;
    }
    fclose(f);

    // Probe dimensions from the JPEG header.
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(tx_buf, (uint32_t)jpeg_size, &info) != ESP_OK) {
        mclog::tagWarn(_tag, "get_info failed");
        free(tx_buf);
        return nullptr;
    }

    // Output buffer: W*H*2 bytes (RGB565), aligned DMA memory (lands in PSRAM).
    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    size_t rx_buf_size = 0;
    size_t need = (size_t)info.width * info.height * 2;
    rx_buf = (uint8_t*)jpeg_alloc_decoder_mem(need, &rx_mem_cfg, &rx_buf_size);
    if (!rx_buf) { mclog::tagWarn(_tag, "rx alloc {} failed", need); free(tx_buf); return nullptr; }

    jpeg_decode_engine_cfg_t eng_cfg = { .intr_priority = 0, .timeout_ms = 2000 };
    if (jpeg_new_decoder_engine(&eng_cfg, &engine) != ESP_OK) {
        mclog::tagWarn(_tag, "engine create failed");
        free(tx_buf);
        free(rx_buf);
        return nullptr;
    }

    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        // BGR element order: the panel/LVGL RGB565 expects B and R swapped
        // relative to the decoder's native RGB output (otherwise red↔blue).
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std      = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(engine, &dec_cfg, tx_buf, (uint32_t)jpeg_size,
                                         rx_buf, rx_buf_size, &out_size);
    jpeg_del_decoder_engine(engine);
    free(tx_buf);

    if (err != ESP_OK) {
        mclog::tagWarn(_tag, "decode failed: {}", esp_err_to_name(err));
        free(rx_buf);
        return nullptr;
    }

    outW   = (int)info.width;
    outH   = (int)info.height;
    result = rx_buf;
    mclog::tagInfo(_tag, "decoded {}x{} ({} bytes) from {}", outW, outH, out_size, filePath);
    return result;  // caller frees with free()
}
