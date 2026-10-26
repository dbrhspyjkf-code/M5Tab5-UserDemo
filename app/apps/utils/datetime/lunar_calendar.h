/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD.
 * SPDX-License-Identifier: MIT
 *
 * 公历 → 农历 换算（1900-01-31 ~ 2100-12-31，无节气/生肖/干支）。
 *
 * 表编码（每年一个 uint32_t）：
 *   bit 0-3   : 闰月月份，0 = 当年无闰月
 *   bit 4-15  : 农历 1~12 月大小月标志（1 = 30 天，0 = 29 天），bit4 为正月
 *   bit 16    : 闰月大小（1 = 30 天）
 * 基准：1900-01-31 = 农历 1900 年正月初一。
 */
#pragma once

#include <cstdint>
#include <cstdio>

namespace lunar {

inline constexpr int kBaseYear = 1900;
inline constexpr int kLastYear = 2100;

namespace detail {

// 1900 ~ 2100 年农历信息表
inline constexpr uint32_t kLunarInfo[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,  // 1900-1909
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,  // 1910-1919
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,  // 1920-1929
    0x06566, 0x0d4a0, 0x0ea50, 0x16a95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,  // 1930-1939
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,  // 1940-1949
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,  // 1950-1959
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,  // 1960-1969
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6,  // 1970-1979
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,  // 1980-1989
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0,  // 1990-1999
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,  // 2000-2009
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,  // 2010-2019
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,  // 2020-2029
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,  // 2030-2039
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,  // 2040-2049
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0,  // 2050-2059
    0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4,  // 2060-2069
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,  // 2070-2079
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d260,  // 2080-2089
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a4d0, 0x0d150, 0x0f252,  // 2090-2099
    0x0d520,                                                                                   // 2100
};

// 天数（自 0000-03-01 起的连续纪日, Howard Hinnant 算法）。仅用于求两个公历日期的差。
inline int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);                       // [0, 399]
    const uint32_t doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;         // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                      // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

inline int leap_month_of(int lunar_year)
{
    return kLunarInfo[lunar_year - kBaseYear] & 0xf;
}

inline int leap_days_of(int lunar_year)
{
    const int lm = leap_month_of(lunar_year);
    return lm ? ((kLunarInfo[lunar_year - kBaseYear] & 0x10000) ? 30 : 29) : 0;
}

inline int month_days_of(int lunar_year, int month)  // month: 1..12, 普通月
{
    return (kLunarInfo[lunar_year - kBaseYear] & (0x10000 >> month)) ? 30 : 29;
}

inline int year_days_of(int lunar_year)
{
    int sum = 348;  // 12 × 29
    for (uint32_t bit = 0x8000; bit > 0x8; bit >>= 1)
        if (kLunarInfo[lunar_year - kBaseYear] & bit) ++sum;
    return sum + leap_days_of(lunar_year);
}

}  // namespace detail

struct LunarDate {
    int  year;    // 农历年（数字，如 2026）
    int  month;   // 1..12
    int  day;     // 1..30
    bool is_leap_month;
};

// 公历 → 农历。范围外返回 false。
inline bool solar_to_lunar(int y, int m, int d, LunarDate* out)
{
    if (!out) return false;
    if (y < kBaseYear || y > kLastYear) return false;

    const int64_t offset = detail::days_from_civil(y, m, d)
                         - detail::days_from_civil(kBaseYear, 1, 31);
    if (offset < 0) return false;

    int64_t rest = offset;
    int     ly   = kBaseYear;
    while (true) {
        const int ydays = detail::year_days_of(ly);
        if (rest < ydays) break;
        rest -= ydays;
        if (++ly > kLastYear) return false;
    }

    const int leap = detail::leap_month_of(ly);
    int lm = 1;
    bool is_leap = false;
    for (int ordinal = 1; ordinal <= 12; ++ordinal) {
        const int mdays = detail::month_days_of(ly, ordinal);
        if (rest < mdays) { lm = ordinal; is_leap = false; break; }
        rest -= mdays;
        if (leap == ordinal) {  // 闰月紧随其同名普通月之后
            const int ld = detail::leap_days_of(ly);
            if (rest < ld) { lm = ordinal; is_leap = true; break; }
            rest -= ld;
        }
        if (ordinal == 12) return false;  // 不应到达
    }

    out->year = ly;
    out->month = lm;
    out->day = static_cast<int>(rest) + 1;
    out->is_leap_month = is_leap;
    return true;
}

// 农历日期格式化为习惯写法，如 "正月初一"、"九月十五"、"闰四月十六"。
// out_size 不足时返回 false。
inline bool format_lunar_date(int y, int m, int d, char* out, size_t out_size)
{
    LunarDate ld{};
    if (!solar_to_lunar(y, m, d, &ld)) return false;

    static const char* kMonths[] = {"正", "二", "三", "四", "五", "六",
                                    "七", "八", "九", "十", "冬", "腊"};
    static const char* kDays[] = {
        "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

    const char* leap_prefix = ld.is_leap_month ? "闰" : "";
    return snprintf(out, out_size, "%s%s月%s", leap_prefix, kMonths[ld.month - 1],
                    kDays[ld.day - 1]) > 0;
}

}  // namespace lunar
