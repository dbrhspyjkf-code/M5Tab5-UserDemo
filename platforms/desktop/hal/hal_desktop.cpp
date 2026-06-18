/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal_desktop.h"
#include <SDL2/SDL.h>
#include <mooncake_log.h>
#include <random>
#include <filesystem>
#include <thread>
#include <curl/curl.h>

static const std::string _tag = "hal";

void HalDesktop::init()
{
    mclog::tagInfo(_tag, "init");
    lvgl_init();
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
void HalDesktop::delay(uint32_t ms)
{
    SDL_Delay(ms);
}

uint32_t HalDesktop::millis()
{
    return SDL_GetTicks();
}

int HalDesktop::getCpuTemp()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.99, 1.01);

    return dis(gen) * 45.0f;
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */
void HalDesktop::setDisplayBrightness(uint8_t brightness)
{
    _current_lcd_brightness = std::clamp((int)brightness, 0, 100);
    mclog::tagInfo(_tag, "set display brightness: {}%", _current_lcd_brightness);
}

uint8_t HalDesktop::getDisplayBrightness()
{
    return _current_lcd_brightness;
}

/* -------------------------------------------------------------------------- */
/*                                Power monitor                               */
/* -------------------------------------------------------------------------- */
void HalDesktop::updatePowerMonitorData()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(0.95, 1.0);

    powerMonitorData.busVoltage   = dis(gen) * 8.0f;
    powerMonitorData.shuntCurrent = -dis(gen) * 0.5f;
}

/* -------------------------------------------------------------------------- */
/*                                     IMU                                    */
/* -------------------------------------------------------------------------- */
void HalDesktop::updateImuData()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<> dis(-0.1, 0.1);

    imuData.accelX = dis(gen);
    imuData.accelY = dis(gen);
    imuData.accelZ = dis(gen);
}

/* -------------------------------------------------------------------------- */
/*                                    Power                                   */
/* -------------------------------------------------------------------------- */
void HalDesktop::setChargeQcEnable(bool enable)
{
    _charge_qc_enable = enable;
    mclog::tagInfo(_tag, "set charge qc enable: {}", _charge_qc_enable);
}

bool HalDesktop::getChargeQcEnable()
{
    return _charge_qc_enable;
}

void HalDesktop::setChargeEnable(bool enable)
{
    _charge_enable = enable;
    mclog::tagInfo(_tag, "set charge enable: {}", _charge_enable);
}

bool HalDesktop::getChargeEnable()
{
    return _charge_enable;
}

void HalDesktop::setUsb5vEnable(bool enable)
{
    _usba_5v_enable = enable;
    mclog::tagInfo(_tag, "set usb5v enable: {}", _usba_5v_enable);
}

bool HalDesktop::getUsb5vEnable()
{
    return _usba_5v_enable;
}

void HalDesktop::setExt5vEnable(bool enable)
{
    _ext_5v_enable = enable;
    mclog::tagInfo(_tag, "set ext5v enable: {}", _ext_5v_enable);
}

bool HalDesktop::getExt5vEnable()
{
    return _ext_5v_enable;
}

void HalDesktop::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(_tag, "set ext antenna enable: {}", _ext_antenna_enable);
}

bool HalDesktop::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

/* -------------------------------------------------------------------------- */
/*                                   SD card                                  */
/* -------------------------------------------------------------------------- */
bool HalDesktop::isSdCardMounted()
{
    return true;
}

std::vector<hal::HalBase::FileEntry_t> HalDesktop::scanSdCard(const std::string& dirPath)
{
    std::filesystem::path path(dirPath);
    std::vector<hal::HalBase::FileEntry_t> file_entries;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        file_entries.push_back({entry.path().filename().string(), entry.is_directory()});
    }
    return file_entries;
}

/* -------------------------------------------------------------------------- */
/*                                  Interface                                 */
/* -------------------------------------------------------------------------- */
bool HalDesktop::usbCDetect()
{
    // return rand() % 2 == 0;
    return false;
}

bool HalDesktop::usbADetect()
{
    // return rand() % 2 == 0;
    return false;
}

bool HalDesktop::headPhoneDetect()
{
    // return rand() % 2 == 0;
    return false;
}

std::vector<uint8_t> HalDesktop::i2cScan(bool isInternal)
{
    // Random 6 addrs
    std::vector<uint8_t> addrs;
    for (int i = 0; i < 6; i++) {
        addrs.push_back(rand() % 128);
    }
    return addrs;
}

/* -------------------------------------------------------------------------- */
/*                                UART monitor                                */
/* -------------------------------------------------------------------------- */
void HalDesktop::uartMonitorSend(std::string msg, bool newLine)
{
    static bool is_test_thread_running = false;
    if (is_test_thread_running) {
        return;
    }

    is_test_thread_running = true;
    std::thread([&]() {
        for (int i = 0; i < 6; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::lock_guard<std::mutex> lock(GetHAL()->uartMonitorData.mutex);
            std::string send_msg = "[2025-04-24 14:32:38.111] [info] [panel-com] recv msg: 32\n";
            mclog::tagInfo(_tag, "send msg: {}", send_msg);
            for (auto c : send_msg) {
                GetHAL()->uartMonitorData.rxQueue.push(c);
            }
        }
        is_test_thread_running = false;
    }).detach();
}

// ── HTTP (libcurl) ────────────────────────────────────────────────────────────

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

hal::HalBase::HttpResponse_t HalDesktop::httpGet(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    CURL* curl = curl_easy_init();
    if (!curl) return resp;

    std::string body;
    struct curl_slist* hdr_list = nullptr;
    for (auto& [k, v] : headers)
        hdr_list = curl_slist_append(hdr_list, (k + ": " + v).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status = (int)code;
        resp.ok     = (resp.status >= 200 && resp.status < 300);
        resp.body   = std::move(body);
    }

    curl_slist_free_all(hdr_list);
    curl_easy_cleanup(curl);
    return resp;
}

hal::HalBase::HttpResponse_t HalDesktop::httpPost(
    const std::string& url,
    const std::string& post_data,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    CURL* curl = curl_easy_init();
    if (!curl) return resp;

    std::string body;
    struct curl_slist* hdr_list = nullptr;
    for (auto& [k, v] : headers)
        hdr_list = curl_slist_append(hdr_list, (k + ": " + v).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)post_data.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 240L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status = (int)code;
        resp.ok     = (resp.status >= 200 && resp.status < 300);
        resp.body   = std::move(body);
    }

    curl_slist_free_all(hdr_list);
    curl_easy_cleanup(curl);
    return resp;
}
