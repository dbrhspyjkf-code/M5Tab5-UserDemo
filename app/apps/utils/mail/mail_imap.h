// mail_imap — IMAP 客户端 (oneapp mail.py 的 C++ 等价, esp-tls 直连 993)。
//
// 会话模型 (对齐 mail.py _SESS): 外层(mail_direct)持全局单会话 + mutex,
// 同账号 <240s 内 NOOP 探活通过即复用 (防 163 短连接风暴限流); 探活 5s
// 短超时 (死会话 NOOP 163 实测会挂满), 死会话 LOGOUT 3s。
// 163 必发 ID 命令 (Unsafe Login 防御); 收件浏览全程 EXAMINE+PEEK 不置
// 已读; QQ 上禁用 OR 复合搜索 (实测返回空) — 由调用方两查一并。
//
// 仅设备编译 (include 处守卫)。
#pragma once

#ifndef PLATFORM_BUILD_DESKTOP

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include "mail_tls.h"

namespace mail_direct {

struct ImapLine {
    std::string head;      // 字面量前的行体 (多段拼接, 不含 {n} 标记)
    std::string lit;       // 字面量字节 (≤16KB 用; 大字面量走 fetch_raw)
    bool has_lit = false;
};

struct ImapRet {
    bool ok = false;
    std::string err;
    std::vector<ImapLine> lines;   // untagged 记录 (tagged 行已消费判 ok)
};

class Imap {
public:
    ~Imap() { disconnect(); }

    // TLS 连接 + 问候 + LOGIN + ID。err 返回 _friendly_err 同款中文文案。
    bool connect_login(const std::string& email, const std::string& auth,
                       int timeout_s, std::string* err);
    // NOOP 探活 (probe_sec 短超时)
    bool noop(int probe_sec);
    void logout_quiet(int timeout_s);   // 尽力 LOGOUT + 关连接 (死会话短超时)
    void disconnect() { _tls.close(); }
    bool alive() const { return _tls.alive(); }
    const std::string& email() const { return _email; }

    // 通用命令: 自动加 tag 前缀, 收齐至 tag 行。ok = tagged OK。
    ImapRet cmd(const std::string& command, int timeout_s);

    // ── 高层操作 (对应 mail.py 各函数内部步骤) ────────────────────────────
    // LIST "" "*" → (raw 含引号, utf7 解码显示名) 仅未跳过夹
    bool list_folders(std::vector<std::pair<std::string, std::string>>* out,
                      std::string* err);
    // STATUS <raw> (MESSAGES UNSEEN)
    bool status_counts(const std::string& raw, int* total, int* unread,
                       std::string* err);
    bool examine(const std::string& raw, std::string* err);   // readonly
    bool select_rw(const std::string& raw, std::string* err); // STORE 需要
    bool uid_search(const char* key, std::vector<uint32_t>* uids,
                    std::string* err);
    // 批量 UID FETCH (≤16KB 字面量, 阶段一 headers / 阶段二 列表)
    struct FetchItem {
        uint32_t uid = 0;
        std::string flags_head;   // 前导串 (含 FLAGS (...) 与 UID)
        std::string body;         // 字面量字节
    };
    bool uid_fetch(const std::string& uidset, const char* items,
                   std::vector<FetchItem>* out, std::string* err);
    // 正文大字面量 (128KB): PSRAM 缓冲, 调用方 free。OK=false 时 *data 为空。
    bool fetch_raw(const std::string& uidset, const char* items,
                   uint8_t** data, size_t* len, std::string* err);
    bool uid_store(uint32_t uid, const char* op, const char* flags,
                   std::string* err);
    // APPEND <box> (\Seen) {n} (QQ 落 Sent 用)
    bool append(const std::string& raw, const char* data, size_t n,
                std::string* err);

    // ── 静态助手 ──────────────────────────────────────────────────────────
    static std::string provider_imap(const std::string& email);
    static int provider_smtp_port(const std::string& email);   // 465 (outlook 587)
    static std::string provider_smtp(const std::string& email);
    // raw (可含引号) 是否跳过: oneapp should_skip (utf7 解码后双判)
    static bool should_skip(const std::string& raw);
    // LOGIN 参数引号转义
    static std::string imap_quote(const std::string& s);

private:
    std::string next_tag();
    MailTls _tls;
    std::string _email;
    int _tag_n = 0;
};

// _friendly_err 等价: 异常串 → oneapp 用户可读中文文案
std::string friendly_err(const std::string& s);

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
