#pragma once
/*
 * news_rss — 新闻数据层: 设备直连中文 RSS/Atom 源 (oneapp news_rss.py
 * 固件直译, 2026-11-15; 原 Tab5 链路 = hermes :8766/news/feeds 中转;
 * 2026-11-21 对齐 oneapp: 内置源 8→11 + 动态生效源表 setFeeds)。
 *
 * - 内置源 = oneapp 全部 10 源 (大陆直连实测全通: 7 源 2026-11-14,
 *   中新网 2026-11-20, 新华国际/军事 2026-11-21) + 阮一峰周刊 (Tab5
 *   附加内置, atom, 2026-11-15 Zero 实测 200/70KB/1.9s)。
 * - 生效源表 = setFeeds([{id,name,url}]) 动态换 (news_feeds 源表模块
 *   持有持久态); 未初始化时回落全部内置 (桌面/单测兜底)。隐藏源不抓。
 * - RSS2.0 / Atom 双格式, 命名空间无关 (按 local 名匹配), CDATA 支持,
 *   剥 HTML 标签 + 实体反转义; 单源失败不影响整体 (ok:false, 不记成功时间
 *   戳 → 下轮重试, oneapp 同款)。
 * - 输出形状与 hermes /news/feeds 完全一致 (下游 app_news 页签解析、
 *   app_home 轮播 newsfeed::interleave 零改动):
 *     {"ok":true,"fetched":"MM-dd HH:MM","feeds":{<id>:{ok,name,items:[{t,d,u,ts}]}}}
 * - 每源 10min TTL 缓存 (调用方任意 worker; mutex 串行, 暖缓存命中即回;
 *   片段按源 id 键控 — 源表重排/换址后旧缓存不串位)。
 * - 时间戳统一折算北京时间显示 (gmtime+8h, 不依赖设备 TZ 设置);
 *   解析失败为 "" (oneapp 同款)。
 * - 缓存/聚合文本走 PsramString (聚合 ~100KB, 常驻落 PSRAM 防内部堆搅拌 —
 *   2026-11-07 邮件 158KB 内部堆 churn 教训同源)。
 */
#include <hal/hal.h>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <ctime>
#include <cstdint>
#include "../psram_json.h"

namespace news_rss {

inline constexpr const char* UA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 TAB5NEWS/1.0";
inline constexpr int MAX_ITEMS = 20;
inline constexpr int64_t TTL_S = 600;          // 每源缓存 (10min, oneapp 同款)

inline constexpr struct Feed {
    const char* id;
    const char* name;
    const char* url;
} FEEDS[] = {
    {"ai",        "AI 新闻",    "https://www.qbitai.com/feed"},   // 量子位
    {"ithome",    "IT之家",     "https://www.ithome.com/rss/"},
    {"sspai",     "少数派",     "https://sspai.com/feed"},
    {"solidot",   "Solidot",    "https://www.solidot.org/index.rss"},
    {"ifanr",     "爱范儿",     "https://www.ifanr.com/feed"},
    {"leiphone",  "雷峰网",     "https://www.leiphone.com/feed"},
    {"wscn",      "华尔街见闻", "https://dedicated.wallstreetcn.com/rss.xml"},
    {"chinanews", "中新网",     "https://www.chinanews.com.cn/rss/scroll-news.xml"},
    {"xh_world",  "新华国际",   "http://www.xinhuanet.com/world/news_world.xml"},
    {"xh_mil",    "新华军事",   "http://www.xinhuanet.com/mil/news_mil.xml"},
    {"ruan",      "阮一峰周刊", "https://www.ruanyifeng.com/blog/atom.xml"},  // Tab5 附加
};
inline constexpr int FEED_COUNT = (int)(sizeof(FEEDS) / sizeof(FEEDS[0]));

// 生效源表条目 (动态表: news_feeds 源表模块按显隐给; 值语义方便跨线程快照)
struct FeedCfg {
    std::string id, name, url;
};

inline std::vector<FeedCfg> builtinFeeds()
{
    std::vector<FeedCfg> out;
    out.reserve(FEED_COUNT);
    for (int i = 0; i < FEED_COUNT; i++)
        out.push_back({FEEDS[i].id, FEEDS[i].name, FEEDS[i].url});
    return out;
}

// ── 生效源表 (oneapp news_rss.set_feeds 同款: 显隐驱动, 隐藏源不抓) ─────────
// setFeeds 由 news_feeds 源表模块在启动/每次变更后调; fetchAllJson 首次调用
// 前若没人 set 过 → 回落全部内置 (桌面/单测/异常路径兜底)。换表走独立短
// 临界区 — UI 线程换表绝不等慢抓取 (取数 mutex 整段持锁, 12s×N 源)。
inline std::mutex& tableMutex()
{
    static std::mutex s_mx;
    return s_mx;
}

inline std::vector<FeedCfg>& activeFeeds()
{
    static std::vector<FeedCfg> s_active = builtinFeeds();
    return s_active;
}

inline void setFeeds(const std::vector<FeedCfg>& feeds)
{
    std::lock_guard<std::mutex> lk(tableMutex());
    activeFeeds() = feeds;
}

inline std::vector<FeedCfg> snapshotFeeds()
{
    std::lock_guard<std::mutex> lk(tableMutex());
    return activeFeeds();
}

// ── 文本清洗 ────────────────────────────────────────────────────────────────

// 实体反转义 (命名实体小表 + 数字/十六进制)
inline std::string unescape_entities(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '&') { out += s[i++]; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out += s[i++]; continue; }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent == "nbsp") out += ' ';
        else if (ent.size() > 1 && ent[0] == '#') {
            bool hex = (ent[1] == 'x' || ent[1] == 'X');
            long cp = strtol(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            if (cp > 0 && cp < 0x110000) {   // UTF-8 编码
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
            }
        } else {
            out += s.substr(i, semi - i + 1);   // 未知实体原样
        }
        i = semi + 1;
    }
    return out;
}

// 剥 HTML 标签 (oneapp 同口径: 标签→空格, 不摘 style/script 内文 — 200 字
// 截断后残余可忽略)
inline std::string strip_tags(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') { in_tag = true; out += ' '; continue; }
        if (in_tag) {
            if (c == '>') in_tag = false;
            continue;
        }
        out += c;
    }
    return out;
}

// 非法 UTF-8 序列剔除 (坏源防御: 一个源的脏字节不能拖垮整包 JSON —
// nlohmann 对字符串做严格 UTF-8 校验, 2026-11-15 实锤)
inline void utf8_sanitize(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t len = (c & 0x80) == 0   ? 1
                     : (c & 0xE0) == 0xC0 ? 2
                     : (c & 0xF0) == 0xE0 ? 3
                     : (c & 0xF8) == 0xF0 ? 4
                                          : 0;
        if (len == 0 || i + len > s.size()) { i++; continue; }
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = false; break; }
        }
        if (!ok) { i++; continue; }
        out.append(s, i, len);
        i += len;
    }
    s.swap(out);
}

inline std::string clean_text(const std::string& raw, size_t limit)
{
    // oneapp 顺序: unescape → strip → 折叠空白 (实体化过的内嵌标签整段吃掉)
    std::string s = unescape_entities(raw);
    s = strip_tags(s);
    std::string out;
    out.reserve(s.size());
    bool ws = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!ws && !out.empty()) { out += ' '; ws = true; }
            continue;
        }
        ws = false;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    utf8_sanitize(out);
    // 截断必须落在 UTF-8 字符边界 (resize 拦腰切多字节 = 非法序列,
    // nlohmann 严格校验直接拒整包 — 2026-11-15 新闻全挂真凶)
    if (out.size() > limit) {
        size_t cut = limit;
        while (cut > 0 && (((unsigned char)out[cut]) & 0xC0) == 0x80) cut--;
        out.resize(cut);
    }
    return out;
}

// CDATA 剥壳 (xml.etree 自动解, 这里手动: 首个 <![CDATA[ ... ]]> 内文优先)
inline std::string unwrap_cdata(const std::string& inner)
{
    size_t p = inner.find("<![CDATA[");
    if (p == std::string::npos) return inner;
    size_t b = p + 9;
    size_t e = inner.find("]]>", b);
    return e == std::string::npos ? inner.substr(b) : inner.substr(b, e - b);
}

// ── 标签定位 (命名空间无关: 按 local 名大小写不敏感匹配) ────────────────────

// s[lt]=='<'; 开标签全名可带 "ns:" 前缀, local 名与 name icase 相等且后随分隔符
inline bool tagname_at(const std::string& s, size_t lt, const char* name)
{
    size_t i = lt + 1;
    size_t local_begin = i;
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/') break;
        if (c == ':') local_begin = i + 1;   // 命名空间前缀
        i++;
    }
    size_t local_len = i - local_begin;
    if (local_len != std::strlen(name)) return false;
    for (size_t k = 0; k < local_len; k++) {
        char a = s[local_begin + k];
        char b = name[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return i < s.size();   // 必须停在分隔符上
}

inline size_t find_tag_open(const std::string& s, const char* name, size_t from)
{
    size_t p = from;
    while ((p = s.find('<', p)) != std::string::npos) {
        if (tagname_at(s, p, name)) return p;
        p++;
    }
    return std::string::npos;
}

inline size_t find_tag_close(const std::string& s, const char* name, size_t from)
{
    // "</name...>": 名紧跟 '/' 之后, 与开标签不同 — '/' 不在名字里
    size_t p = from;
    while ((p = s.find("</", p)) != std::string::npos) {
        size_t i = p + 2;
        size_t local_begin = i;
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>') break;
            if (c == ':') local_begin = i + 1;
            i++;
        }
        size_t local_len = i - local_begin;
        bool ok = (local_len == std::strlen(name)) && i < s.size();
        for (size_t k = 0; ok && k < local_len; k++) {
            char a = s[local_begin + k];
            char b = name[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) ok = false;
        }
        if (ok) return p;
        p++;
    }
    return std::string::npos;
}

// 首个 <name ...>...</name> 的内文 (CDATA 已剥壳; 自闭合/未闭合 = "")
inline std::string extract_field(const std::string& block, const char* name)
{
    size_t p = 0;
    while (true) {
        size_t lt = find_tag_open(block, name, p);
        if (lt == std::string::npos) return "";
        size_t gt = block.find('>', lt);
        if (gt == std::string::npos) return "";
        if (gt > lt && block[gt - 1] == '/') return "";   // 自闭合 = 空
        size_t cb = gt + 1;
        size_t ce = find_tag_close(block, name, cb);
        if (ce == std::string::npos) return "";
        return unwrap_cdata(block.substr(cb, ce - cb));
    }
}

// 候选字段名逐个提取取最长者 (2026-11-22 详情扩容: description 常是短摘要,
// content:encoded 才是全文 — 首个非空会拿到少的; 清洗后比长度, 空全跳过)
inline std::string extract_field_any(const std::string& block,
                                     const char* const* names, int n, size_t limit)
{
    std::string best;
    for (int i = 0; i < n; i++) {
        std::string c = clean_text(extract_field(block, names[i]), limit);
        if (c.size() > best.size()) best = std::move(c);
    }
    return best;
}

inline std::string extract_link(const std::string& block)
{
    size_t p = 0;
    while (true) {
        size_t lt = find_tag_open(block, "link", p);
        if (lt == std::string::npos) return "";
        size_t gt = block.find('>', lt);
        if (gt == std::string::npos) return "";
        std::string open = block.substr(lt, gt - lt);   // 开标签全文
        // Atom: href="URL" 优先 (oneapp: ch.get("href") or ch.text)
        size_t h = open.find("href=\"");
        if (h != std::string::npos) {
            size_t vb = h + 6;
            size_t ve = open.find('"', vb);
            if (ve != std::string::npos) {
                std::string u = open.substr(vb, ve - vb);
                if (!u.empty()) return u;
            }
        }
        if (gt > lt && block[gt - 1] == '/') {          // 自闭合无 href → 下一个
            p = gt;
            continue;
        }
        size_t ce = find_tag_close(block, "link", gt + 1);
        if (ce == std::string::npos) return "";
        std::string u = clean_text(block.substr(gt + 1, ce - gt - 1), 300);
        if (!u.empty() && u.find(' ') == std::string::npos) return u;
        p = ce;
    }
}

// ── 时间: RFC822 / ISO8601 → 北京时间 "MM-dd HH:MM" (gmtime+8h, 不依赖 TZ) ──
inline int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline std::string fmt_ts(const std::string& s)
{
    static const char* MON[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                  "jul", "aug", "sep", "oct", "nov", "dec"};
    int y = -1, mo = -1, d = -1, hh = -1, mi = -1;
    int off = 8 * 3600;                       // 无时区信息按北京时间 (中文源)

    if (s.size() >= 12 && s[4] == '-' && s[7] == '-') {
        // ISO8601: yyyy-mm-ddThh:mm(:ss)?(Z|±hh(:mm))?
        y  = atoi(s.substr(0, 4).c_str());
        mo = atoi(s.substr(5, 2).c_str());
        d  = atoi(s.substr(8, 2).c_str());
        size_t t = s.find('T');
        if (t != std::string::npos && s.size() > t + 5) {
            hh = atoi(s.substr(t + 1, 2).c_str());
            mi = atoi(s.substr(t + 4, 2).c_str());
        }
        bool isz = (s.find('Z', 10) != std::string::npos ||
                    s.find('z', 10) != std::string::npos);
        size_t zp = s.find_first_of("+-", 10);
        if (isz) off = 0;
        else if (zp != std::string::npos && s.size() >= zp + 3) {
            off = atoi(s.substr(zp + 1, 2).c_str()) * 3600;
            if (s.size() > zp + 4 && s[zp + 3] == ':')
                off += atoi(s.substr(zp + 4, 2).c_str()) * 60;
            if (s[zp] == '-') off = -off;
        }
    } else {
        // RFC822 分词 (逗号/冒号/空格; 顺手小写化 — 月名/时区表全小写)
        std::vector<std::string> tok;
        std::string cur;
        for (char c : s) {
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c == ',' || c == ' ' || c == ':') {
                if (!cur.empty()) tok.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) tok.push_back(cur);
        int ss = 0;
        for (size_t i = 0; i < tok.size(); i++) {
            const std::string& t = tok[i];
            if (mo == -1) {
                bool ismon = false;
                for (int m = 0; m < 12; m++) {
                    if (t == MON[m]) { mo = m + 1; ismon = true; break; }
                }
                if (ismon) {
                    for (size_t k = i; k-- > 0;) {   // 月名前最近的纯数字 = 日
                        if (tok[k].size() <= 2 &&
                            tok[k].find_first_not_of("0123456789") == std::string::npos) {
                            d = atoi(tok[k].c_str());
                            break;
                        }
                    }
                    continue;
                }
            }
            if (t.size() == 4 && t.find_first_not_of("0123456789") == std::string::npos &&
                y == -1 && mo != -1) {
                y = atoi(t.c_str());
                int got = 0;                          // 时间 = 年后紧邻 2-3 个数字段
                for (size_t k = i + 1; k < tok.size() && got < 3; k++) {
                    if (tok[k].find_first_not_of("0123456789") != std::string::npos) break;
                    if (got == 0) hh = atoi(tok[k].c_str());
                    else if (got == 1) mi = atoi(tok[k].c_str());
                    else ss = atoi(tok[k].c_str());
                    got++;
                }
                continue;
            }
            if (i == tok.size() - 1 && t.size() >= 2) {   // 末 token = 时区
                if (t == "z" || t == "ut" || t == "gmt" || t == "utc") off = 0;
                else if (t.size() == 5 && (t[0] == '+' || t[0] == '-') &&
                         t.find_first_not_of("0123456789", 1) == std::string::npos) {
                    int v = atoi(t.substr(1, 2).c_str()) * 3600 +
                            atoi(t.substr(3, 2).c_str()) * 60;
                    off = (t[0] == '-') ? -v : v;
                }
            }
        }
        (void)ss;
        if (y < 0 || mo < 0 || d < 0) return "";
    }
    if (y < 0 || mo < 0 || d < 0 || hh < 0 || mi < 0) return "";
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return "";

    int64_t epoch = days_from_civil(y, mo, d) * 86400 +
                    hh * 3600 + mi * 60 - off;
    time_t disp = (time_t)(epoch + 8 * 3600);   // 北京时间展示
    struct tm tmv;
    gmtime_r(&disp, &tmv);
    char b[24];
    strftime(b, sizeof(b), "%m-%d %H:%M", &tmv);
    return b;
}

// ── RSS/Atom 解析 (命名空间无关; oneapp parse_feed 同口径) ───────────────────

struct RssItem {
    std::string t, d, u, ts;   // 标题(120)/摘要(1600)/链接(300)/"MM-dd HH:MM"
};

inline std::vector<RssItem> parse_feed(const std::string& raw)
{
    std::vector<RssItem> out;
    size_t p = 0;
    while ((int)out.size() < MAX_ITEMS) {
        size_t ip = raw.find("<item", p);
        size_t ep = raw.find("<entry", p);
        bool use_item;
        size_t b;
        if (ip == std::string::npos && ep == std::string::npos) break;
        if (ep == std::string::npos || (ip != std::string::npos && ip < ep)) {
            use_item = true; b = ip;
        } else {
            use_item = false; b = ep;
        }
        const char* close = use_item ? "</item>" : "</entry>";
        size_t ce = raw.find(close, b);
        if (ce == std::string::npos) break;
        std::string block = raw.substr(b, ce - b);
        p = ce + strlen(close);

        RssItem it;
        it.t = clean_text(extract_field(block, "title"), 120);
        if (it.t.empty()) continue;
        static const char* const DESC_TAGS[] = {"description", "summary",
                                                "content", "encoded"};
        // "encoded" = content:encoded (local 名匹配, RSS 全文摘要); 上限
        // 200→1600: 详情弹窗滚动看全文, 列表行 64px 裁剪不受影响
        it.d  = extract_field_any(block, DESC_TAGS, 4, 1600);
        it.u  = extract_link(block);
        utf8_sanitize(it.u);   // href 原文不经 clean_text, 单独防御
        static const char* const DATE_TAGS[] = {"pubdate", "published", "updated", "date"};
        it.ts = fmt_ts(extract_field_any(block, DATE_TAGS, 4, 300));
        out.push_back(std::move(it));
    }
    return out;
}

// ── 取数入口 (返回 hermes /news/feeds 同形状 JSON 文本) ──────────────────────

inline void json_escape(std::string& out, const std::string& s)
{
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        case '\t': out += " "; break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04X", (unsigned char)c);
                out += b;
            } else {
                out += c;   // UTF-8 原样
            }
        }
    }
}

inline std::string now_bj()
{
    time_t n = time(nullptr) + 8 * 3600;
    struct tm tmv;
    gmtime_r(&n, &tmv);
    char b[24];
    strftime(b, sizeof(b), "%m-%d %H:%M", &tmv);
    return b;
}

// 单源结果片段: '"<id>":{"ok":..,"name":"..","items":[..]}' (PsramString 常驻)
inline std::string fetch_one_fragment(const FeedCfg& f)
{
    std::string fr;
    fr.reserve(4096);
    fr += "\"";
    fr += f.id;
    fr += "\":{\"ok\":";
    auto resp = GetHAL()->httpGet(
        f.url, {{"User-Agent", UA}, {"Accept", "application/rss+xml, application/xml, text/xml, */*"}});
    if (!resp.ok) {
        fr += "false,\"name\":\"";
        json_escape(fr, f.name);
        fr += "\"}";
        return fr;
    }
    auto items = parse_feed(resp.body);
    if (items.empty()) {
        fr += "false,\"name\":\"";
        json_escape(fr, f.name);
        fr += "\"}";
        return fr;
    }
    fr += "true,\"name\":\"";
    json_escape(fr, f.name);
    fr += "\",\"items\":[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i) fr += ",";
        fr += "{\"t\":\"";
        json_escape(fr, items[i].t);
        fr += "\",\"d\":\"";
        json_escape(fr, items[i].d);
        fr += "\",\"u\":\"";
        json_escape(fr, items[i].u);
        fr += "\",\"ts\":\"";
        json_escape(fr, items[i].ts);
        fr += "\"}";
    }
    fr += "]}";
    return fr;
}

// 聚合取数 (调用方已在自身 worker 线程; 取数 mutex 串行 + 每源 10min TTL):
// 抓 setFeeds 生效表快照 (隐藏源不抓, oneapp 同款); 片段按源 id 键控 —
// 源表重排/换址/增删后旧缓存不串位。失败源保留 ok:false 片段 (不缓存成功
// 时间戳 → 下轮重试, oneapp 同款); 曾成功的源失败时保留旧片段 (陈旧优于
// 空页)。
inline std::string fetchAllJson()
{
    static std::mutex s_mx;
    static std::map<std::string, PsramString> s_frag;      // id → 最近片段 (PSRAM 常驻)
    static std::map<std::string, int64_t> s_feed_at;       // id → 最近成功 epoch
    std::lock_guard<std::mutex> lk(s_mx);
    const auto feeds = snapshotFeeds();
    int64_t now = (int64_t)time(nullptr);
    for (const auto& f : feeds) {
        auto at = s_feed_at.find(f.id);
        if (at != s_feed_at.end() && at->second != 0 && now - at->second < TTL_S) continue;
        std::string frag = fetch_one_fragment(f);
        bool ok = frag.find("\"ok\":true") != std::string::npos;
        if (ok) s_feed_at[f.id] = now;
        else if (at == s_feed_at.end()) s_feed_at[f.id] = 0;
        auto& slot = s_frag[f.id];
        if (ok || at == s_feed_at.end()) slot.assign(frag.data(), frag.size());
    }
    std::string agg;
    agg.reserve(8192);
    agg += "{\"ok\":true,\"fetched\":\"";
    agg += now_bj();
    agg += "\",\"feeds\":{";
    bool first = true;
    for (const auto& f : feeds) {
        auto it = s_frag.find(f.id);
        if (it == s_frag.end() || it->second.empty()) continue;
        if (!first) agg += ",";
        first = false;
        agg.append(it->second.data(), it->second.size());
    }
    agg += "}}";
    return agg;
}

}  // namespace news_rss
