/*
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <string>

namespace webui {
void sleepMs(uint32_t ms);
// 把本帧累积的脏区发布给浏览器主线程 (rAF 侧读取绘制), 并切换 staging
void publishFrame();
}  // namespace webui

class HalWeb : public hal::HalBase {
public:
    std::string type() override
    {
        return "Web";
    }

    void init() override;

    void delay(uint32_t ms) override;
    uint32_t millis() override;
    int getCpuTemp() override;

    void setDisplayBrightness(uint8_t brightness) override;
    uint8_t getDisplayBrightness() override;

    void lvglLock() override;
    void lvglUnlock() override;

    void setSpeakerVolume(uint8_t volume) override;
    uint8_t getSpeakerVolume() override;

    // Persistent config — 设备 NVS 的 web 替身: 经同步 XHR 读写网关
    // /gw/prefs (服务端按用户持久化, 主题等设置跨浏览器一致)。
    std::string getConfig(const std::string& key, const std::string& defaultValue = "") override;
    void setConfig(const std::string& key, const std::string& value) override;

    HttpResponse_t httpGet(const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers = {}) override;
    HttpResponse_t httpPost(const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers = {}) override;
    HttpResponse_t httpPut(const std::string& url, const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers = {}) override;

private:
    uint8_t _current_lcd_brightness = 100;
    uint8_t _current_speaker_volume = 20;

    void lvgl_init();
};
