// mail_tls — 邮件直连 TLS 封装 (esp-tls raw 连接, IMAP 993 / SMTP 465 隐式 TLS)。
//
// 参照 oneapp mail.py: imaplib.IMAP4_SSL(timeout=12) / SMTP_SSL(timeout=15)。
// 超时语义: 读写用「命令级 deadline」(esp_timer) 控制 — 一次 cmd 内任何一次
// 底层 read 超过 deadline 即失败; 另设 SO_RCVTIMEO 兜底防 recv 永挂。
// 证书走系统 crt bundle (163/QQ 均公共 CA; xiaozhi tenclass 需要钉根的特例
// 与邮箱无关, 见 patches/esp-ml307_esp_ssl.cc.patched)。
//
// 仅设备编译 (include 处守卫; web=DESKTOP+WEB 一并排除)。
#pragma once

#ifndef PLATFORM_BUILD_DESKTOP

#include <cstdint>
#include <string>
#include <esp_tls.h>

namespace mail_direct {

class MailTls {
public:
    ~MailTls();
    MailTls() = default;
    MailTls(const MailTls&) = delete;
    MailTls& operator=(const MailTls&) = delete;

    // 连接 + TLS 握手 (timeout_ms 同时是 connect 与后续 IO 的兜底超时基准)
    bool connect(const char* host, uint16_t port, int timeout_ms);
    // 底层 socket 兜底超时 (秒) — SO_RCVTIMEO/SO_SNDTIMEO
    void set_sock_timeout(int sec);
    bool write_all(const char* data, size_t len);
    bool write_str(const std::string& s) { return write_all(s.data(), s.size()); }
    // 读一行 (到 '\n', 去 \r\n)。deadline_ms = 相对当前时刻。
    bool read_line(std::string* out, int deadline_ms, size_t max_len = 64 * 1024);
    // 精确读 n 字节 (IMAP 字面量)。deadline_ms 同上。
    bool read_bytes(uint8_t* buf, size_t n, int deadline_ms);
    void close();
    bool alive() const { return _tls != nullptr; }

private:
    // 内部缓冲补给至少 1 字节 (受 deadline 约束); 返回可读字节数, 0=失败/超时
    size_t fill(int deadline_ms);
    esp_tls_t* _tls = nullptr;
    uint8_t _buf[2048];
    size_t  _pos = 0, _len = 0;
};

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
