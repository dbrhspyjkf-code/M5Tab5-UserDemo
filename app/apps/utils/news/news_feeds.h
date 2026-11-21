#pragma once
/*
 * news_feeds — 新闻源表共享助手 (2026-11-05 car-dash 同款多选; 2026-11-21
 * 对齐 oneapp 新闻源设置重写为完整源表模型)。
 *
 * NVS "news_feeds" = 源表 JSON 数组 [{id,name,url,on}] — 与 oneapp
 * news_feeds.json / noteserver kv news_feeds 完全同形状 (云同步零转换)。
 * 旧版存选中 id 数组 ["ai",...]: 首读自动迁移 (已勾选集 → on 态; 未动过
 * 的默认 ["ai"] / 空表 → 全内置 on, 不因迁移缩页签)。
 *
 * 单一可见性概念 (oneapp 对齐): on=true 的源 = 新闻 app 页签 + 主页轮播 +
 * 抓取层 (隐藏不抓, news_rss::setFeeds 动态生效表)。
 * - 内置源缺失自动补齐 (_mergeBuiltins, on=true): 新增内置源老 NVS/云端
 *   旧表自动拿到, 用户显隐态/自定义源不动 (oneapp _news_feeds_merge 同款)。
 * - 自定义源 id = "c_" + fnv1a(url) 前 8 hex (形状同 oneapp md5[:8]; 哈希
 *   算法不必同源 — 同址重加走 url 去重, id 差异无影响)。
 * - 上限 NEWS_FEEDS_MAX=16 (oneapp 同款, 并发抓取时长可控)。
 * - 全部隐藏合法 (oneapp 同款): 新闻 app 出引导空态, 轮播自然为空。
 *
 * 云同步 (想法云 noteserver /v1/kv/news_feeds, 与 Zero oneapp 同端点同键):
 * - saveTable 后台 PUT (登录态; 失败静默, 下次变更/拉取补推)。
 * - kvPullMaybe(): worker 线程调, 60s 节流, LWW 对齐 (±2s 同刻不动):
 *   云 updated 较新 → 应用云端 (补内置后落盘, 本地 ts 对齐云 updated,
 *   不回推防两端乒乓); 本地较新 → 补推。云空/404 不动本地。
 */
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include "../psram_json.h"
#include "news_rss.h"
#include "../notes_account/notes_account_panel.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace newsfeed {

inline constexpr int NEWS_FEEDS_MAX = 16;      // 源数上限 (oneapp 同款)
inline constexpr const char* kNvsKey = "news_feeds";
inline constexpr const char* kTsKey  = "news_feeds_ts";   // 本地表最近变更 epoch

struct Entry {
    std::string id, name, url;
    bool on = true;
};

// ── 内置源补齐 (oneapp _news_feeds_merge 同款: 缺的内置追加, on=true) ──────
inline void _mergeBuiltins(std::vector<Entry>& t)
{
    auto have = [&](const std::string& id) {
        for (auto& e : t)
            if (e.id == id) return true;
        return false;
    };
    for (auto& b : news_rss::builtinFeeds()) {
        if (have(b.id)) continue;
        t.push_back({b.id, b.name, b.url, true});
    }
}

// 规范化一张表 (跳过缺 id/url 的坏条目, on 缺省 true; 防御取值 — noteserver
// 数字 bool 坑同源: nlohmann .value() 撞错型会 throw, worker 无人接 = abort)
inline std::vector<Entry> normalizeTable(const nlohmann::json& v)
{
    std::vector<Entry> t;
    if (!v.is_array()) return t;
    for (auto& e : v) {
        if (!e.is_object()) continue;
        std::string id  = NotesAccountPanel::jstr(e, "id");
        std::string url = NotesAccountPanel::jstr(e, "url");
        if (id.empty() || url.empty()) continue;
        t.push_back({id, NotesAccountPanel::jstr(e, "name", id.c_str()), url,
                     NotesAccountPanel::jbool(e, "on", true)});
    }
    return t;
}

inline std::string dumpTable(const std::vector<Entry>& t)
{
    nlohmann::json j = nlohmann::json::array();
    for (auto& e : t) {
        // 逐字段 UTF-8 清洗: 键盘输入/云端来的脏字节不能让 dump throw
        // (nlohmann 严格校验, UI 回调里 throw = abort)
        std::string id = e.id, name = e.name, url = e.url;
        news_rss::utf8_sanitize(id);
        news_rss::utf8_sanitize(name);
        news_rss::utf8_sanitize(url);
        j.push_back({{"id", id}, {"name", name}, {"url", url}, {"on", e.on}});
    }
    return j.dump();
}

inline int64_t tsLoad()
{
    return (int64_t)strtoll(GetHAL()->getConfig(kTsKey, "0").c_str(), nullptr, 10);
}

// 落盘 (ts 由调用方定: 本地变更=now, 云应用=云 updated); 不换生效表不推云
inline void storeTable(const std::vector<Entry>& t, int64_t ts)
{
    GetHAL()->setConfig(kNvsKey, dumpTable(t));
    GetHAL()->setConfig(kTsKey, std::to_string(ts));
}

// ── 读: 迁移 + 补内置 (变更时回写; 一次读全流程, 调用方随便高频) ───────────
inline std::vector<Entry> loadTable()
{
    std::string raw = GetHAL()->getConfig(kNvsKey, "");
    std::vector<Entry> t;
    bool migrated = false;
    if (!raw.empty()) {
        try {
            auto j = PsramJson::parse(raw);
            if (j.is_array() && !j.empty() && j[0].is_string()) {
                // 旧版: 选中 id 数组 → on 态映射
                std::vector<std::string> cur;
                for (auto& e : j)
                    if (e.is_string()) cur.push_back(e.get<std::string>());
                bool untouched = cur.empty() || (cur.size() == 1 && cur[0] == "ai");
                auto seeds = news_rss::builtinFeeds();
                for (auto& b : seeds) {
                    bool on = untouched;
                    if (!untouched)
                        for (auto& c : cur)
                            if (c == b.id) { on = true; break; }
                    t.push_back({b.id, b.name, b.url, on});
                }
                migrated = true;
            } else {
                t = normalizeTable(j);
            }
        } catch (...) {}
    }
    if (t.empty()) {                              // 无键/坏数据 → 内置种子
        for (auto& b : news_rss::builtinFeeds()) t.push_back({b.id, b.name, b.url, true});
        migrated = true;
    }
    size_t before = t.size();
    _mergeBuiltins(t);                            // 缺内置 → 补齐
    if (migrated || t.size() != before) {
        storeTable(t, tsLoad());                  // 迁移/补齐回写 (ts 不动)
    }
    return t;
}

// ── 写: 落盘 + 换生效表 + 后台推云 (UI 线程可直调, PUT 在 detached worker) ──
inline void kvPush();

inline void saveTable(const std::vector<Entry>& t)
{
    storeTable(t, (int64_t)time(nullptr));
    std::vector<news_rss::FeedCfg> act;
    for (auto& e : t)
        if (e.on) act.push_back({e.id, e.name, e.url});
    news_rss::setFeeds(act);
    kvPush();
}

// ── 可见源集 (页签/轮播/详情共用; 顺序 = 表序) ─────────────────────────────
inline std::vector<news_rss::FeedCfg> visibleOf(const std::vector<Entry>& t)
{
    std::vector<news_rss::FeedCfg> out;
    for (auto& e : t)
        if (e.on) out.push_back({e.id, e.name, e.url});
    return out;
}

inline std::vector<news_rss::FeedCfg> visible()
{
    return visibleOf(loadTable());
}

// 一次性把生效表喂给抓取层 (首个使用者调; 之后 saveTable 维护)
inline void ensureActive()
{
    static std::atomic<bool> s_done{false};
    if (s_done.exchange(true)) return;
    news_rss::setFeeds(visible());
}

// ── 按可见源轮转交错合并 (car-dash buildNewsItems 同款, 上限 cap) ──────────
struct Item {
    std::string cat, title, desc, ts;
};

inline std::vector<Item> interleave(const nlohmann::json& feeds,
                                    const std::vector<news_rss::FeedCfg>& srcs,
                                    size_t cap = 48)
{
    std::vector<Item> out;
    if (!feeds.is_object() || srcs.empty()) return out;
    // 每个可见源游标 + 有效源计数
    struct Cur {
        const nlohmann::json* items = nullptr;
        size_t idx = 0, count = 0;
        std::string name, id;
    };
    std::vector<Cur> curs;
    for (auto& s : srcs) {
        if (!feeds.contains(s.id) || !feeds[s.id].is_object()) continue;
        const auto& f = feeds[s.id];
        if (!f.value("ok", false)) continue;
        if (!f.contains("items") || !f["items"].is_array()) continue;
        Cur c;
        c.items = &f["items"];
        c.count = f["items"].size();
        c.name  = s.name;
        c.id    = s.id;
        if (c.count == 0) continue;
        curs.push_back(std::move(c));
    }
    if (curs.empty()) return out;
    for (size_t n = 0; n < cap; n++) {
        Cur& c = curs[n % curs.size()];
        if (c.idx >= c.count) continue;   // 该源已尽 (尾段只从剩余源出条)
        const auto& it = (*c.items)[c.idx++];
        if (!it.is_object()) continue;
        Item o;
        std::string t = it.value("t", std::string());
        if (t.empty()) continue;
        o.cat   = c.name;
        o.title = t;
        o.desc  = it.value("d", std::string());
        o.ts    = it.value("ts", std::string());
        out.push_back(std::move(o));
        if (out.size() >= cap) break;
    }
    return out;
}

// ── 云同步 (想法云 = oneapp 同一 noteserver, /v1/kv/news_feeds) ───────────
namespace kv {

inline std::mutex g_mx;                        // 串行化推/拉 (含节流状态)
inline std::atomic<bool> g_pushing{false};
inline int64_t g_last_pull = 0;                // epoch, 60s 节流

}  // namespace kv

// 后台推云 (登录态; 静默失败 — 下次变更/拉取补推). 快照在调用线程拍好,
// PUT 在 detached worker (16KB 栈, a697bcf 惯例)。
inline void kvPush()
{
    if (NotesAccountPanel::token().empty()) return;
    if (kv::g_pushing.exchange(true)) return;
    std::string body = dumpTable(loadTable());
    auto headers = NotesAccountPanel::auth_header();
    std::string url = std::string(NotesAccountPanel::BASE) + "/v1/kv/news_feeds";
    const bool ok = GetHAL()->tryRunDetached([url, body, headers]() {
        auto resp = GetHAL()->httpPut(url, body, headers);
        if (!resp.ok || resp.status != 200)
            mclog::tagWarn("newsfeed", "kv push st={}", resp.status);
        kv::g_pushing.store(false);
    }, 16 * 1024);
    if (!ok) kv::g_pushing.store(false);       // 线程起不来也必须放行下次
}

// LWW 拉取对齐 (worker 线程调; 60s 节流)。返回 true = 表被云端更新过
// (调用方应按新表重取数据/重建 UI)。
inline bool kvPullMaybe()
{
    if (NotesAccountPanel::token().empty()) return false;
    std::lock_guard<std::mutex> lk(kv::g_mx);
    int64_t now = (int64_t)time(nullptr);
    if (now - kv::g_last_pull < 60) return false;
    kv::g_last_pull = now;

    std::string url = std::string(NotesAccountPanel::BASE) + "/v1/kv/news_feeds";
    auto resp = GetHAL()->httpGet(url, NotesAccountPanel::auth_header());
    if (resp.status == 404) return false;      // 云端无键 (Zero 未配过): 正常
    if (!resp.ok || resp.status != 200) {
        mclog::tagWarn("newsfeed", "kv pull st={}", resp.status);
        return false;
    }
    nlohmann::json j;
    try {
        j = PsramJson::parse(resp.body);
    } catch (...) {
        return false;
    }
    if (!NotesAccountPanel::jbool(j, "ok") || !j.contains("v") || !j["v"].is_array())
        return false;
    auto cloud = normalizeTable(j["v"]);
    if (cloud.empty()) return false;           // 云空不落地 (清零事故同款哲学)
    _mergeBuiltins(cloud);
    int64_t upd = (int64_t)NotesAccountPanel::jnum(j, "updated");
    int64_t local = tsLoad();
    if (upd > local + 2) {
        // 云端较新 → 应用 (不回推防乒乓; 本地 ts 对齐云 updated)
        storeTable(cloud, upd);
        news_rss::setFeeds(visibleOf(cloud));
        mclog::tagInfo("newsfeed", "kv pull applied ({} srcs)", (int)cloud.size());
        return true;
    }
    if (local > upd + 2) kvPush();             // 本地较新 → 补推 (上次可能失败)
    return false;
}

}  // namespace newsfeed
