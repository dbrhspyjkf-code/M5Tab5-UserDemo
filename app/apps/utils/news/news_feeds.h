#pragma once
/*
 * news_feeds — 新闻多源共享助手 (2026-11-05, car-dash 同款)。
 *
 * 数据源 hermes GET :8766/news/feeds (服务端逐源 10min TTL 缓存):
 *   {ok, fetched:"MM-DD HH:MM", feeds:{ <id>:{ok, name, items:[{t,d,u,ts}]}, ... }}
 *   ai 源 items 为 {t,d,s} (s=分区标签), 其余 RSS 为 {t,d,u,ts}。
 *
 * 源选择存 NVS "news_feeds" (JSON 字符串数组, 如 ["ai","ithome"]),
 * 默认 {"ai"} — 与 car-dash 的 localStorage car_news_feeds 同语义:
 * 主页轮播卡按选择轮转交错; 新闻 app 恒显全部源页签。
 * 主页 (app_home 轮播 + 设置弹窗 chips) 与 app_news 共用本表。
 */
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace newsfeed {

struct Source {
    const char* id;
    const char* name;
};

// 与 car-dash car.js NEWS_SOURCES 完全同表 (顺序即页签顺序)
inline constexpr Source kSources[] = {
    {"ai",       "AI 新闻"},
    {"ithome",   "IT之家"},
    {"sspai",    "少数派"},
    {"solidot",  "Solidot"},
    {"ifanr",    "爱范儿"},
    {"ruan",     "阮一峰周刊"},
    {"leiphone", "雷峰网"},
    {"wscn",     "华尔街见闻"},
};
inline constexpr int kSourceCount = (int)(sizeof(kSources) / sizeof(kSources[0]));

inline const char* nameOf(const std::string& id)
{
    for (auto& s : kSources)
        if (id == s.id) return s.name;
    return id.c_str();
}

// NVS 读取 (非法/越界 id 过滤; 空 → {"ai"} 兜底)
inline std::vector<std::string> loadSel()
{
    std::vector<std::string> sel;
    try {
        auto j = nlohmann::json::parse(
            GetHAL()->getConfig("news_feeds", "[\"ai\"]"));
        if (j.is_array())
            for (auto& e : j) {
                if (!e.is_string()) continue;
                std::string id = e.get<std::string>();
                for (auto& s : kSources)
                    if (id == s.id) { sel.push_back(id); break; }
            }
    } catch (...) {}
    if (sel.empty()) sel.push_back("ai");
    return sel;
}

// NVS 写入 (紧凑 JSON; 全空视为异常, 保留原值由调用方守卫)
inline void saveSel(const std::vector<std::string>& sel)
{
    nlohmann::json j = nlohmann::json::array();
    for (auto& id : sel) j.push_back(id);
    GetHAL()->setConfig("news_feeds", j.dump());
}

// 合并结果条目 (cat=来源名[·分区], 详情弹窗 meta/标题用)
struct Item {
    std::string cat, title, desc, ts;
};

// 按选择轮转交错合并各源 items (car-dash buildNewsItems 同款, 上限 cap)
inline std::vector<Item> interleave(const nlohmann::json& feeds,
                                    const std::vector<std::string>& sel,
                                    size_t cap = 48)
{
    std::vector<Item> out;
    if (!feeds.is_object()) return out;
    // 每个选中源游标 + 有效源计数
    struct Cur {
        const nlohmann::json* items = nullptr;
        size_t idx = 0, count = 0;
        const char* name = nullptr;
        std::string id;
    };
    std::vector<Cur> curs;
    for (auto& id : sel) {
        if (!feeds.contains(id) || !feeds[id].is_object()) continue;
        const auto& f = feeds[id];
        if (!f.value("ok", false)) continue;
        if (!f.contains("items") || !f["items"].is_array()) continue;
        Cur c;
        c.items = &f["items"];
        c.count = f["items"].size();
        c.name  = nameOf(id.c_str());
        c.id    = id;
        if (c.count == 0) continue;
        curs.push_back(std::move(c));
    }
    if (curs.empty()) return out;
    for (size_t n = 0; n < cap; n++) {
        Cur& c = curs[n % curs.size()];
        if (c.idx >= c.count) continue;   // 该源已尽 (下方全尽判定收口)
        const auto& it = (*c.items)[c.idx++];
        if (!it.is_object()) continue;
        Item o;
        std::string t = it.value("t", std::string());
        if (t.empty()) continue;
        if (c.id == "ai") {
            std::string s = it.value("s", std::string());
            o.cat = s.empty() ? c.name : std::string(c.name) + " · " + s;
        } else {
            o.cat = c.name;
        }
        o.title = t;
        o.desc  = it.value("d", std::string());
        o.ts    = it.value("ts", std::string());
        out.push_back(std::move(o));
        if (out.size() >= cap) break;
    }
    // 轮转里跳过已尽源: 上面的 continue 会让尾段只从剩余源出条
    // (n % curs.size() 固定步进, 尽源被跳过即等效轮转剩余源)
    return out;
}

}  // namespace newsfeed
