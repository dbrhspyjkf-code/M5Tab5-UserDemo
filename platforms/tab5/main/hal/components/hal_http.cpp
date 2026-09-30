/*
 * ESP-IDF HTTP implementation using esp_http_client.
 * Thread-safe: each call creates its own client handle.
 */
#include "../hal_esp32.h"
#include <mooncake_log.h>
#include <esp_http_client.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

static const std::string _tag = "hal-http";

static bool _is_claude_gateway_url(const std::string& url)
{
    return url.find(":8769/api/claude/message") != std::string::npos
        || url.find(":8770/api/") != std::string::npos;
}

// email-status-server.py 在 8768 端口, 读 /tmp/email-status.json 缓存.
// 比 hermes IMAP 快 (<10ms), 1s timeout 足够; 给点余量用 3s.
static bool _is_slow_hermes_url(const std::string& url)
{
    return url.find(":8768/api/email/status") != std::string::npos;
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
    config.timeout_ms     = _is_slow_hermes_url(url) ? 30000 : 10000;
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
        mclog::tagWarn(_tag, "GET {} failed: {} (status={})", url,
                       esp_err_to_name(err), esp_http_client_get_status_code(client));
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

hal::HalBase::HttpResponse_t HalEsp32::httpPut(
    const std::string& url,
    const std::string& put_data,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    HttpResponse_t resp;
    std::string body;
    body.reserve(512);

    esp_http_client_config_t config = {};
    config.url            = url.c_str();
    config.method         = HTTP_METHOD_PUT;
    config.event_handler  = _http_event_handler;
    config.user_data      = &body;
    config.timeout_ms     = 10000;
    config.buffer_size    = 1024;
    config.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { mclog::tagWarn(_tag, "init failed"); return resp; }

    for (auto& [k, v] : headers)
        esp_http_client_set_header(client, k.c_str(), v.c_str());

    esp_http_client_set_post_field(client, put_data.c_str(), (int)put_data.size());

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
        resp.ok     = (resp.status >= 200 && resp.status < 300);
        resp.body   = std::move(body);
    } else {
        mclog::tagWarn(_tag, "PUT {} failed: {}", url, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return resp;
}

// ── Keep-alive POST for high-frequency callers ──────────────────────────────
// The mobile-base lease frames run at up to 20 Hz; a fresh TCP handshake per
// request dominated the round-trip time and dropped the short lease on Wi-Fi
// jitter. This path caches one esp_http_client per origin so the server's
// keep-alive connection is reused. Calls are serialized by mutex; the event
// handler reads a static body pointer that is valid only inside the lock.
static std::mutex s_keepalive_mutex;
static esp_http_client_handle_t s_keepalive_client = nullptr;
static std::string s_keepalive_origin;
static std::string* s_keepalive_body = nullptr;

static esp_err_t _keepalive_event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_keepalive_body && evt->data_len > 0)
        s_keepalive_body->append(static_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
}

static void _keepalive_cleanup()
{
    if (s_keepalive_client) {
        esp_http_client_cleanup(s_keepalive_client);
        s_keepalive_client = nullptr;
    }
    s_keepalive_origin.clear();
}

hal::HalBase::HttpResponse_t HalEsp32::httpPostKeepAlive(
    const std::string& url,
    const std::string& post_data,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    auto scheme = url.find("://");
    auto slash = (scheme == std::string::npos) ? std::string::npos
                                               : url.find('/', scheme + 3);
    const std::string origin =
        (slash == std::string::npos) ? url : url.substr(0, slash);

    std::lock_guard<std::mutex> lock(s_keepalive_mutex);
    if (s_keepalive_client && s_keepalive_origin != origin)
        _keepalive_cleanup();

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!s_keepalive_client) {
            esp_http_client_config_t config = {};
            config.url               = url.c_str();
            config.method            = HTTP_METHOD_POST;
            config.event_handler     = _keepalive_event_handler;
            config.timeout_ms        = 3000;
            config.buffer_size       = 1024;
            config.buffer_size_tx    = 2048;
            config.keep_alive_enable = true;
            s_keepalive_client = esp_http_client_init(&config);
            if (!s_keepalive_client) {
                mclog::tagWarn(_tag, "keep-alive init failed");
                _keepalive_cleanup();
                return {};
            }
            s_keepalive_origin = origin;
            for (auto& [k, v] : headers)
                esp_http_client_set_header(s_keepalive_client, k.c_str(), v.c_str());
        }

        std::string body;
        body.reserve(512);
        s_keepalive_body = &body;
        esp_http_client_set_url(s_keepalive_client, url.c_str());
        esp_http_client_set_post_field(s_keepalive_client, post_data.c_str(),
                                       static_cast<int>(post_data.size()));
        esp_err_t err = esp_http_client_perform(s_keepalive_client);
        s_keepalive_body = nullptr;
        if (err == ESP_OK) {
            HttpResponse_t resp;
            resp.status = esp_http_client_get_status_code(s_keepalive_client);
            resp.ok     = (resp.status >= 200 && resp.status < 300);
            resp.body   = std::move(body);
            return resp;
        }
        // Stale idle connection or transport error: rebuild once.
        mclog::tagWarn(_tag, "keep-alive POST {} failed: {} (attempt {})",
                       url, esp_err_to_name(err), attempt);
        _keepalive_cleanup();
    }
    return {};
}
