// mail_tls 实现 — 见 mail_tls.h。esp_tls API 先例:
// platforms/tab5/patches/esp-ml307_esp_ssl.cc.patched (esp_tls_conn_new_sync /
// conn_write / conn_read / get_conn_sockfd / conn_destroy)。
#include "mail_tls.h"

#ifndef PLATFORM_BUILD_DESKTOP

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <cstring>

static const char* TAG = "mail-tls";

namespace mail_direct {

static bool tls_deadline_left(int64_t deadline_us)
{
    return esp_timer_get_time() < deadline_us;
}

MailTls::~MailTls()
{
    close();
}

bool MailTls::connect(const char* host, uint16_t port, int timeout_ms)
{
    close();
    _tls = esp_tls_init();
    if (!_tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return false;
    }
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = timeout_ms;
    int ret = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, _tls);
    if (ret != 1) {
        esp_tls_error_handle_t h;
        int code = 0, flags = 0;
        if (esp_tls_get_error_handle(_tls, &h) == ESP_OK)
            esp_tls_get_and_clear_last_error(h, &code, &flags);
        ESP_LOGE(TAG, "TLS connect %s:%u failed code=0x%x", host, port, code);
        esp_tls_conn_destroy(_tls);
        _tls = nullptr;
        return false;
    }
    _pos = _len = 0;
    set_sock_timeout((timeout_ms + 999) / 1000);
    return true;
}

void MailTls::set_sock_timeout(int sec)
{
    if (!_tls) return;
    int fd = -1;
    if (esp_tls_get_conn_sockfd(_tls, &fd) != ESP_OK || fd < 0) return;
    struct timeval tv = {sec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool MailTls::write_all(const char* data, size_t len)
{
    if (!_tls) return false;
    size_t sent = 0;
    while (sent < len) {
        int ret = esp_tls_conn_write(_tls, data + sent, len - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ)
            continue;
        if (ret <= 0) {
            ESP_LOGW(TAG, "tls write failed ret=%d errno=%d", ret, errno);
            return false;
        }
        sent += (size_t)ret;
    }
    return true;
}

size_t MailTls::fill(int deadline_ms)
{
    if (_pos > 0) {                    // 压缩残留
        memmove(_buf, _buf + _pos, _len - _pos);
        _len -= _pos;
        _pos = 0;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)deadline_ms * 1000;
    while (_len < sizeof(_buf)) {
        if (!tls_deadline_left(deadline)) return 0;
        int ret = esp_tls_conn_read(_tls, _buf + _len, sizeof(_buf) - _len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT) continue;   // SO_RCVTIMEO 兜底到期, 由 deadline 决断
        if (ret <= 0) {
            // 对端关闭且无数据 → 正常终止流 (server LOGOUT 后)
            ESP_LOGD(TAG, "tls read ret=%d", ret);
            return 0;
        }
        _len += (size_t)ret;
        return _len - _pos;
    }
    return 0;
}

bool MailTls::read_line(std::string* out, int deadline_ms, size_t max_len)
{
    out->clear();
    int64_t deadline = esp_timer_get_time() + (int64_t)deadline_ms * 1000;
    while (true) {
        // 找 '\n'
        for (size_t i = _pos; i < _len; i++) {
            if (_buf[i] == '\n') {
                size_t n = i - _pos;
                out->append(reinterpret_cast<const char*>(_buf + _pos),
                            n > 0 && _buf[i - 1] == '\r' ? n - 1 : n);
                _pos = i + 1;
                return true;
            }
        }
        if (_len - _pos >= max_len) return false;   // 异常超长行保护
        size_t avail = _len - _pos;
        out->append(reinterpret_cast<const char*>(_buf + _pos), avail);
        _pos = _len;
        out->reserve(out->size() + sizeof(_buf));
        if (!tls_deadline_left(deadline)) return false;
        int64_t left_us = deadline - esp_timer_get_time();
        if (fill((int)((left_us + 999) / 1000)) == 0) {
            if (_len == _pos) return false;         // 真超时/断流
            if (esp_timer_get_time() >= deadline) return false;
        }
    }
}

bool MailTls::read_bytes(uint8_t* buf, size_t n, int deadline_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)deadline_ms * 1000;
    size_t got = 0;
    while (got < n) {
        size_t avail = _len - _pos;
        if (avail > 0) {
            size_t take = avail < (n - got) ? avail : (n - got);
            memcpy(buf + got, _buf + _pos, take);
            _pos += take;
            got += take;
            continue;
        }
        if (!tls_deadline_left(deadline)) return false;
        int64_t left_us = deadline - esp_timer_get_time();
        if (fill((int)((left_us + 999) / 1000)) == 0 && esp_timer_get_time() >= deadline)
            return false;
    }
    return true;
}

void MailTls::close()
{
    if (_tls) {
        esp_tls_conn_destroy(_tls);
        _tls = nullptr;
    }
    _pos = _len = 0;
}

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
