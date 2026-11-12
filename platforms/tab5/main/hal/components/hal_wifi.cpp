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
#include <freertos/semphr.h>
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
// Leave empty — configure via Settings app (stored in NVS).
#define WIFI_STA_SSID  ""
#define WIFI_STA_PASS  ""

// NVS namespace + keys for runtime device config (WiFi + HA server, etc.)
#define DEVCFG_NS "devcfg"

#define TAG "wifi"

// Live "STA has an IP" status — flipped true/false by the event handler below
// so isWifiConnected() reflects reality across router outages.
static bool s_wifi_initialized = false;
static bool s_wifi_connected = false;
// Set once the esp_wifi_init/start boot sequence has run (init-once guard;
// re-running wifi_init() would esp_wifi_init() twice → ESP_ERROR_CHECK abort).
static bool s_wifi_started = false;

// ─── Auto-reconnect ───────────────────────────────────────────────────────────
// esp_wifi does NOT reconnect by itself after a router reboot / AP loss, and a
// plain "retry N times" handler gives up during longer outages (2026-11-09:
// WiFi stayed dead forever after router restart). So: the event handler stays
// registered forever and a persistent worker owns reconnecting — a few fast
// retries for transient drops, then a slow 30 s cadence indefinitely. Every
// successful reconnect also re-syncs SNTP (RTC drift / TLS cert validity).
static SemaphoreHandle_t s_disc_sem = nullptr;   // DISCONNECTED → worker token
#define WIFI_FAST_RETRIES 5
#define WIFI_FAST_RETRY_MS 2000
#define WIFI_SLOW_RETRY_MS 30000

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
    bool is_secret = (key == "wifi_pass" || key == "ha_token");
    ESP_LOGI(TAG, "setConfig: %s = %s", key.c_str(), is_secret ? "***" : value.c_str());
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

// Persistent reconnect worker — see comment above s_disc_sem. Runs forever;
// woken by a token from the DISCONNECTED handler, retries until GOT_IP.
static void wifi_reconnect_worker()
{
    int fast_left = WIFI_FAST_RETRIES;
    for (;;) {
        if (xSemaphoreTake(s_disc_sem, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_disc_sem, 0);   // collapse burst of disconnect tokens
        int attempt = 0;
        while (!s_wifi_connected) {
            esp_err_t err = esp_wifi_connect();
            attempt++;
            int wait_ms = (fast_left > 0) ? WIFI_FAST_RETRY_MS : WIFI_SLOW_RETRY_MS;
            if (fast_left > 0) fast_left--;
            ESP_LOGI(TAG, "reconnect attempt %d (next in %ds, %s)", attempt,
                     wait_ms / 1000, esp_err_to_name(err));
            int waited = 0;
            while (!s_wifi_connected && waited < wait_ms) {
                vTaskDelay(pdMS_TO_TICKS(500));
                waited += 500;
            }
            if (s_wifi_connected) break;
            xSemaphoreTake(s_disc_sem, 0);   // drop stale tokens before next try
        }
        ESP_LOGI(TAG, "wifi reconnected, resync time");
        fast_left = WIFI_FAST_RETRIES;
        sync_network_time();
    }
}

// ─── 网络哑死看门狗 ───
// 重连 worker 只认 DISCONNECTED 事件, 但 esp_hosted SDIO 传输层可能在堆耗尽
// 丢包后静默僵死: 名义在线 (s_wifi_connected 一直 true)、IP 还挂着, 实际所有
// TCP 超时、连 ARP 都不回 (2026-11-12: 哑死 30+ 分钟直到手动复位)。这里兜底:
// WiFi 在线 + 近 4 分钟内有人发过 HTTP 请求 + 5 分钟没有任何一次完整响应 →
// esp_restart 自愈 (复位后 esp_hosted 全链重初始化, 每次实测 ~14s 拿回 IP;
// SDIO 断言软化补丁把"OOM 即砖"改成"OOM 丢包", 本看门狗再把"丢包后哑死"
// 收敛为"哑死 5 分钟即自愈重启")。要求 ok_tick 非零 (开机后至少成功过一次),
// 避免"服务端一直关机"被误判成传输层死亡而无限重启循环。
#define NET_WDT_CHECK_MS     30000
#define NET_WDT_DEAD_MS      300000
#define NET_WDT_ATTEMPT_MS   240000
static void network_liveness_worker()
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NET_WDT_CHECK_MS));
        if (!s_wifi_connected) continue;   // 断线归重连 worker 管
        uint32_t now = xTaskGetTickCount();
        uint32_t ok  = HalEsp32::net_last_ok_tick.load();
        uint32_t att = HalEsp32::net_last_attempt_tick.load();
        if (att == 0 || ok == 0) continue; // 还没流量 / 从未成功过: 不判死
        int32_t since_ok  = (int32_t)(now - ok);   // 32 位 tick 回绕安全
        int32_t since_att = (int32_t)(now - att);
        if (since_att < 0 || since_att > (int32_t)pdMS_TO_TICKS(NET_WDT_ATTEMPT_MS))
            continue;   // 近 4 分钟没人发请求 = 设备在睡, 不是网络死了
        if (since_ok > (int32_t)pdMS_TO_TICKS(NET_WDT_DEAD_MS)) {
            ESP_LOGW(TAG, "liveness watchdog: no HTTP success for %ds despite "
                          "attempts — esp_restart self-heal",
                     (int)(since_ok / configTICK_RATE_HZ));
            vTaskDelay(pdMS_TO_TICKS(3000));   // 给日志留冲刷时间
            esp_restart();
        }
    }
}

static void sta_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            auto* d = (wifi_event_sta_disconnected_t*)event_data;
            ESP_LOGW(TAG, "STA disconnected, reason=%d", d ? d->reason : -1);
            s_wifi_connected = false;
            s_wifi_initialized = false;   // isWifiConnected() reflects reality
            if (s_disc_sem) xSemaphoreGive(s_disc_sem);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wifi_initialized = true;
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool HalEsp32::wifi_init()
{
    if (s_wifi_started) return s_wifi_initialized;

    // Runtime credentials from NVS, falling back to the compile-time defaults
    // the first time (before the user sets WiFi via the on-screen Settings app).
    std::string ssid = getConfig("wifi_ssid", WIFI_STA_SSID);
    std::string pass = getConfig("wifi_pass", WIFI_STA_PASS);
    ESP_LOGI(TAG, "WiFi STA init: connecting to %s", ssid.c_str());

    // NVS required by WiFi (getConfig above already ensured init, but be safe).
    ensure_nvs_ready();

    // Reconnect plumbing must exist before esp_wifi_start(): the first
    // DISCONNECTED can arrive within milliseconds (router down at boot).
    s_disc_sem = xSemaphoreCreateBinary();
    if (!tryRunDetached(wifi_reconnect_worker, 16384)) {
        ESP_LOGE(TAG, "reconnect worker spawn failed — WiFi won't self-heal");
    }
    if (!tryRunDetached(network_liveness_worker, 16384)) {
        ESP_LOGE(TAG, "network liveness worker spawn failed — no wedge self-heal");
    }

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

    // Handlers stay registered for the whole session — the reconnect worker
    // depends on DISCONNECTED/GOT_IP events after wifi_init() returns.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid,     ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, pass.c_str(), sizeof(wifi_config.sta.password) - 1);
    // Open networks have an empty password; don't force WPA2-only in that case.
    wifi_config.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_wifi_started = true;
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait up to 15s for connection (the worker keeps retrying after this)
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(15000));

    // Boot wait is over; the handler keeps tracking state, only the group goes.
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;

    bool ok = s_wifi_connected;   // set by the GOT_IP handler above
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
