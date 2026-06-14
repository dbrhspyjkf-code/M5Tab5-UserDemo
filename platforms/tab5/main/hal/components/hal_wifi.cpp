/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <string.h>
#include <algorithm>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>

// ─── WiFi credentials (compile-time defaults) ─────────────────────────────────
// These are only used the very first time, before the user sets WiFi on-screen.
// Runtime values live in NVS ("devcfg" namespace) and override these.
#define WIFI_STA_SSID  "ChinaNet-11G"
#define WIFI_STA_PASS  "Blackbug225"
#define WIFI_MAX_RETRY 10

// NVS namespace + keys for runtime device config (WiFi + HA server, etc.)
#define DEVCFG_NS "devcfg"

#define TAG "wifi"

// True once the STA has an IP. Defined here (early) so isWifiConnected() and
// the event handler below can both see it.
static bool s_wifi_initialized = false;

// Make sure NVS flash is initialized exactly once before any nvs_open.
static void ensure_nvs_ready()
{
    static bool s_nvs_ready = false;
    if (s_nvs_ready) return;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    s_nvs_ready = true;
}

std::string HalEsp32::getConfig(const std::string& key, const std::string& defaultValue)
{
    ensure_nvs_ready();
    nvs_handle_t h;
    if (nvs_open(DEVCFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return defaultValue;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, key.c_str(), nullptr, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return defaultValue;
    }
    std::string out(len, '\0');
    err = nvs_get_str(h, key.c_str(), out.data(), &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return defaultValue;
    }
    if (!out.empty() && out.back() == '\0') out.pop_back();  // drop NUL terminator
    return out;
}

void HalEsp32::setConfig(const std::string& key, const std::string& value)
{
    ensure_nvs_ready();
    nvs_handle_t h;
    if (nvs_open(DEVCFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "setConfig: nvs_open failed for %s", key.c_str());
        return;
    }
    nvs_set_str(h, key.c_str(), value.c_str());
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "setConfig: %s = %s", key.c_str(), key == "wifi_pass" ? "***" : value.c_str());
}

void HalEsp32::reboot()
{
    ESP_LOGI(TAG, "reboot requested");
    esp_restart();
}

bool HalEsp32::isWifiConnected()
{
    return s_wifi_initialized;
}

std::vector<hal::HalBase::WifiAp_t> HalEsp32::wifiScan()
{
    std::vector<WifiAp_t> result;

    // STA must be started (wifi_init() does this at boot). A blocking scan works
    // even while connected to an AP.
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifiScan: scan_start failed: %s", esp_err_to_name(err));
        return result;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return result;
    if (num > 32) num = 32;

    std::vector<wifi_ap_record_t> records(num);
    if (esp_wifi_scan_get_ap_records(&num, records.data()) != ESP_OK) {
        return result;
    }

    // Dedupe by SSID, keeping the strongest. Skip empty SSIDs.
    for (uint16_t i = 0; i < num; i++) {
        const char* ssid = (const char*)records[i].ssid;
        if (ssid[0] == '\0') continue;
        auto it = std::find_if(result.begin(), result.end(),
            [&](const WifiAp_t& a) { return a.ssid == ssid; });
        bool locked = records[i].authmode != WIFI_AUTH_OPEN;
        if (it == result.end()) {
            result.push_back({ssid, records[i].rssi, locked});
        } else if (records[i].rssi > it->rssi) {
            it->rssi = records[i].rssi;
            it->locked = locked;
        }
    }
    std::sort(result.begin(), result.end(),
        [](const WifiAp_t& a, const WifiAp_t& b) { return a.rssi > b.rssi; });
    ESP_LOGI(TAG, "wifiScan: %d unique APs", (int)result.size());
    return result;
}

// Defined in hal_esp32.cpp — SNTP time sync (needed for TLS to xiaozhi cloud).
void sync_network_time();

static EventGroupHandle_t s_wifi_event_group = nullptr;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_num < WIFI_MAX_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "retry WiFi connect (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGW(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool HalEsp32::wifi_init()
{
    if (s_wifi_initialized) return true;

    // Runtime credentials from NVS, falling back to the compile-time defaults
    // the first time (before the user sets WiFi via the on-screen Settings app).
    std::string ssid = getConfig("wifi_ssid", WIFI_STA_SSID);
    std::string pass = getConfig("wifi_pass", WIFI_STA_PASS);
    ESP_LOGI(TAG, "WiFi STA init: connecting to %s", ssid.c_str());

    // NVS required by WiFi (getConfig above already ensured init, but be safe).
    ensure_nvs_ready();

    s_wifi_event_group = xEventGroupCreate();

    // netif and event loop may already be initialized by BSP
    esp_err_t ni = esp_netif_init();
    if (ni != ESP_OK && ni != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(ni);

    esp_err_t el = esp_event_loop_create_default();
    if (el != ESP_OK && el != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(el);

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any;
    esp_event_handler_instance_t inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, nullptr, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, nullptr, &inst_ip));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid,     ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, pass.c_str(), sizeof(wifi_config.sta.password) - 1);
    // Open networks have an empty password; don't force WPA2-only in that case.
    wifi_config.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait up to 15s for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(15000));

    esp_event_handler_instance_unregister(IP_EVENT,  IP_EVENT_STA_GOT_IP, inst_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,   inst_any);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;

    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;
    s_wifi_initialized = ok;

    if (ok) {
        ESP_LOGI(TAG, "WiFi connected");
        // Sync system clock via SNTP. Without this the RTC reads year 2002,
        // which breaks TLS cert validation (e.g. api.tenclass.net for xiaozhi).
        // Also serves as a DNS/internet reachability check (uses ntp.aliyun.com).
        sync_network_time();
    } else {
        ESP_LOGW(TAG, "WiFi NOT connected");
    }
    return ok;
}

void HalEsp32::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(TAG, "set ext antenna enable: {}", _ext_antenna_enable);
    bsp_set_ext_antenna_enable(_ext_antenna_enable);
}

bool HalEsp32::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

void HalEsp32::startWifiAp()
{
    // STA mode only — AP not used
    wifi_init();
}
