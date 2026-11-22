// mail_direct — 邮件直连总门面 (oneapp mail.py 语义, C++ 移植, 零 OrangePi 依赖)。
//
// 9 个操作直接产出 AppSettings 邮件 UI 的数据结构 (字段一一对应, worker 里
// 逐字段拷入)。全局单 IMAP 会话 + mutex 串行 (对应 Pi 移植版 RLock; 会话
// 复用 240s TTL + NOOP 探活 5s — 163 限流防御, 见 mail_imap.h)。
// 徽标 sweep 60s TTL 缓存 (accounts 与主页角标共用)。
//
// 仅设备编译 (include 处守卫; web/桌面保留 8768 HTTP 路径)。
#pragma once

#ifndef PLATFORM_BUILD_DESKTOP

#include <string>
#include <vector>
#include <cstdint>

namespace mail_direct {

struct Acct   { std::string email; int unread = 0; };
struct Folder { std::string raw, name; int unread = -1, total = -1;
                bool virt = false; };
struct Msg    { std::string uid, fld, fldn, from, subject, date, preview;
                bool seen = false, flagged = false; };
struct Meta   { std::string msg_id, refs, from_addr, date, subject; };

struct Ret {
    bool ok = false;
    std::string err;
};

// 账号列表 + 角标 (total_unread 口径: LIST+逐夹 STATUS(UNSEEN) 求和; 60s TTL)
Ret accounts(std::vector<Acct>* out);
// 登录验证 (connect+EXAMINE INBOX+SEARCH UNSEEN) + 落盘 upsert
Ret login(const std::string& email, const std::string& auth, int* unread);
Ret del_acct(const std::string& email);
// 文件夹列表 (虚拟夹 @unread/@flagged 置顶 + LIST/STATUS, INBOX 先, 未读多靠前)
Ret folders(const std::string& email, std::vector<Folder>* out);
// 邮件列表: folder "@unread"/"@flagged" = 跨夹聚合 (单键搜索, QQ 禁 OR),
// 其余 = 单夹最近 limit 封。total = 命中总数。
Ret list(const std::string& email, const std::string& folder, int limit,
         std::vector<Msg>* out, int* total);
// 详情正文 (≤6000) + 回信元数据
Ret body(const std::string& email, const std::string& folder,
         const std::string& uid, std::string* text, Meta* meta);
// 标已读/未读/旗标/取旗 (SELECT 读写 + UID STORE)
Ret action(const std::string& email, const std::string& folder,
           const std::string& uid, const std::string& act);
// SMTP 直发 + Sent APPEND (163 自动存 Sent 查重; 失败静默)
Ret send(const std::string& email, const std::string& to,
         const std::string& subject, const std::string& body,
         const std::string& in_reply_to, const std::string& references);
// 主页信封角标: 全账号未读总数 (60s TTL)
Ret home_badge(int* unread_total);

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
