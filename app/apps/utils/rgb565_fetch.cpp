#include "rgb565_fetch.h"
#include <hal/hal.h>
#include <cstdlib>
#include <cstring>

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

extern "C" int web_jpeg_to_rgb565(const uint8_t* jpg, int jpg_len, uint16_t* out, int out_bytes);

namespace rgb565 {

void* alloc(int w, int h)
{
    const size_t bytes = (size_t)w * h * 2;
#ifndef PLATFORM_BUILD_DESKTOP
    void* b = heap_caps_malloc(sizeof(lv_image_dsc_t) + bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void* b = malloc(sizeof(lv_image_dsc_t) + bytes);
#endif
    if (!b) return nullptr;
    auto* dsc = static_cast<lv_image_dsc_t*>(b);
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data_size = bytes;
    dsc->data = static_cast<const uint8_t*>(b) + sizeof(lv_image_dsc_t);
    return b;
}

void release(void* buf)
{
    if (!buf) return;
#ifndef PLATFORM_BUILD_DESKTOP
    heap_caps_free(buf);
#else
    ::free(buf);
#endif
}

bool download(const std::string& url, void* buf)
{
    auto* dsc = static_cast<lv_image_dsc_t*>(buf);
    const int bytes = (int)dsc->data_size;
    char* dst = const_cast<char*>(reinterpret_cast<const char*>(dsc->data));

#if defined(PLATFORM_BUILD_WEB)
    // web: 拉 JPEG (~1/10 流量) → worker OffscreenCanvas 解码 → RGB565 写入
    std::string jpg_url = url;
    const size_t dot = jpg_url.rfind(".rgb565");
    // ".rgb565" 是 7 字符 —— 曾误替换 8 字符把 '?' 吃掉 (thumb.jpgid= → 404 全挂)
    if (dot != std::string::npos) jpg_url.replace(dot, 7, ".jpg");
    auto resp = GetHAL()->httpGet(jpg_url);
    if (!resp.ok || resp.status != 200 || resp.body.empty()) return false;
    // 解码: 返回像素数 (w*h=成功)
    return ::web_jpeg_to_rgb565((const uint8_t*)resp.body.data(), (int)resp.body.size(),
                                (uint16_t*)dst, bytes) == bytes / 2;
#elif defined(PLATFORM_BUILD_DESKTOP)
    auto resp = GetHAL()->httpGet(url);
    if (!resp.ok || resp.status != 200 || (int)resp.body.size() != bytes) return false;
    memcpy(dst, resp.body.data(), bytes);
    return true;
#else
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return false;
    bool ok = false;
    if (esp_http_client_open(http, 0) == ESP_OK) {
        esp_http_client_fetch_headers(http);
        if (esp_http_client_get_status_code(http) == 200) {
            int len = 0, n;
            while ((n = esp_http_client_read(http, dst + len, bytes - len)) > 0) {
                len += n;
                if (len >= bytes) break;
                if ((len & 0x7FFF) < n) vTaskDelay(pdMS_TO_TICKS(2));   // 每跨 32KB 边界让 2ms
            }
            ok = (len == bytes);
        }
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    return ok;
#endif
}

}  // namespace rgb565
