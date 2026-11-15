#pragma once
// 新股申购直连 (2026-11-15 去 OrangePi)
// 数据源 = 东方财富 datacenter RPTA_APP_IPOAPPLY (与 oneapp / hermes
// _stock_ipo.py 同源), 设备直连; 30 分钟内存缓存 (首页 60s 轮询不重复打).
// 输出格式与 hermes /stocks/ipo 对齐: 首行「新股申购」, 之后每股一行
// 「名称 MM-DD(今)」, 首页最多显示 3 条.

#include <ctime>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <hal/hal.h>
#include <mooncake_log.h>
#include "../utils/psram_json.h"

namespace ipo_direct {

static const char* kTag = "ipo-direct";
static const char* kUrl =
    "https://datacenter-web.eastmoney.com/api/data/v1/get"
    "?reportName=RPTA_APP_IPOAPPLY"
    "&columns=SECURITY_NAME_ABBR,SECURITY_CODE,APPLY_DATE,ISSUE_PRICE"
    "&pageSize=30&sortColumns=APPLY_DATE&sortTypes=-1"
    "&source=SECURITIES&client=WEB";

inline std::string bj_today_iso()
{
    std::time_t t = std::time(nullptr) + 8 * 3600;   // 北京时间
    std::tm g{};
    gmtime_r(&t, &g);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d",
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    return b;
}

struct Row {
    std::string name, date_iso, mmdd;
    bool today = false;
};

// 拉取并格式化成首页文本. 返回 true=有有效结果 (out 可为空串=无新股,
// 首页应隐藏); false=请求/解析失败 (调用方保持上次文本).
inline bool fetchText(std::string& out)
{
    struct Cache {
        std::mutex mu;
        bool has = false;
        uint32_t ms = 0;
        std::string text;
    };
    static Cache c;
    constexpr uint32_t kTtlMs = 30u * 60 * 1000;

    {
        std::lock_guard<std::mutex> lk(c.mu);
        if (c.has && GetHAL()->millis() - c.ms < kTtlMs) {
            out = c.text;
            return true;
        }
    }

    auto resp = GetHAL()->httpGet(kUrl, {{"Referer", "https://data.eastmoney.com/"},
                                         {"Accept", "application/json"},
                                         {"User-Agent", "Mozilla/5.0"}});
    if (!resp.ok) {
        mclog::tagWarn(kTag, "http {}", resp.status);
        return false;
    }

    std::vector<Row> rows;
    try {
        auto j = PsramJson::parse(resp.body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        const auto& data = j["result"]["data"];
        if (!data.is_array()) return false;
        const std::string today = bj_today_iso();
        for (const auto& r : data) {
            if (!r.is_object()) continue;
            const std::string apply =
                r.value("APPLY_DATE", std::string());   // "2026-11-23 00:00:00"
            if (apply.size() < 10) continue;
            const std::string d = apply.substr(0, 10);
            if (d < today) continue;                    // 已截止的不要
            Row row;
            row.name = r.value("SECURITY_NAME_ABBR", std::string());
            row.date_iso = d;
            row.mmdd = d.substr(5);                     // MM-DD
            row.today = (d == today);
            rows.push_back(std::move(row));
        }
    } catch (...) {
        return false;
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.date_iso < b.date_iso; });
    if (rows.size() > 5) rows.resize(5);

    std::string text;
    if (!rows.empty()) {
        text = "新股申购";
        int shown = 0;
        for (const auto& r : rows) {
            if (shown >= 3) break;   // hero 空间限制 (2026-10-28 用户定稿)
            text += "\n" + r.name + " " + r.mmdd;
            if (r.today) text += "(今)";
            shown++;
        }
    }
    {
        std::lock_guard<std::mutex> lk(c.mu);
        c.has = true;
        c.ms = GetHAL()->millis();
        c.text = text;
    }
    mclog::tagInfo(kTag, "ipo refreshed: {} items", rows.size());
    out = text;
    return true;
}

}  // namespace ipo_direct
