/*
 * PowerScheduler — 定时开关机 (2026-11-01)
 *
 * 场景:夜间 0:00 自动关机、早 6:30 由 RX8130 RTC 闹钟自动开机。
 * 关机 = bsp_generate_poweroff_signal() 真断电;开机 = 断电前武装的 RTC
 * 每日闹钟到点拉 IRQ 重新上电(冷启动)。闹钟只在 powerOff 前一刻设置,
 * 运行期间不挂(开机流程 clearRtcIrq 会清,也避免带电状态 IRQ 行为未验证)。
 *
 * 配置 (NVS,设置弹窗读写):
 *   psched_en  "1"/"0"   总开关,默认开(2026-11-01 用户需求:夜间 0:00-6:30 不需要)
 *   psched_off "HH:MM"   每日关机时刻,默认 00:00
 *   psched_on  "HH:MM"   每日开机时刻(RTC 闹钟),默认 06:30
 *
 * 无 RTC 闹钟能力的平台(桌面模拟器 hasDailyRtcAlarm()=false)只允许配置、
 * 到点不动作。header-only,由 app_installer 在启动时 start()。
 */
#pragma once
#include <hal/hal.h>
#include <lvgl.h>
#include <mooncake_log.h>
#include <cstdio>
#include <ctime>
#include <string>

namespace power_scheduler {

inline constexpr int kPollMs = 30 * 1000;  // 分钟级判定,30s 轮询必命中一次

inline const char* kKeyEn  = "psched_en";
inline const char* kKeyOff = "psched_off";
inline const char* kKeyOn  = "psched_on";

inline const std::string _tag = "psched";

inline bool parseHHMM(const std::string& s, int* h, int* m)
{
    if (sscanf(s.c_str(), "%d:%d", h, m) != 2) return false;
    return *h >= 0 && *h < 24 && *m >= 0 && *m < 60;
}

inline std::string fmtHHMM(int h, int m)
{
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", h, m);
    return b;
}

inline void _tick(lv_timer_t*)
{
    auto* hal = GetHAL();
    if (!hal->hasDailyRtcAlarm()) return;      // 桌面模拟器:只配置不动作
    if (hal->getConfig(kKeyEn, "1") != "1") return;  // 默认开(2026-11-01 用户需求 0:00-6:30 夜间断电)

    int oh, om, nh, nm;
    if (!parseHHMM(hal->getConfig(kKeyOff, "00:00"), &oh, &om)) return;
    if (!parseHHMM(hal->getConfig(kKeyOn, "06:30"), &nh, &nm)) return;
    if (oh * 60 + om == nh * 60 + nm) return;  // 关机==开机,无效配置

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    int now_min = lt.tm_hour * 60 + lt.tm_min;

    // 防同分钟重复触发(万一 powerOff 未断成)
    static int s_fired_min = -1;
    if (now_min == oh * 60 + om && s_fired_min != now_min) {
        s_fired_min = now_min;
        mclog::tagInfo(_tag, "scheduled off {:02d}:{:02d} hit -> power off (rtc alarm {:02d}:{:02d})",
                       oh, om, nh, nm);
        // powerOff 内部:psched_en=1 时先武装每日 RTC 闹钟再断电
        hal->powerOff();
    }
}

inline void start()
{
    // lv_timer 须在 lvgl 锁内创建(app 启动期主线程与 port 任务并发)
    GetHAL()->lvglLock();
    lv_timer_create(_tick, kPollMs, nullptr);
    GetHAL()->lvglUnlock();
    mclog::tagInfo(_tag, "power scheduler started (poll {} ms)", kPollMs);
}

}  // namespace power_scheduler
