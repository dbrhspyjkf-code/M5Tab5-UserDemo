// mail_smtp 实现 — 见 mail_smtp.h。
#include "mail_smtp.h"

#ifndef PLATFORM_BUILD_DESKTOP

#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstring>

#include "mail_mime.h"

static const char* TAG = "mail-smtp";

namespace mail_direct {

// 读 SMTP 应答一行, 返回应答码 (≤0 = 失败/超时)
static int smtp_read_reply(MailTls& tls, std::string* line, int dl)
{
    if (!tls.read_line(line, dl)) return -1;
    if (line->size() < 3) return -1;
    int code = atoi(line->c_str());
    // 多行应答 "250-..." → 继续读至 "250 ..."
    while (line->size() > 3 && (*line)[3] == '-') {
        if (!tls.read_line(line, dl)) return -1;
        if (line->size() < 3) return -1;
        code = atoi(line->c_str());
    }
    return code;
}

// Subject → RFC2047-B (UTF-8, 45 字节/段, encoded-word ≤75 列)
static std::string rfc2047_b_words(const std::string& text)
{
    if (text.empty()) return "(无主题)";
    bool ascii = true;
    for (uint8_t c : text)
        if (c >= 0x80) { ascii = false; break; }
    if (ascii) return text;
    std::string out;
    const size_t CHUNK = 45;
    for (size_t i = 0; i < text.size(); i += CHUNK) {
        if (!out.empty()) out += "\r\n ";      // 头折叠
        out += "=?utf-8?B?";
        out += b64_encode((const uint8_t*)text.data() + i,
                          (text.size() - i > CHUNK) ? CHUNK : text.size() - i);
        out += "?=";
    }
    return out;
}

bool smtp_send(const std::string& email, const std::string& auth,
               const std::string& to, const std::string& subject,
               const std::string& body, const std::string& in_reply_to,
               const std::string& references, std::string* err,
               std::string* raw_msg, std::string* msg_id)
{
    if (email.empty() || auth.empty()) {
        *err = "账号未配置";
        return false;
    }
    if (to.find('@') == std::string::npos) {
        *err = "收件人格式不对";
        return false;
    }
    std::string host = Imap::provider_smtp(email);
    int port = Imap::provider_smtp_port(email);
    if (host.empty()) {
        *err = "无法识别邮箱域名";
        return false;
    }
    // ── 组消息 (EmailMessage 头顺序同款) ──
    static uint32_t s_mid_seq = 0;
    std::string dom = email.substr(email.rfind('@') + 1);
    std::string mid = "<" + std::to_string((long long)esp_timer_get_time()) +
                      "." + std::to_string(++s_mid_seq) + "@" + dom + ">";
    std::string body_txt = body.empty() ? " " : body;
    // 去尾部 CRLF 后统一 CRLF (SMTP DATA 终界)
    while (!body_txt.empty() && (body_txt.back() == '\n' || body_txt.back() == '\r'))
        body_txt.pop_back();
    std::string b64body = b64_encode((const uint8_t*)body_txt.data(),
                                     body_txt.size(), 76);
    std::string msg;
    msg.reserve(body_txt.size() + b64body.size() + 256);
    msg += "From: " + email + "\r\n";
    msg += "To: " + to + "\r\n";
    msg += "Subject: " + rfc2047_b_words(subject) + "\r\n";
    msg += "Date: " + rfc5322_now() + "\r\n";
    msg += "Message-ID: " + mid + "\r\n";
    if (!in_reply_to.empty()) {
        std::string refs = (references + " " + in_reply_to);
        // 折叠空白 + 截 900 (mail.py 同口径: 头过长部分服务器会拒)
        std::string refs2;
        for (char c : refs)
            if (c == '\r' || c == '\n') refs2.push_back(' ');
            else refs2.push_back(c);
        if (refs2.size() > 900) refs2 = refs2.substr(0, 900);
        msg += "In-Reply-To: " + in_reply_to + "\r\n";
        msg += "References: " + refs2 + "\r\n";
    }
    msg += "MIME-Version: 1.0\r\n";
    msg += "Content-Type: text/plain; charset=utf-8\r\n";
    msg += "Content-Transfer-Encoding: base64\r\n";
    msg += "\r\n";
    msg += b64body;
    msg += "\r\n";
    if (raw_msg) *raw_msg = msg;
    if (msg_id) *msg_id = mid;

    // ── SMTP_SSL 会话 ──
    MailTls tls;
    if (!tls.connect(host.c_str(), (uint16_t)port, 15 * 1000)) {
        *err = "连接超时";
        return false;
    }
    auto fail = [&](const std::string& e) {
        *err = e;
        tls.close();
        return false;
    };
    int dl = 15 * 1000;
    std::string line;
    int code = smtp_read_reply(tls, &line, dl);
    if (code != 220) return fail(friendly_err(line.empty() ? "连接超时" : line));
    if (!tls.write_str("EHLO tab5\r\n")) return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 250) return fail(friendly_err(line.empty() ? "连接超时" : line));
    // AUTH LOGIN
    if (!tls.write_str("AUTH LOGIN\r\n")) return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 334) return fail("登录被拒 (授权码错或未开 IMAP/SMTP)");
    if (!tls.write_str(b64_encode((const uint8_t*)email.data(), email.size()) +
                       "\r\n"))
        return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 334) return fail("登录被拒 (授权码错或未开 IMAP/SMTP)");
    if (!tls.write_str(b64_encode((const uint8_t*)auth.data(), auth.size()) +
                       "\r\n"))
        return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 235) return fail("登录被拒 (授权码错或未开 IMAP/SMTP)");
    // 信封
    if (!tls.write_str("MAIL FROM:<" + email + ">\r\n")) return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 250) return fail(friendly_err(line.empty() ? "发送失败" : line));
    if (!tls.write_str("RCPT TO:<" + to + ">\r\n")) return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 250 && code != 251)
        return fail(friendly_err(line.empty() ? "发送失败" : line));
    if (!tls.write_str("DATA\r\n")) return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    if (code != 354) return fail(friendly_err(line.empty() ? "发送失败" : line));
    // 正文点填充 (行首 '.' 翻倍) + 终界 "."
    {
        size_t i = 0;
        while (i < msg.size()) {
            size_t e = msg.find('\n', i);
            std::string one = msg.substr(
                i, e == std::string::npos ? std::string::npos : e - i + 1);
            if (!one.empty() && one[0] == '.') one = "." + one;
            if (!tls.write_str(one)) return fail("连接超时");
            if (e == std::string::npos) break;
            i = e + 1;
        }
        if (!msg.empty() && msg.back() != '\n') {
            if (!tls.write_str("\r\n")) return fail("连接超时");
        }
    }
    if (!tls.write_str(".\r\n")) return fail("连接超时");
    code = smtp_read_reply(tls, &line, dl);
    tls.write_str("QUIT\r\n");
    tls.close();
    if (code != 250) return fail(friendly_err(line.empty() ? "发送失败" : line));
    return true;
}

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
