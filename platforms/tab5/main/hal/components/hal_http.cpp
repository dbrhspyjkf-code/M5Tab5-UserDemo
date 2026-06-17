/*
 * ESP-IDF HTTP implementation using esp_http_client.
 * Thread-safe: each call creates its own client handle.
 */
#include "../hal_esp32.h"
#include <mooncake_log.h>
#include <esp_http_client.h>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>

static const std::string _tag = "hal-http";

static bool _is_claude_gateway_url(const std::string& url)
{
    return url.find(":8769/api/claude/message") != std::string::npos
        || url.find(":8770/api/") != std::string::npos;
}

static esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    auto* body = static_cast<std::string*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && body && evt->data_len > 0)
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
}

hal::HalBase::HttpResponse_t HalEsp32::httpGet(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    std::string body;
    body.reserve(2048);

    esp_http_client_config_t config = {};
    config.url            = url.c_str();
    config.event_handler  = _http_event_handler;
    config.user_data      = &body;
    config.timeout_ms     = 10000;
    config.buffer_size    = 2048;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { mclog::tagWarn(_tag, "init failed"); return resp; }

    for (auto& [k, v] : headers)
        esp_http_client_set_header(client, k.c_str(), v.c_str());

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
        resp.ok     = (resp.status >= 200 && resp.status < 300);
        resp.body   = std::move(body);
    } else {
        mclog::tagWarn(_tag, "GET {} failed: {}", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return resp;
}

hal::HalBase::HttpResponse_t HalEsp32::httpPost(
    const std::string& url,
    const std::string& post_data,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    std::string body;
    body.reserve(512);

    esp_http_client_config_t config = {};
    config.url            = url.c_str();
    config.method         = HTTP_METHOD_POST;
    config.event_handler  = _http_event_handler;
    config.user_data      = &body;
    config.timeout_ms     = _is_claude_gateway_url(url) ? 240000 : 10000;
    config.buffer_size    = 1024;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { mclog::tagWarn(_tag, "init failed"); return resp; }

    for (auto& [k, v] : headers)
        esp_http_client_set_header(client, k.c_str(), v.c_str());

    esp_http_client_set_post_field(client, post_data.c_str(), (int)post_data.size());

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
        resp.ok     = (resp.status >= 200 && resp.status < 300);
        resp.body   = std::move(body);
    } else {
        mclog::tagWarn(_tag, "POST {} failed: {}", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return resp;
}
