// mail_direct 实现 — 见 mail_direct.h。编排逐条对齐 oneapp mail.py 各函数。
#include "mail_direct.h"

#ifndef PLATFORM_BUILD_DESKTOP

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>
#include <mutex>

#include "mail_imap.h"
#include "mail_mime.h"
#include "mail_smtp.h"
#include "mail_store.h"

static const char* TAG = "mail-direct";

namespace mail_direct {

// ── 全局单会话 (mail.py _SESS 等价; mutex 对应 Pi 移植版 RLock) ─────────────
static std::mutex g_mail_mu;             // 所有公共操作整段串行
static Imap* g_sess = nullptr;           // 缓存中的会话 (nullptr=无)
static std::string g_sess_email;
static int64_t g_sess_at = 0;            // esp_timer ms
static const int64_t SESS_TTL_MS = 240 * 1000;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

// 死会话丢弃 (短超时 LOGOUT, mail.py _drop_sess 同款)
static void drop_sess()
{
    if (g_sess) {
        g_sess->logout_quiet(3);
        delete g_sess;
        g_sess = nullptr;
    }
    g_sess_email.clear();
    g_sess_at = 0;
}

// 操作收尾: 命中缓存会话不登出留给复用; 一次性会话登出 (mail.py _release)
static void release_sess(Imap* used)
{
    if (g_sess != used) {
        if (used) {
            used->logout_quiet(3);
            delete used;
        }
    }
}

// connect (mail.py _connect 同款): 会话复用 → NOOP 探活 5s; 否则全新连接。
// 失败/换账号自动 drop。返回 Imap* (调用方 finally 调 release_sess)。
static Imap* connect_sess(const std::string& email, const std::string& auth,
                          std::string* err)
{
    if (g_sess && g_sess_email == email && now_ms() - g_sess_at < SESS_TTL_MS) {
        if (g_sess->noop(5)) {           // 探活短超时 (死会话别顶满 12s)
            g_sess_at = now_ms();
            return g_sess;
        }
        drop_sess();                     // 会话已被服务端掐 → 重连
    }
    Imap* m = new Imap();
    if (!m->connect_login(email, auth, 12, err)) {
        delete m;
        return nullptr;
    }
    drop_sess();                         // 换账号/旧会话让位
    g_sess = m;
    g_sess_email = email;
    g_sess_at = now_ms();
    return m;
}

// ── 徽标 sweep (total_unread 口径: LIST + 逐夹 STATUS UNSEEN 求和) ──────────
struct BadgeCache {
    int64_t at = 0;
    std::vector<Acct> accts;
};
static BadgeCache g_badge;

static int sweep_unread(Imap* m)
{
    std::string err;
    std::vector<std::pair<std::string, std::string>> folders;
    if (!m->list_folders(&folders, &err)) return -1;
    int sum = 0;
    for (const auto& f : folders) {
        int t = -1, u = -1;
        if (m->status_counts(f.first, &t, &u, &err) && u >= 0) sum += u;
        // STATUS 失败按 -1 兜底跳过 (mail.py 同口径)
    }
    return sum;
}

// ── 账号层 ──────────────────────────────────────────────────────────────────
Ret accounts(std::vector<Acct>* out)
{
    Ret r;
    std::lock_guard<std::mutex> lk(g_mail_mu);
    if (g_badge.at && now_ms() - g_badge.at < 60 * 1000) {
        *out = g_badge.accts;
        r.ok = true;
        return r;
    }
    std::string last;
    auto store = load_accounts(&last);
    std::vector<Acct> accs;
    for (const auto& a : store) accs.push_back({a.email, -1});
    for (auto& a : accs) {
        StoreAcct sa;
        if (!find_account(a.email, &sa)) continue;
        std::string err;
        Imap* m = connect_sess(sa.email, sa.auth, &err);
        if (!m) {
            a.unread = -1;
            r.err = err;
            continue;
        }
        a.unread = sweep_unread(m);
        release_sess(m);
    }
    if (!accs.empty()) {
        g_badge.accts = accs;
        g_badge.at = now_ms();
    }
    *out = accs;
    r.ok = true;
    return r;
}

Ret login(const std::string& email, const std::string& auth, int* unread)
{
    Ret r;
    *unread = 0;
    if (email.empty() || auth.empty()) {
        r.err = "邮箱/授权码格式不对";
        return r;
    }
    std::lock_guard<std::mutex> lk(g_mail_mu);
    std::string err;
    Imap* m = connect_sess(email, auth, &err);
    if (!m) {
        r.err = err;
        return r;
    }
    // test_login: EXAMINE INBOX + UID SEARCH UNSEEN 计数
    do {
        if (!m->examine("INBOX", &err)) {
            r.err = err.empty() ? "INBOX 打开失败" : err;
            break;
        }
        std::vector<uint32_t> uids;
        err.clear();
        if (!m->uid_search("UNSEEN", &uids, &err)) {
            r.err = err;
            break;
        }
        *unread = (int)uids.size();
        g_badge.at = 0;                  // 新账号角标即时失效
        r.ok = true;
    } while (false);
    release_sess(m);
    if (r.ok) upsert_account(email, auth);
    return r;
}

Ret del_acct(const std::string& email)
{
    Ret r;
    std::lock_guard<std::mutex> lk(g_mail_mu);
    remove_account(email);
    g_badge.at = 0;
    r.ok = true;
    return r;
}

// ── 文件夹层 ────────────────────────────────────────────────────────────────
Ret folders(const std::string& email, std::vector<Folder>* out)
{
    Ret r;
    StoreAcct sa;
    if (!find_account(email, &sa)) {
        r.err = "账号未配置";
        return r;
    }
    std::lock_guard<std::mutex> lk(g_mail_mu);
    std::string err;
    Imap* m = connect_sess(sa.email, sa.auth, &err);
    if (!m) {
        r.err = err;
        return r;
    }
    do {
        std::vector<std::pair<std::string, std::string>> dirs;
        if (!m->list_folders(&dirs, &err)) {
            r.err = err;
            break;
        }
        std::vector<Folder> list;
        for (const auto& d : dirs) {
            Folder f;
            f.raw = d.first;
            f.name = d.second;
            int t = -1, u = -1;
            std::string serr;
            if (m->status_counts(d.first, &t, &u, &serr)) {
                f.total = t;
                f.unread = u;
            }                      // 失败保持 -1 (mail.py 口径)
            list.push_back(std::move(f));
        }
        std::sort(list.begin(), list.end(),
                  [](const Folder& a, const Folder& b) {
                      bool ai = a.name == "INBOX" || a.raw == "\"INBOX\"";
                      bool bi = b.name == "INBOX" || b.raw == "\"INBOX\"";
                      if (ai != bi) return ai;
                      int ua = a.unread >= 0 ? a.unread : 0;
                      int ub = b.unread >= 0 ? b.unread : 0;
                      if (ua != ub) return ua > ub;
                      return a.name < b.name;
                  });
        // 虚拟夹置顶 (163 网页版「未读/红旗」智能视图 IMAP 不暴露 — 手动聚合)
        Folder un, fl;
        un.raw = "@unread";
        un.name = "全部未读";
        un.virt = true;
        fl.raw = "@flagged";
        fl.name = "全部红旗";
        fl.virt = true;
        out->push_back(un);
        out->push_back(fl);
        for (auto& f : list) out->push_back(std::move(f));
        r.ok = true;
    } while (false);
    release_sess(m);
    return r;
}

// ── 列表 (单夹 / 聚合) ──────────────────────────────────────────────────────
// FETCH 前导串提取 FLAGS (…)
static void flags_of(const std::string& head, bool* seen, bool* flagged)
{
    *seen = head.find("\\Seen") != std::string::npos;
    *flagged = head.find("\\Flagged") != std::string::npos;
}

// 16KB 原文 → MailMsg (mime 解码, 同 mail.py 列表行构建)
static Msg build_msg(const Imap::FetchItem& it, const std::string& fld_raw,
                     const std::string& fld_disp, bool inbox)
{
    Msg mm;
    mm.uid = std::to_string(it.uid);
    mm.fld = fld_raw;
    mm.fldn = fld_disp;
    MimeNode node;
    if (mime_parse(it.body.data(), it.body.size(), &node)) {
        mm.subject = mime_subject(node).substr(0, 80);
        std::string from_hdr;
        if (const std::string* f = node.hdr("from")) from_hdr = *f;
        mm.from = decode_from(from_hdr, inbox).substr(0, 70);
        std::string dh;
        if (const std::string* d = node.hdr("date")) dh = *d;
        mm.date = fmt_date(dh);
        mm.preview = extract_preview(node, 200);
    } else {
        mm.subject = "(正文解析失败)";
    }
    flags_of(it.flags_head, &mm.seen, &mm.flagged);
    return mm;
}

// 单夹列表 (mail.py fetch_msgs, scope=all, 最近 limit 封)
static Ret list_one(Imap* m, const std::string& email, const std::string& raw,
                    const std::string& disp, int limit, std::vector<Msg>* out,
                    int* total)
{
    Ret r;
    std::string err;
    if (!m->examine(raw, &err)) {
        r.err = err.empty() ? "文件夹打开失败" : err;
        return r;
    }
    std::vector<uint32_t> ids;
    err.clear();
    if (!m->uid_search("ALL", &ids, &err)) {
        r.err = err;
        return r;
    }
    *total = (int)ids.size();
    if (ids.empty()) {
        r.ok = true;
        return r;
    }
    // 最近 limit 封 (升序尾=最新)
    size_t start = ids.size() > (size_t)limit ? ids.size() - (size_t)limit : 0;
    std::string uidset;
    for (size_t i = start; i < ids.size(); i++) {
        if (!uidset.empty()) uidset += ",";
        uidset += std::to_string(ids[i]);
    }
    std::vector<Imap::FetchItem> items;
    err.clear();
    if (!m->uid_fetch(uidset, "(FLAGS BODY.PEEK[]<0.16384>)", &items, &err)) {
        r.err = err;
        return r;
    }
    bool inbox = raw == "\"INBOX\"" || raw == "INBOX";
    for (size_t i = ids.size(); i-- > start;) {          // 最新在前
        for (const auto& it : items) {
            if (it.uid == ids[i]) {
                out->push_back(build_msg(it, raw, disp, inbox));
                break;
            }
        }
    }
    r.ok = true;
    return r;
}

// 聚合视图 (mail.py fetch_all, 已修口径: 含 INBOX; 单键 UNSEEN/FLAGGED)
static Ret list_agg(Imap* m, const std::string& which, int limit,
                    std::vector<Msg>* out, int* total)
{
    Ret r;
    const char* key = (which == "flagged") ? "FLAGGED" : "UNSEEN";
    std::string err;
    std::vector<std::pair<std::string, std::string>> dirs;
    if (!m->list_folders(&dirs, &err)) {
        r.err = err.empty() ? "LIST 失败" : err;
        return r;
    }
    // 每夹 EXAMINE + UID SEARCH → uid 并集 (升序尾=最新)
    std::vector<std::pair<std::string, std::vector<uint32_t>>> hits;
    for (const auto& d : dirs) {
        if (!m->examine(d.first, &err)) continue;    // 单夹失败不拖垮聚合
        std::vector<uint32_t> uids;
        err.clear();
        if (!m->uid_search(key, &uids, &err)) continue;
        if (uids.empty()) continue;
        hits.emplace_back(d.first, std::move(uids));
    }
    int total_hits = 0;
    for (const auto& h : hits) total_hits += (int)h.second.size();
    *total = total_hits;
    // 阶段一: 每夹最新 40 封头 (FROM/SUBJECT/DATE) → Date 排序
    struct Cand { int64_t ep; std::string fld; std::string disp; uint32_t uid; };
    std::vector<Cand> cand;
    for (const auto& h : hits) {
        size_t n40 = h.second.size() > 40 ? 40 : h.second.size();
        std::string uidset;
        for (size_t i = h.second.size() - n40; i < h.second.size(); i++) {
            if (!uidset.empty()) uidset += ",";
            uidset += std::to_string(h.second[i]);
        }
        std::string disp = h.first;
        for (const auto& d : dirs)
            if (d.first == h.first) disp = d.second;
        if (!m->examine(h.first, &err)) continue;
        std::vector<Imap::FetchItem> heads;
        std::string e2;
        if (!m->uid_fetch(uidset,
                          "(FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])",
                          &heads, &e2))
            continue;
        for (const auto& it : heads) {
            MimeNode node;
            int64_t ep = 0;
            if (mime_parse(it.body.data(), it.body.size(), &node)) {
                std::string dh;
                if (const std::string* d = node.hdr("date")) dh = *d;
                ep = date_epoch(dh);
            }
            cand.push_back({ep, h.first, disp, it.uid});
        }
    }
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.ep > b.ep; });
    if (cand.size() > (size_t)limit) cand.resize(limit);
    if (cand.empty()) {
        r.ok = true;
        return r;
    }
    // 阶段二: 选中的逐夹批量取 16KB
    for (const auto& d : dirs) {
        std::string uidset;
        for (const auto& c : cand)
            if (c.fld == d.first) {
                if (!uidset.empty()) uidset += ",";
                uidset += std::to_string(c.uid);
            }
        if (uidset.empty()) continue;
        if (!m->examine(d.first, &err)) continue;
        std::vector<Imap::FetchItem> items;
        std::string e2;
        if (!m->uid_fetch(uidset, "(FLAGS BODY.PEEK[]<0.16384>)", &items, &e2))
            continue;
        // 按 cand 顺序 (最新在前) 输出
        for (const auto& c : cand) {
            if (c.fld != d.first) continue;
            for (const auto& it : items) {
                if (it.uid == c.uid) {
                    out->push_back(build_msg(it, d.first, d.second, false));
                    break;
                }
            }
        }
    }
    r.ok = true;
    return r;
}

Ret list(const std::string& email, const std::string& folder, int limit,
         std::vector<Msg>* out, int* total)
{
    Ret r;
    StoreAcct sa;
    if (!find_account(email, &sa)) {
        r.err = "账号未配置";
        return r;
    }
    if (folder.empty()) {
        r.err = "缺 folder";
        return r;
    }
    std::lock_guard<std::mutex> lk(g_mail_mu);
    std::string err;
    Imap* m = connect_sess(sa.email, sa.auth, &err);
    if (!m) {
        r.err = err;
        return r;
    }
    if (folder.rfind("@", 0) == 0)
        r = list_agg(m, folder.substr(1), limit, out, total);
    else {
        std::string disp = utf7_decode(
            folder.size() >= 2 && folder.front() == '"' && folder.back() == '"'
                ? folder.substr(1, folder.size() - 2)
                : folder);
        r = list_one(m, email, folder, disp, limit, out, total);
    }
    release_sess(m);
    return r;
}

// ── 详情 ────────────────────────────────────────────────────────────────────
Ret body(const std::string& email, const std::string& folder,
         const std::string& uid, std::string* text, Meta* meta)
{
    Ret r;
    StoreAcct sa;
    if (!find_account(email, &sa)) {
        r.err = "账号未配置";
        return r;
    }
    std::lock_guard<std::mutex> lk(g_mail_mu);
    std::string err;
    Imap* m = connect_sess(sa.email, sa.auth, &err);
    if (!m) {
        r.err = err;
        return r;
    }
    do {
        if (!m->examine(folder, &err)) {
            ESP_LOGW(TAG, "body: examine %s fail: %s", folder.c_str(), err.c_str());
            r.err = err.empty() ? "文件夹打开失败" : err;
            break;
        }
        uint8_t* raw = nullptr;
        size_t n = 0;
        err.clear();
        if (!m->fetch_raw(uid, "(BODY.PEEK[]<0.131072>)", &raw, &n, &err) ||
            !raw || n == 0) {
            ESP_LOGW(TAG, "body: fetch_raw uid=%s fail: %s", uid.c_str(), err.c_str());
            r.err = err.empty() ? "取正文失败" : err;
            if (raw) heap_caps_free(raw);
            break;
        }
        ESP_LOGI(TAG, "body: uid=%s raw=%uB head=%.60s", uid.c_str(),
                 (unsigned)n, (const char*)raw);
        MimeNode node;
        if (mime_parse((const char*)raw, n, &node)) {
            *text = extract_body(node, 6000);
            ESP_LOGI(TAG, "body: text_len=%u head=%.40s", (unsigned)text->size(),
                     text->c_str());
            if (const std::string* v = node.hdr("message-id"))
                meta->msg_id = *v;
            if (const std::string* v = node.hdr("references")) meta->refs = *v;
            std::string reply_raw;
            if (const std::string* v = node.hdr("reply-to"))
                reply_raw = decode_rfc2047(*v);
            if (reply_raw.empty()) {
                if (const std::string* v = node.hdr("from"))
                    reply_raw = decode_rfc2047(*v);
            }
            std::string disp, addr;
            parse_addr(reply_raw, &disp, &addr);
            meta->from_addr = addr;
            if (const std::string* v = node.hdr("date")) meta->date = fmt_date(*v);
            meta->subject = mime_subject(node);
        } else {
            *text = "(正文解析失败)";
        }
        heap_caps_free(raw);
        r.ok = true;
    } while (false);
    release_sess(m);
    return r;
}

// ── 动作 ────────────────────────────────────────────────────────────────────
Ret action(const std::string& email, const std::string& folder,
           const std::string& uid, const std::string& act)
{
    Ret r;
    const char* op = nullptr;
    const char* fl = nullptr;
    if (act == "mark_read") { op = "+FLAGS"; fl = "(\\Seen)"; }
    else if (act == "mark_unread") { op = "-FLAGS"; fl = "(\\Seen)"; }
    else if (act == "flag") { op = "+FLAGS"; fl = "(\\Flagged)"; }
    else if (act == "unflag") { op = "-FLAGS"; fl = "(\\Flagged)"; }
    else {
        r.err = "未知操作";
        return r;
    }
    StoreAcct sa;
    if (!find_account(email, &sa)) {
        r.err = "账号未配置";
        return r;
    }
    std::lock_guard<std::mutex> lk(g_mail_mu);
    std::string err;
    Imap* m = connect_sess(sa.email, sa.auth, &err);
    if (!m) {
        r.err = err;
        return r;
    }
    do {
        if (!m->select_rw(folder, &err)) {
            r.err = err.empty() ? "文件夹打开失败" : err;
            break;
        }
        uint32_t u = (uint32_t)strtoul(uid.c_str(), nullptr, 10);
        err.clear();
        if (!m->uid_store(u, op, fl, &err)) {
            r.err = err.empty() ? "操作失败" : err;
            break;
        }
        r.ok = true;
        g_badge.at = 0;                  // 角标可能变化
    } while (false);
    release_sess(m);
    return r;
}

// ── 发送 ────────────────────────────────────────────────────────────────────
Ret send(const std::string& email, const std::string& to,
         const std::string& subject, const std::string& body,
         const std::string& in_reply_to, const std::string& references)
{
    Ret r;
    StoreAcct sa;
    if (!find_account(email, &sa)) {
        r.err = "账号未配置";
        return r;
    }
    std::string raw_msg, msg_id;
    std::lock_guard<std::mutex> lk(g_mail_mu);
    std::string err;
    if (!smtp_send(sa.email, sa.auth, to, subject, body, in_reply_to,
                   references, &err, &raw_msg, &msg_id)) {
        r.err = err;
        return r;
    }
    r.ok = true;
    g_badge.at = 0;
    // ── Sent APPEND (mail.py _append_sent: 163 自动存 Sent 查重, 全失败静默) ──
    do {
        Imap* m = connect_sess(sa.email, sa.auth, &err);
        if (!m) break;
        std::vector<std::pair<std::string, std::string>> dirs;
        std::string e2;
        if (m->list_folders(&dirs, &e2)) {
            std::string box;
            for (const auto& d : dirs) {
                std::string name = d.second;
                for (auto& c : name) c = (char)tolower((unsigned char)c);
                if (name.find("已发送") != std::string::npos ||
                    name == "sent messages" || name == "sent items" ||
                    name == "sent mail" || name == "sent") {
                    box = d.first;
                    break;
                }
            }
            if (!box.empty() && !msg_id.empty()) {
                // 查重: SMTP 自动存档的服务商已有原件
                if (!m->examine(box, &e2)) break;
                std::vector<uint32_t> uids;
                e2.clear();
                if (m->uid_search(("HEADER Message-ID \"" + msg_id + "\"")
                                      .c_str(),
                                  &uids, &e2) &&
                    uids.empty())
                    m->append(box, raw_msg.data(), raw_msg.size(), &e2);
            }
        }
        release_sess(m);
    } while (false);
    return r;
}

// ── 主页角标 ────────────────────────────────────────────────────────────────
Ret home_badge(int* unread_total)
{
    Ret r;
    std::lock_guard<std::mutex> lk(g_mail_mu);
    if (g_badge.at && now_ms() - g_badge.at < 60 * 1000) {
        int total = 0;
        for (const auto& a : g_badge.accts)
            if (a.unread > 0) total += a.unread;
        *unread_total = total;
        r.ok = true;
        return r;
    }
    std::string last;
    auto store = load_accounts(&last);
    g_badge.accts.clear();
    int total = 0;
    for (const auto& a : store) {
        std::string err;
        Imap* m = connect_sess(a.email, a.auth, &err);
        if (!m) {
            g_badge.accts.push_back({a.email, -1});
            continue;
        }
        int u = sweep_unread(m);
        release_sess(m);
        g_badge.accts.push_back({a.email, u});
        if (u > 0) total += u;
    }
    g_badge.at = now_ms();
    *unread_total = total;
    r.ok = true;
    return r;
}

}  // namespace mail_direct

#endif  // !PLATFORM_BUILD_DESKTOP
