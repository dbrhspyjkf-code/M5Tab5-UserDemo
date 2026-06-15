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

static esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    auto* body = static_cast<std::string*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && body && evt->data_len > 0)
        body->append(static_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
}

// Streaming download: write each chunk straight to an open FILE* so a large
// (~hundreds of KB) response never lives fully in RAM.
struct _FileSink {
    FILE*  fp    = nullptr;
    size_t bytes = 0;
    bool   error = false;
};

static esp_err_t _http_file_event_handler(esp_http_client_event_t* evt)
{
    auto* sink = static_cast<_FileSink*>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && sink && sink->fp && evt->data_len > 0) {
        size_t n = fwrite(evt->data, 1, evt->data_len, sink->fp);
        if (n != (size_t)evt->data_len) sink->error = true;
        sink->bytes += n;
    }
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
    config.timeout_ms     = 10000;
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

bool HalEsp32::httpGetToFile(
    const std::string& url,
    const std::string& filePath,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    _FileSink sink;
    sink.fp = fopen(filePath.c_str(), "wb");
    if (!sink.fp) {
        mclog::tagWarn(_tag, "open {} for write failed", filePath);
        return false;
    }

    esp_http_client_config_t config = {};
    config.url            = url.c_str();
    config.event_handler  = _http_file_event_handler;
    config.user_data      = &sink;
    config.timeout_ms     = 20000;
    config.buffer_size    = 4096;
    config.buffer_size_tx = 1024;
    // Plain HTTP only — no TLS. The worker thread runs on a small (8 KB) stack
    // that a mbedtls handshake would overflow, and Bing serves these URLs over
    // http without forcing https, so no cert bundle is needed.

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        mclog::tagWarn(_tag, "init failed");
        fclose(sink.fp);
        remove(filePath.c_str());
        return false;
    }

    for (auto& [k, v] : headers)
        esp_http_client_set_header(client, k.c_str(), v.c_str());

    esp_err_t err = esp_http_client_perform(client);
    int  status   = esp_http_client_get_status_code(client);
    bool ok       = (err == ESP_OK) && (status >= 200 && status < 300) && !sink.error;

    esp_http_client_cleanup(client);
    fclose(sink.fp);

    if (!ok) {
        mclog::tagWarn(_tag, "GET->file {} failed: err={} status={} bytes={}",
                       url, esp_err_to_name(err), status, sink.bytes);
        remove(filePath.c_str());
        return false;
    }

    mclog::tagInfo(_tag, "downloaded {} bytes -> {}", sink.bytes, filePath);
    return true;
}
