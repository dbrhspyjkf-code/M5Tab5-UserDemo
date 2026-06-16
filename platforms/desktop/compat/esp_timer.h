#pragma once
// Desktop simulator shim for esp_timer.h.
// Only esp_timer_get_time() (microseconds since boot) is needed by the app
// layer; back it with a portable monotonic clock.
#include <cstdint>
#include <chrono>

static inline int64_t esp_timer_get_time(void)
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - t0).count();
}
