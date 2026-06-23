#pragma once

#include <ctime>

namespace stocks_refresh {

inline bool isAshareTradingTime(std::time_t utc_epoch)
{
    constexpr std::time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 UTC
    constexpr std::time_t UTC8_OFFSET_S = 8 * 60 * 60;
    if (utc_epoch < MIN_VALID_EPOCH) return false;

    std::time_t beijing_epoch = utc_epoch + UTC8_OFFSET_S;
    std::tm beijing = {};
    if (gmtime_r(&beijing_epoch, &beijing) == nullptr) return false;

    if (beijing.tm_wday == 0 || beijing.tm_wday == 6) return false;

    const int seconds = beijing.tm_hour * 3600
                      + beijing.tm_min * 60
                      + beijing.tm_sec;
    constexpr int MORNING_OPEN = 9 * 3600 + 30 * 60;
    constexpr int MORNING_CLOSE = 11 * 3600 + 30 * 60;
    constexpr int AFTERNOON_OPEN = 13 * 3600;
    constexpr int AFTERNOON_CLOSE = 15 * 3600;

    return (seconds >= MORNING_OPEN && seconds < MORNING_CLOSE)
        || (seconds >= AFTERNOON_OPEN && seconds < AFTERNOON_CLOSE);
}

}  // namespace stocks_refresh
