// mail_store 实现 — 见 mail_store.h。JSON 串手工拼/解 (字段值需转义的只有
// email/auth — 均为 \ 与 " 两字符, 沿用 imap_quote 同款转义)。
#include "mail_store.h"

#ifndef PLATFORM_BUILD_DESKTOP

#include <hal/hal.h>
#include <mooncake_log.h>
#include <cstring>

#if __has_include("mail_seed.h")
#include "mail_seed.h"          // gitignored: MAIL_SEED_ACCOUNTS_JSON
#endif

static const char* TAG = "mail-store";
static const char* NVS_KEY = "mail_accts";      // ≤15 字符

namespace mail_direct {

// ── JSON 串手工拼解 (仅 email/auth 两字段, 转义 \ 与 ") ──────────────────────
static std::string jesc(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// 从对象级 JSON 串解出一个字符串键的值 (value 须为字符串); 找不到返回空。
// 简易扫描: "key" 后跳空白与冒号, 取引号串 (处理 \" 转义)。
static std::string jget_str(const std::string& json, const std::string& key)
{
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    while (p != std::string::npos) {
        p += pat.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == ':')) p++;
        if (p < json.size() && json[p] == '"') {
            std::string out;
            p++;
            while (p < json.size() && json[p] != '"') {
                if (json[p] == '\\' && p + 1 < json.size()) {
                    out.push_back(json[p + 1]);
                    p += 2;
                } else {
                    out.push_back(json[p]);
                    p++;
                }
            }
            return out;
        }
        p = json.find(pat, p);
    }
    return "";
}

// accounts 数组元素逐个解出 (email/auth 对)
static std::vector<StoreAcct> jget_accts(const std::string& json)
{
    std::vector<StoreAcct> out;
    size_t arr = json.find("\"accounts\"");
    if (arr == std::string::npos) return out;
    size_t lb = json.find('[', arr);
    if (lb == std::string::npos) return out;
    int depth = 0;
    size_t obj_start = 0;
    for (size_t i = lb; i < json.size(); i++) {
        char c = json[i];
        if (c == '{') {
            if (depth == 0) obj_start = i;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && obj_start) {
                StoreAcct a;
                a.email = jget_str(json.substr(obj_start, i - obj_start + 1),
                                   "email");
                a.auth = jget_str(json.substr(obj_start, i - obj_start + 1),
                                  "auth");
                if (!a.email.empty() && !a.auth.empty())
                    out.push_back(std::move(a));
                obj_start = 0;
            }
        }
    }
    return out;
}

std::vector<StoreAcct> load_accounts(std::string* last)
{
    last->clear();
    std::string json = GetHAL()->getConfig(NVS_KEY, "");
    if (json.empty()) {
#if defined(MAIL_SEED_ACCOUNTS_JSON)
        // 首启种子 (Pi accounts.json 同源, 用户零配置即用)
        GetHAL()->setConfig(NVS_KEY, MAIL_SEED_ACCOUNTS_JSON);
        mclog::tagInfo(TAG, "mail accounts seeded from build-time config");
        json = GetHAL()->getConfig(NVS_KEY, "");
#endif
    }
    if (json.empty()) return {};
    *last = jget_str(json, "last");
    return jget_accts(json);
}

bool save_accounts(const std::vector<StoreAcct>& accts, const std::string& last)
{
    std::string json = "{\"accounts\":[";
    for (size_t i = 0; i < accts.size(); i++) {
        if (i) json += ",";
        json += "{\"email\":\"" + jesc(accts[i].email) + "\",\"auth\":\"" +
                jesc(accts[i].auth) + "\"}";
    }
    json += "],\"last\":\"" + jesc(last) + "\"}";
    GetHAL()->setConfig(NVS_KEY, json);
    return true;
}

bool upsert_account(const std::string& email, const std::string& auth)
{
    if (email.find('@') == std::string::npos || auth.empty()) return false;
    std::string last;
    auto accts = load_accounts(&last);
    bool found = false;
    for (auto& a : accts) {
        std::string low = a.email, want = email;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        for (auto& c : want) c = (char)tolower((unsigned char)c);
        if (low == want) {
            a.auth = auth;
            last = a.email;
            found = true;
            break;
        }
    }
    if (!found) {
        accts.push_back({email, auth});
        last = email;
    }
    save_accounts(accts, last);
    return true;
}

bool remove_account(const std::string& email)
{
    std::string last;
    auto accts = load_accounts(&last);
    std::vector<StoreAcct> keep;
    std::string low_want = email;
    for (auto& c : low_want) c = (char)tolower((unsigned char)c);
    for (auto& a : accts) {
        std::string low = a.email;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low != low_want) keep.push_back(std::move(a));
    }
    std::string new_last = last;
    std::string low_last = last;
    for (auto& c : low_last) c = (char)tolower((unsigned char)c);
    if (low_last == low_want)
        new_last = keep.empty() ? "" : keep.front().email;
    save_accounts(keep, new_last);
    return true;
}

bool find_account(const std::string& email, StoreAcct* out)
{
    std::string last;
    auto accts = load_accounts(&last);
    std::string want = email;
    for (auto& c : want) c = (char)tolower((unsigned char)c);
    for (auto& a : accts) {
        std::string low = a.email;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        if (low == want) {
            *out = a;
            return true;
        }
    }
    return false;
}

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
