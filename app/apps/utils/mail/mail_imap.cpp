// mail_imap 实现 — 见 mail_imap.h。协议序列逐条对齐 oneapp mail.py。
#include "mail_imap.h"

#ifndef PLATFORM_BUILD_DESKTOP

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdlib>
#include <regex>

#include "mail_mime.h"

static const char* TAG = "mail-imap";

namespace mail_direct {

// ── provider 注册表 (mail.py PROVIDERS) ─────────────────────────────────────
static const struct {
    const char* dom;
    const char* imap;
    const char* smtp;
    int smtp_port;
} PROVIDERS[] = {
    {"163.com", "imap.163.com", "smtp.163.com", 465},
    {"126.com", "imap.126.com", "smtp.126.com", 465},
    {"yeah.net", "imap.yeah.net", "smtp.yeah.net", 465},
    {"qq.com", "imap.qq.com", "smtp.qq.com", 465},
    {"foxmail.com", "imap.qq.com", "smtp.qq.com", 465},
    {"outlook.com", "outlook.office365.com", "smtp.office365.com", 587},
    {"hotmail.com", "outlook.office365.com", "smtp.office365.com", 587},
    {"gmail.com", "imap.gmail.com", "smtp.gmail.com", 465},
};

static std::string domain_of(const std::string& email)
{
    size_t at = email.rfind('@');
    if (at == std::string::npos || at + 1 >= email.size()) return "";
    std::string dom = email.substr(at + 1);
    for (auto& c : dom) c = (char)tolower((unsigned char)c);
    return dom;
}

std::string Imap::provider_imap(const std::string& email)
{
    std::string dom = domain_of(email);
    if (dom.empty()) return "";
    for (const auto& p : PROVIDERS)
        if (dom == p.dom) return p.imap;
    return "imap." + dom;                    // 未知域回落 (mail.py 同款)
}

std::string Imap::provider_smtp(const std::string& email)
{
    std::string dom = domain_of(email);
    if (dom.empty()) return "";
    for (const auto& p : PROVIDERS)
        if (dom == p.dom) return p.smtp;
    return "smtp." + dom;
}

int Imap::provider_smtp_port(const std::string& email)
{
    std::string dom = domain_of(email);
    for (const auto& p : PROVIDERS)
        if (dom == p.dom) return p.smtp_port;
    return 465;
}

// ── 错误文案 (mail.py _friendly_err) ────────────────────────────────────────
std::string friendly_err(const std::string& s)
{
    if (s.find("AUTHENTICATIONFAILED") != std::string::npos ||
        s.find("LOGIN failed") != std::string::npos ||
        s.find("5.7.8") != std::string::npos /* SMTP auth */)
        return "登录被拒 (授权码错或未开 IMAP/SMTP)";
    if (s.find("DT:SPM") != std::string::npos)
        return "163 拒发 (疑垃圾/频率限制, 稍后再试)";
    if (s.find("Unsafe Login") != std::string::npos)
        return "服务端拒连 (Unsafe Login)";
    std::string low;
    low.reserve(s.size());
    for (char c : s) low.push_back((char)tolower((unsigned char)c));
    if (low.find("timeout") != std::string::npos ||
        low.find("timed out") != std::string::npos)
        return "连接超时";
    std::smatch m;
    if (std::regex_search(s, m, std::regex("\\[([A-Z-]+)[^\\]]*\\]")))
        return "登录失败 " + m.str(1);
    return s.substr(0, 60);
}

// ── 跳过夹 (mail.py should_skip/_SKIP_*) ────────────────────────────────────
static const char* const SKIP_FOLDERS[] = {
    "&V4NXPpCuTvY-",   // 垃圾邮件
    "&Xn9USpCuTvY-",   // 广告邮件
    "&i6KWBZCuTvY-",   // 订阅邮件
    "&g0l6P3ux-",      // 草稿箱
};
static const char* const SKIP_NAMES[] = {
    "垃圾邮件", "广告邮件", "订阅邮件", "草稿箱", "Junk", "Drafts",
};

bool Imap::should_skip(const std::string& raw)
{
    if (raw.empty() || raw[0] == '[') return true;
    for (const char* k : SKIP_FOLDERS)
        if (raw == k) return true;
    std::string disp = utf7_decode(
        raw.size() >= 2 && raw.front() == '"' && raw.back() == '"'
            ? raw.substr(1, raw.size() - 2)
            : raw);
    for (const char* k : SKIP_NAMES)
        if (disp == k) return true;
    return false;
}

std::string Imap::imap_quote(const std::string& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// ── 连接/登录 ───────────────────────────────────────────────────────────────
bool Imap::connect_login(const std::string& email, const std::string& auth,
                         int timeout_s, std::string* err)
{
    std::string host = provider_imap(email);
    if (email.empty() || auth.empty()) {
        *err = "账号未配置";
        return false;
    }
    if (host.empty()) {
        *err = "无法识别邮箱域名";
        return false;
    }
    if (!_tls.connect(host.c_str(), 993, timeout_s * 1000)) {
        *err = "连接超时";
        return false;
    }
    _email = email;
    // 服务器问候 * OK ...
    {
        std::string greet;
        if (!_tls.read_line(&greet, timeout_s * 1000)) {
            *err = "连接超时";
            _tls.close();
            return false;
        }
        if (greet.rfind("* OK", 0) != 0 && greet.rfind("* PREAUTH", 0) != 0) {
            ESP_LOGW(TAG, "bad greeting: %.60s", greet.c_str());
            *err = friendly_err("Unsafe Login");
            _tls.close();
            return false;
        }
    }
    ImapRet r = cmd("LOGIN " + imap_quote(email) + " " + imap_quote(auth),
                    timeout_s);
    if (!r.ok) {
        *err = r.err.empty() ? "登录被拒 (授权码错或未开 IMAP/SMTP)" : r.err;
        _tls.close();
        return false;
    }
    // 163 对无 ID 客户端偶发 Unsafe Login 拒连 — 防御性补发, 失败无害
    cmd("ID (\"name\" \"TAB5MAIL\" \"version\" \"1.0.0\")", timeout_s);
    return true;
}

bool Imap::noop(int probe_sec)
{
    ImapRet r = cmd("NOOP", probe_sec);
    return r.ok;
}

void Imap::logout_quiet(int timeout_s)
{
    if (!_tls.alive()) return;
    _tls.set_sock_timeout(timeout_s > 3 ? 3 : timeout_s);
    try {
        cmd("LOGOUT", timeout_s);
    } catch (...) {
    }
    _tls.close();
}

// ── 命令执行 (字面量协议核心) ────────────────────────────────────────────────
std::string Imap::next_tag()
{
    return "T" + std::to_string(++_tag_n);
}

ImapRet Imap::cmd(const std::string& command, int timeout_s)
{
    ImapRet ret;
    if (!_tls.alive()) {
        ret.err = "连接超时";
        return ret;
    }
    std::string tag = next_tag();
    if (!_tls.write_str(tag + " " + command + "\r\n")) {
        ret.err = "连接超时";
        return ret;
    }
    int dl = timeout_s * 1000;
    std::string line;
    while (true) {
        line.clear();
        if (!_tls.read_line(&line, dl)) {
            ret.err = "连接超时";
            return ret;
        }
        // tagged 行: "<tag> OK/NO/BAD ..."
        if (line.rfind(tag + " ", 0) == 0) {
            ret.ok = line.find(" OK", tag.size()) == tag.size();
            if (!ret.ok) ret.err = friendly_err(line);
            return ret;
        }
        // 记录: 可能以 {n} 字面量结尾
        ImapLine rec;
        size_t brace = line.rfind('{');
        bool has_lit = false;
        long lit_n = 0;
        if (brace != std::string::npos && !line.empty() &&
            line.back() == '}') {
            lit_n = strtol(line.c_str() + brace + 1, nullptr, 10);
            if (lit_n >= 0 && lit_n <= 16 * 1024 * 1024) has_lit = true;
        }
        if (!has_lit) {
            rec.head = line;
            ret.lines.push_back(std::move(rec));
            continue;
        }
        rec.head = line.substr(0, brace);
        rec.lit.resize(lit_n);
        if (lit_n > 0 &&
            !_tls.read_bytes((uint8_t*)rec.lit.data(), (size_t)lit_n, dl)) {
            ret.err = "连接超时";
            return ret;
        }
        rec.has_lit = true;
        // 字面量后紧跟的余行 (如 ")") 并入 head 尾
        std::string tail;
        if (_tls.read_line(&tail, dl)) {
            if (tail.rfind(tag + " ", 0) == 0) {
                // 字面量 + ")" + tagged 同轮: tagged 行仍需判定
                ret.lines.push_back(std::move(rec));
                ret.ok = tail.find(" OK", tag.size()) == tag.size();
                if (!ret.ok) ret.err = friendly_err(tail);
                return ret;
            }
            rec.head += tail;
        }
        ret.lines.push_back(std::move(rec));
    }
}

// ── 高层操作 ────────────────────────────────────────────────────────────────
bool Imap::list_folders(std::vector<std::pair<std::string, std::string>>* out,
                        std::string* err)
{
    ImapRet r = cmd("LIST \"\" \"*\"", 12);
    if (!r.ok) {
        *err = r.err.empty() ? "LIST 失败" : r.err;
        return false;
    }
    // _LIST_RE 等价解析: (\flags) "delim"|NIL name
    for (const auto& ln : r.lines) {
        const std::string& line = ln.head;
        if (line.rfind("* LIST ", 0) != 0) continue;
        size_t p1 = line.find('(');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find(')', p1);
        if (p2 == std::string::npos) continue;
        size_t i = p2 + 1;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= line.size()) continue;
        std::string delim;
        if (line[i] == '"') {
            size_t q = line.find('"', i + 1);
            if (q == std::string::npos) continue;
            i = q + 1;
        } else if (line.compare(i, 3, "NIL") == 0) {
            i += 3;
        } else {
            continue;
        }
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        std::string raw = line.substr(i);
        // 去尾部空白
        while (!raw.empty() &&
               (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r'))
            raw.pop_back();
        if (raw.empty()) continue;
        std::string disp = utf7_decode(
            raw.size() >= 2 && raw.front() == '"' && raw.back() == '"'
                ? raw.substr(1, raw.size() - 2)
                : raw);
        if (should_skip(raw)) continue;
        out->emplace_back(raw, disp);
    }
    return true;
}

bool Imap::status_counts(const std::string& raw, int* total, int* unread,
                         std::string* err)
{
    *total = *unread = -1;
    ImapRet r = cmd("STATUS " + raw + " (MESSAGES UNSEEN)", 12);
    if (!r.ok) {
        *err = r.err;
        return false;                        // 调用方按 -1 兜底 (mail.py 口径)
    }
    for (const auto& ln : r.lines) {
        if (ln.head.rfind("* STATUS", 0) != 0) continue;
        auto grab = [&](const char* key) -> int {
            size_t p = ln.head.find(key);
            if (p == std::string::npos) return -1;
            return atoi(ln.head.c_str() + p + strlen(key));
        };
        int t = grab("MESSAGES ");
        int u = grab("UNSEEN ");
        *total = t >= 0 ? t : 0;
        *unread = u >= 0 ? u : 0;
        return true;
    }
    return true;
}

bool Imap::examine(const std::string& raw, std::string* err)
{
    ImapRet r = cmd("EXAMINE " + raw, 12);
    if (!r.ok) *err = r.err.empty() ? "文件夹打开失败" : r.err;
    return r.ok;
}

bool Imap::select_rw(const std::string& raw, std::string* err)
{
    ImapRet r = cmd("SELECT " + raw, 12);
    if (!r.ok) *err = r.err.empty() ? "文件夹打开失败" : r.err;
    return r.ok;
}

bool Imap::uid_search(const char* key, std::vector<uint32_t>* uids,
                      std::string* err)
{
    ImapRet r = cmd(std::string("UID SEARCH ") + key, 12);
    if (!r.ok) {
        *err = r.err;
        return false;
    }
    for (const auto& ln : r.lines) {
        if (ln.head.rfind("* SEARCH", 0) != 0) continue;
        size_t p = 8;
        while (p < ln.head.size()) {
            while (p < ln.head.size() && ln.head[p] == ' ') p++;
            if (p >= ln.head.size()) break;
            uint32_t u = (uint32_t)strtoul(ln.head.c_str() + p, nullptr, 10);
            if (u > 0) uids->push_back(u);
            while (p < ln.head.size() && ln.head[p] != ' ') p++;
        }
    }
    return true;
}

// 从 FETCH 前导串提取 UID (rb"UID (\d+)" 等价)
static uint32_t uid_from_head(const std::string& head)
{
    size_t p = head.find("UID ");
    if (p == std::string::npos) return 0;
    return (uint32_t)strtoul(head.c_str() + p + 4, nullptr, 10);
}

bool Imap::uid_fetch(const std::string& uidset, const char* items,
                     std::vector<FetchItem>* out, std::string* err)
{
    ImapRet r = cmd("UID FETCH " + uidset + " " + items, 20);
    if (!r.ok) {
        *err = r.err.empty() ? "取列表失败" : r.err;
        return false;
    }
    for (const auto& ln : r.lines) {
        if (ln.head.find("FETCH (") == std::string::npos || !ln.has_lit)
            continue;
        uint32_t uid = uid_from_head(ln.head);
        if (uid == 0 || ln.lit.empty()) continue;
        FetchItem it;
        it.uid = uid;
        it.flags_head = ln.head;
        it.body = std::move(ln.lit);
        out->push_back(std::move(it));
    }
    return true;
}

bool Imap::fetch_raw(const std::string& uidset, const char* items,
                     uint8_t** data, size_t* len, std::string* err)
{
    // 单 uid 专用 (正文取回): 响应恰一条 FETCH 记录 + tagged 行。
    // 字面量直接读进 PSRAM 缓冲 (128KB 级不占内部堆)。
    *data = nullptr;
    *len = 0;
    if (!_tls.alive()) {
        *err = "连接超时";
        return false;
    }
    std::string tag = next_tag();
    if (!_tls.write_str(tag + " UID FETCH " + uidset + " " + items + "\r\n")) {
        *err = "连接超时";
        return false;
    }
    int dl = 30 * 1000;
    std::string line;
    while (true) {
        line.clear();
        if (!_tls.read_line(&line, dl)) {
            *err = "连接超时";
            return false;
        }
        if (line.rfind(tag + " ", 0) == 0) {
            bool ok = line.find(" OK", tag.size()) == tag.size();
            if (!ok) *err = friendly_err(line);
            return ok;
        }
        size_t brace = line.rfind('{');
        if (brace == std::string::npos || line.empty() || line.back() != '}')
            continue;                        // 非字面量记录 (EXAMINE 回显等)
        long lit_n = strtol(line.c_str() + brace + 1, nullptr, 10);
        if (lit_n <= 0 || lit_n > 256 * 1024) continue;
        uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)lit_n,
                                                  MALLOC_CAP_SPIRAM |
                                                      MALLOC_CAP_8BIT);
        bool ok_read = true;
        if (!buf) {
            ESP_LOGW(TAG, "fetch_raw: alloc %ld failed", lit_n);
            std::string tmp(lit_n, '\0');
            ok_read = _tls.read_bytes((uint8_t*)tmp.data(), (size_t)lit_n, dl);
        } else if (!_tls.read_bytes(buf, (size_t)lit_n, dl)) {
            ok_read = false;
        }
        if (!ok_read) {
            if (buf) heap_caps_free(buf);
            *err = "连接超时";
            return false;
        }
        if (buf) {
            if (!*data) {
                *data = buf;
                *len = (size_t)lit_n;
            } else
                heap_caps_free(buf);         // 多记录时只留首条 (单 uid 不发生)
        }
        // 余下记录 (不应出现, 单 uid) 防御性消费到 tagged 行
    }
}

bool Imap::uid_store(uint32_t uid, const char* op, const char* flags,
                     std::string* err)
{
    char set[32];
    snprintf(set, sizeof(set), "%lu", (unsigned long)uid);
    ImapRet r = cmd(std::string("UID STORE ") + set + " " + op + " " + flags, 12);
    if (!r.ok) *err = r.err;
    return r.ok;
}

bool Imap::append(const std::string& raw, const char* data, size_t n,
                  std::string* err)
{
    if (!_tls.alive()) {
        *err = "连接超时";
        return false;
    }
    std::string tag = next_tag();
    char head[256];
    snprintf(head, sizeof(head), "%s APPEND %s (\\Seen) {%zu}", tag.c_str(),
             raw.c_str(), n);
    if (!_tls.write_str(std::string(head) + "\r\n")) {
        *err = "连接超时";
        return false;
    }
    std::string line;
    if (!_tls.read_line(&line, 15000)) {
        *err = "连接超时";
        return false;
    }
    if (line.empty() || line[0] != '+') {
        *err = friendly_err(line.empty() ? "连接超时" : line);
        return false;
    }
    if (!_tls.write_all(data, n) || !_tls.write_str("\r\n")) {
        *err = "连接超时";
        return false;
    }
    std::string l2;
    if (!_tls.read_line(&l2, 15000)) {
        *err = "连接超时";
        return false;
    }
    bool ok = l2.rfind(tag + " ", 0) == 0 &&
              l2.find(" OK", tag.size()) == tag.size();
    if (!ok) *err = friendly_err(l2.empty() ? "APPEND 失败" : l2);
    return ok;
}

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
