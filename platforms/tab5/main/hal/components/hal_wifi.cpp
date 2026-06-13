/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <string.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>

// ─── WiFi credentials ─────────────────────────────────────────────────────────
#define WIFI_STA_SSID  "ChinaNet-11G"
#define WIFI_STA_PASS  "Blackbug225"
#define WIFI_MAX_RETRY 10

#define TAG "wifi"

// Defined in hal_esp32.cpp — SNTP time sync (needed for TLS to xiaozhi cloud).
void sync_network_time();

static EventGroupHandle_t s_wifi_event_group = nullptr;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_wifi_initialized = false;

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

    ESP_LOGI(TAG, "WiFi STA init: connecting to %s", WIFI_STA_SSID);

    // NVS required by WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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
    strncpy((char*)wifi_config.sta.ssid,     WIFI_STA_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_STA_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

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
