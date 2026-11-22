// mail_store — 邮件账号 NVS 存储 (oneapp mail.json 结构, 固件侧落 devcfg NVS)。
//
// 键 "mail_accts" = {"accounts":[{"email","auth"}],"last":"..."} (≤15 字符键,
// 值为 JSON 串)。首启 NVS 为空时从 mail_seed.h 种子导入 (gitignored, 与
// Pi accounts.json 同源; 见 mail_seed.h.example)。auth 明文落 NVS —
// hal_wifi setConfig 日志掩码名单已含 mail_accts。
//
// 仅设备编译 (include 处守卫)。
#pragma once

#ifndef PLATFORM_BUILD_DESKTOP

#include <string>
#include <vector>

namespace mail_direct {

struct StoreAcct {
    std::string email;
    std::string auth;
};

// 账号表 (load 时按需 seed); last 为当前账号 (空=无)
std::vector<StoreAcct> load_accounts(std::string* last);
bool save_accounts(const std::vector<StoreAcct>& accts, const std::string& last);

// upsert (授权码变更同入口, 大小写不敏感去重; oneapp upsert_account 同款)
bool upsert_account(const std::string& email, const std::string& auth);
bool remove_account(const std::string& email);

// 按邮箱查账号 (大小写不敏感; 无则 false)
bool find_account(const std::string& email, StoreAcct* out);

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
