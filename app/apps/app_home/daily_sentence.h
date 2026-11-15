#pragma once
// 每日一句 —— 本地数据库 (2026-11-15 去 OrangePi)
// 数据 = daily_sentences_data.h (由 gen_daily_sentences.py 从 hermes 同款
// daily_sentences.csv 生成, 1437 句). blob 在 flash (const), 运行时只建
// 行偏移表 (~5.7KB), 按日期 ordinal + offset 轮换 —— 与 hermes
// _daily_sentence.get_daily_sentence 完全同算法 (date.toordinal() 基准
// 1970-01-01 = 719163, 北京时间).

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "daily_sentences_data.h"

namespace daily_sentence {

struct Pair {
    std::string en, cn;
};

inline const std::vector<uint32_t>& offsets()
{
    static const std::vector<uint32_t> v = []() {
        std::vector<uint32_t> o;
        const char* p = kDailySentencesBlob;
        const size_t n = std::strlen(p);
        o.reserve((size_t)kDailySentencesCount);
        o.push_back(0);
        for (size_t i = 0; i < n; i++) {
            if (p[i] == '\n' && i + 1 < n) o.push_back((uint32_t)(i + 1));
        }
        return o;
    }();
    return v;
}

inline int count()
{
    return (int)offsets().size();
}

// 北京时间当天 ordinal (Python date.toordinal 同基准)
inline long long beijing_ordinal()
{
    std::time_t t = std::time(nullptr) + 8 * 3600;
    return (long long)(t / 86400) + 719163;   // 1970-01-01 → 719163
}

// offset=0 当日句; offset=N 往后第 N 句 (点击换下一句, 轮换跨日无缝)
inline Pair get(int offset)
{
    Pair out;
    const auto& offs = offsets();
    const int n = (int)offs.size();
    if (n <= 0) return out;
    long long idx = (beijing_ordinal() + offset) % n;
    if (idx < 0) idx += n;
    const char* base = kDailySentencesBlob;
    const char* start = base + offs[(size_t)idx];
    const char* end = (idx + 1 < n) ? base + offs[(size_t)idx + 1]
                                    : base + std::strlen(base);
    std::string line(start, (size_t)(end - start));
    if (!line.empty() && line.back() == '\n') line.pop_back();
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        out.en = std::move(line);
    } else {
        out.en = line.substr(0, tab);
        out.cn = line.substr(tab + 1);
    }
    return out;
}

}  // namespace daily_sentence
