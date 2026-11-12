/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake.h>
#include <memory>
#include <hal/hal.h>
#include "app_template/app_template.h"
#include "app_launcher/app_launcher.h"
#include "app_startup_anim/app_startup_anim.h"
#include "app_ha/app_ha.h"
#include "app_home/app_home.h"
#include "app_xiaozhi/app_xiaozhi.h"
#include "app_settings/app_settings.h"
#include "app_reachy/app_reachy.h"
#include "app_voice_input/app_voice_input.h"
#include "app_lora_chat/app_lora_chat.h"
#include "app_stocks/app_stocks.h"
#include "app_rfid/app_rfid.h"
#include "app_ssh/app_ssh.h"
#include "app_radio/app_radio.h"
#include "app_player/app_player.h"
#include "app_timer/app_timer.h"
#include "app_calendar/app_calendar.h"
#include "app_health/app_health.h"
#include "app_lottery/app_lottery.h"
#include "app_gallery/app_gallery.h"
#include "app_idea/app_idea.h"
#include "app_news/app_news.h"
#include "app_games/app_games.h"
#include "app_yt/app_yt.h"
#include <cstdlib>
#include <cstring>
#include "app_idle_screen/app_idle_screen.h"
#include "xiaozhi_ctl.h"
#include "utils/power_scheduler.h"
/* Header files locator (Don't remove) */

// Start boot anim app and wait for it to finish
inline void on_startup_anim()
{
    auto app_id = mooncake::GetMooncake().installApp(std::make_unique<AppStartupAnim>());
    mooncake::GetMooncake().openApp(app_id);
    while (1) {
        mooncake::GetMooncake().update();
        if (mooncake::GetMooncake().getAppCurrentState(app_id) == mooncake::AppAbility::StateSleeping) {
            break;
        }
        GetHAL()->delay(1);
    }
    mooncake::GetMooncake().uninstallApp(app_id);
}

/**
 * @brief App 安装回调
 *
 * @param mooncake
 */
inline void on_install_apps()
{
    // ── Start xiaozhi's audio task at boot ───────────────────────────────
    // Audio pipeline + AFE wake word stay resident for the life of the
    // device. xiaozhi_start_task() is idempotent.
    xiaozhi_start_task();

    // ── 定时开关机:常驻 30s 轮询,到 psched_off 时刻断电(断电前由
    // powerOff 武装 psched_on 的 RTC 每日闹钟,次晨自动开机) ──────────
    power_scheduler::start();

    // ── Create instances and save raw pointers before moving into Mooncake ──
    auto home_uptr = std::make_unique<AppHome>();
    AppHome* home  = home_uptr.get();

    auto ha_uptr   = std::make_unique<AppHA>();
    AppHA* ha      = ha_uptr.get();

    auto xz_uptr   = std::make_unique<AppXiaoZhi>();

    auto set_uptr  = std::make_unique<AppSettings>();
    AppSettings* settings = set_uptr.get();

#ifndef PLATFORM_BUILD_WEB
    auto lc_uptr = std::make_unique<AppLoraChat>();
    AppLoraChat* lora_chat = lc_uptr.get();
#endif

    auto st_uptr = std::make_unique<AppStocks>();
    AppStocks* stocks = st_uptr.get();

#ifndef PLATFORM_BUILD_WEB
    auto re_uptr = std::make_unique<AppReachy>();
    AppReachy* reachy = re_uptr.get();

    auto rf_uptr = std::make_unique<AppRfid>();
    AppRfid* rfid = rf_uptr.get();
#endif

    auto sh_uptr = std::make_unique<AppSsh>();
    AppSsh* ssh = sh_uptr.get();        // SSH 终端: 工具页入口

    auto ra_uptr = std::make_unique<AppRadio>();
    AppRadio* radio = ra_uptr.get();

#ifndef PLATFORM_BUILD_WEB
    auto pl_uptr = std::make_unique<AppPlayer>();
    AppPlayer* player = pl_uptr.get();   // 播放器: Sonos/HomePod,主页入口 (web 不移植)
#endif

    auto ti_uptr = std::make_unique<AppTimer>();
    AppTimer* timer = ti_uptr.get();     // 计时器: 主页入口

    auto cal_uptr = std::make_unique<AppCalendar>();
    AppCalendar* calendar = cal_uptr.get();  // 万年历: 工具页入口

    auto hl_uptr = std::make_unique<AppHealth>();
    AppHealth* health = hl_uptr.get();       // 健康: 工具页入口

    auto lo_uptr = std::make_unique<AppLottery>();
    AppLottery* lottery = lo_uptr.get();     // 双色球: 工具页入口

    auto ga_uptr = std::make_unique<AppGallery>();
    AppGallery* gallery = ga_uptr.get();     // 图库: 工具页入口

    auto id_uptr = std::make_unique<AppIdea>();
    AppIdea* idea = id_uptr.get();           // 想法: 主页入口 (2026-11-05 替换计时器按钮; 计时器迁工具页)

    auto nw_uptr = std::make_unique<AppNews>();
    AppNews* news = nw_uptr.get();           // 新闻: 主页入口 (2026-11-05 替换小智按钮)

#ifndef PLATFORM_BUILD_WEB
    // 游戏/油管仅设备固件 (web 端冻结不接入, 2026-11-06 用户明确)
    auto gm_uptr = std::make_unique<AppGames>();
    AppGames* games = gm_uptr.get();         // 游戏: 工具页入口 (car-dash 4 小游戏移植)

    auto yt_uptr = std::make_unique<AppYt>();
    AppYt* yt = yt_uptr.get();               // 油管: 工具页入口 (Pi 中继音频版)
#endif

#ifndef PLATFORM_BUILD_WEB
    auto is_uptr = std::make_unique<AppIdleScreen>();
    AppIdleScreen* idle = is_uptr.get();
#endif

    // ── Install (AppHome auto-opens via onCreate → open()) ──
    mooncake::GetMooncake().installApp(std::move(home_uptr));
    int ha_id = mooncake::GetMooncake().installApp(std::move(ha_uptr));
    int xz_id = mooncake::GetMooncake().installApp(std::move(xz_uptr));
    int set_id = mooncake::GetMooncake().installApp(std::move(set_uptr));
#ifndef PLATFORM_BUILD_WEB
    int lc_id = mooncake::GetMooncake().installApp(std::move(lc_uptr));
#endif
    int st_id = mooncake::GetMooncake().installApp(std::move(st_uptr));
#ifndef PLATFORM_BUILD_WEB
    int re_id = mooncake::GetMooncake().installApp(std::move(re_uptr));
    int rf_id = mooncake::GetMooncake().installApp(std::move(rf_uptr));
#endif
    int sh_id = mooncake::GetMooncake().installApp(std::move(sh_uptr));
    int ra_id = mooncake::GetMooncake().installApp(std::move(ra_uptr));
#ifndef PLATFORM_BUILD_WEB
    int pl_id = mooncake::GetMooncake().installApp(std::move(pl_uptr));
#endif
    int ti_id = mooncake::GetMooncake().installApp(std::move(ti_uptr));
    int cal_id = mooncake::GetMooncake().installApp(std::move(cal_uptr));
    int hl_id  = mooncake::GetMooncake().installApp(std::move(hl_uptr));
    int lo_id  = mooncake::GetMooncake().installApp(std::move(lo_uptr));
    int ga_id  = mooncake::GetMooncake().installApp(std::move(ga_uptr));
    int id_id  = mooncake::GetMooncake().installApp(std::move(id_uptr));
    int nw_id  = mooncake::GetMooncake().installApp(std::move(nw_uptr));
#ifndef PLATFORM_BUILD_WEB
    int gm_id  = mooncake::GetMooncake().installApp(std::move(gm_uptr));
    int yt_id  = mooncake::GetMooncake().installApp(std::move(yt_uptr));
#endif

    // ── Wire close callbacks ──
    ha->setCloseCallback([home]() { home->restoreScreen(); });
    // xz 不接 restoreScreen: 悬浮层架构下活动屏从未被切走 (2026-11-11),
    // 关闭小智若回主页反而会把底下正在显示的 app 页面拽回主页。
    settings->setCloseCallback([home]() { home->restoreScreen(); });
#ifndef PLATFORM_BUILD_WEB
    // LoRa 聊天同样从「工具」页打开, 上拨返回工具页.
    lora_chat->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
#endif
    // 自选股也从「工具」页打开, 返回 (← / 上拨) 回到工具页.
    // 金融是主页入口 (2026-10-28 从工具页移到主页底部), 上拨退出回主页。
    // 2026-11-05 修: 此前仍是 openApp(set_id) 遗留 → 金融退出落到工具页,
    // 且金融残留的手势钩子在工具页上拨时反复 openApp(工具页), 工具页无法退出。
    stocks->setCloseCallback([home]() { home->restoreScreen(); });
    // 万年历同样从「工具」页打开, 上拨返回工具页.
    calendar->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 健康 (hermes /api/health) 同从「工具」页打开, 上拨返回工具页.
    health->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 双色球同从「工具」页打开, 上拨返回工具页.
    lottery->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 图库 (共享相簿浏览) 同从「工具」页打开, 上拨返回工具页.
    gallery->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
#ifndef PLATFORM_BUILD_WEB
    // 游戏合集同从「工具」页打开, 上拨返回工具页.
    games->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 油管 (Pi 中继音频版) 同从「工具」页打开, 上拨返回工具页.
    yt->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
#endif
#ifndef PLATFORM_BUILD_WEB
    // Reachy 同样从「工具」页打开, 返回 (← / 上拨) 回到工具页.
    reachy->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // RFID 读写器 (Unit NFC, PORT A I2C) 同样从「工具」页打开, 上拨返回工具页.
    rfid->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
#endif
    // SSH 终端同从「工具」页打开, 上拨返回工具页.
    ssh->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 收音机是主页入口(非工具页), 关闭直接回主屏
    radio->setCloseCallback([home]() { home->restoreScreen(); });
    // 新闻同为主页入口, 关闭直接回主屏 (2026-11-05 替换小智按钮)
#ifndef PLATFORM_BUILD_WEB
    // 播放器(Sonos/HomePod)同为主页入口
    player->setCloseCallback([home]() { home->restoreScreen(); });
#endif
    // 计时器同为主页入口
    timer->setCloseCallback([home]() { home->restoreScreen(); });
    // 想法同为主页入口 (2026-11-05: 替换主页「计时器」按钮位)
    idea->setCloseCallback([home]() { home->restoreScreen(); });
    // 新闻同为主页入口
    news->setCloseCallback([home]() { home->restoreScreen(); });
#ifndef PLATFORM_BUILD_WEB
    // 独占模式: 收音机打开期间抑制屏保(流媒体 app, 不被时钟遮盖/省轮询)
    radio->setSuppressor([idle](bool s) { idle->setSuppressed(s); });
    // 前台独占(2026-10-31): 收音机/播放器界面期间完全不进屏保(不弹照片/
    // 时钟遮盖, 也不熄屏); 收音机后台播放仍保留"不弹遮盖但允许熄屏"语义
    radio->setBlocker([idle](bool b) { idle->setBlocked(b); });
    player->setBlocker([idle](bool b) { idle->setBlocked(b); });
    // 图库浏览照片期间同样前台独占 (屏保本身就是照片轮播, 看图时不该被它盖住)
    gallery->setBlocker([idle](bool b) { idle->setBlocked(b); });
#endif
    // 后台播放: 主页状态栏音符图标读收音机流状态 (在播→显示, 点按回收音机)
    home->setNowPlayingGetter([radio]() -> std::string { return radio->streamInfo(); });
    // 油管后台播放 (2026-11-06): 主页 ♪YT 图标状态 + 点击直达播放页
    home->setYtNowPlayingGetter([yt]() -> std::string { return yt->nowPlayingInfo(); });
    home->setOpenYtHandler([yt_id]() { mooncake::GetMooncake().openApp(yt_id); });
    // 音频焦点互踢: 油管开播前暂停收音机 (开收音机踢油管见 AppRadio)
    yt->setRadioStopper([radio]() { radio->pauseForFocus(); });
    // 反向: 收音机开播踢油管
    radio->setYtStopper([yt]() { yt->pauseForFocus(); });

#ifndef PLATFORM_BUILD_WEB
    settings->setLoraChatAppId(lc_id);
#endif
    settings->setStocksAppId(st_id);
    settings->setCalendarAppId(cal_id);
    settings->setHealthAppId(hl_id);
    settings->setLotteryAppId(lo_id);
    settings->setGalleryAppId(ga_id);
    // 计时器入口 2026-11-05 从主页迁到工具页
    settings->setTimerAppId(ti_id);
#ifndef PLATFORM_BUILD_WEB
    settings->setGamesAppId(gm_id);
    settings->setYtAppId(yt_id);
#endif
    // 主页健康卡长按 → 打开健康 app (工具页同款入口)
    home->setOpenHealthHandler([hl_id]() {
        mooncake::GetMooncake().openApp(hl_id);
    });
#ifndef PLATFORM_BUILD_WEB
    settings->setReachyAppId(re_id);
    settings->setRfidAppId(rf_id);
#endif
    settings->setSshAppId(sh_id);

    // Status-bar mail icon → open the AppSettings email sub-page. The handler
    // (1) brings the AppSettings app to the foreground (its onOpen builds the
    // tools page) and (2) jumps straight to the email list. openApp() returns
    // false if the app is already the active one — that's fine, openEmailPage
    // is idempotent and will just stack the sub-page on the existing screen.
    home->setOpenEmailHandler([settings, set_id]() {
        mooncake::GetMooncake().openApp(set_id);
        settings->openEmailPage();
    });

    // ── Desktop debug hook: TAB5_OPEN_APP=<name> 启动直达 app ──────────────
    // 模拟器手势/点击自动化不稳定时用它做 UI 冒烟 (与 TAB5_THEME 同款惯例)。
#ifdef PLATFORM_BUILD_DESKTOP
    if (const char* want = std::getenv("TAB5_OPEN_APP")) {
        if (strcmp(want, "health") == 0) mooncake::GetMooncake().openApp(hl_id);
        else if (strcmp(want, "lottery") == 0) mooncake::GetMooncake().openApp(lo_id);
        else if (strcmp(want, "tools") == 0) mooncake::GetMooncake().openApp(set_id);
        else if (strcmp(want, "calendar") == 0) mooncake::GetMooncake().openApp(cal_id);
        else if (strcmp(want, "stocks") == 0) mooncake::GetMooncake().openApp(st_id);
        else if (strcmp(want, "radio") == 0) mooncake::GetMooncake().openApp(ra_id);
#ifndef PLATFORM_BUILD_WEB
        else if (strcmp(want, "player") == 0) mooncake::GetMooncake().openApp(pl_id);
#endif
        else if (strcmp(want, "gallery") == 0) mooncake::GetMooncake().openApp(ga_id);
        else if (strcmp(want, "idea") == 0) mooncake::GetMooncake().openApp(id_id);
        else if (strcmp(want, "news") == 0) mooncake::GetMooncake().openApp(nw_id);
#ifndef PLATFORM_BUILD_WEB
        else if (strcmp(want, "games") == 0) mooncake::GetMooncake().openApp(gm_id);
        else if (strcmp(want, "youtube") == 0) mooncake::GetMooncake().openApp(yt_id);
#endif
    }
#endif

    // ── Global wake-word callback ─────────────────────────────────────
    // When the always-on AFE detects "你好小智", switch to the xiaozhi app
    // regardless of which screen is currently active. The callback runs on
    // xiaozhi's main task, so defer to LVGL thread via lv_async_call.
    // web 版无屏保 worker, idle 恒 nullptr。
    struct WakeOpenContext {
        AppIdleScreen* idle;
        int xz_id;
    };
    static WakeOpenContext wake_open_ctx;
#ifndef PLATFORM_BUILD_WEB
    wake_open_ctx = {idle, xz_id};
#else
    wake_open_ctx = {nullptr, xz_id};
#endif
    xiaozhi_register_wake_callback([](const std::string&) {
        lv_async_call([](void* ud) {
            auto* ctx = static_cast<WakeOpenContext*>(ud);
            if (ctx && ctx->idle) ctx->idle->dismiss();
            if (ctx) mooncake::GetMooncake().openApp(ctx->xz_id);
        }, &wake_open_ctx);
    });

    // ── Register apps in the home screen ──
    home->addApp("智能家居", ha_id);
    // 2026-11-05: 「小智」按钮 →「新闻」(xiaozhi 仍在装, 唤醒词"你好小智"照常进入)
    home->addApp("新闻", nw_id);
    home->addApp("行情", st_id);   // 2026-10-28: 入口从工具页移到主页底部; 2026-11-11 改名行情
    home->addApp("收音机", ra_id);
#ifndef PLATFORM_BUILD_WEB
    home->addApp("播放器", pl_id);
#endif
    // 2026-11-05: 主页「计时器」按钮 →「想 法」(计时器迁工具页, 入口不丢)
    home->addApp("想 法", id_id);
    home->addApp("工  具", set_id);   // 2026-10-28: 移到最右 (用户指定)
#ifdef PLATFORM_BUILD_DESKTOP
    // 桌面调试: TAB5_OPEN_CALENDAR=1 启动即打开万年历; TAB5_OPEN_SSH=1 打开 SSH 表单
    if (const char* e = getenv("TAB5_OPEN_CALENDAR")) {
        if (e[0] == '1') mooncake::GetMooncake().openApp(cal_id);
    }
    if (const char* e = getenv("TAB5_OPEN_SSH")) {
        if (e[0] == '1') mooncake::GetMooncake().openApp(sh_id);
    }
#endif
    /* Install app locator (Don't remove) */

#ifndef PLATFORM_BUILD_WEB
    // ── Voice input WorkerAbility (always-on, not a user-launchable app) ──
    int vi_id = mooncake::GetMooncake().extensionManager()->createAbility(
        std::make_unique<AppVoiceInput>());
    mooncake::GetMooncake().extensionManager()->resumeWorkerAbility(vi_id);

    // Email LED notifier WorkerAbility 已移除 (2026-11-12 用户明确邮件不再需要
    // LED 功能, 灯阵硬件 2026-11-08 已清): 后台常驻实例不再创建。未读邮件由
    // 主页状态栏 (120s) 与工具页自行轮询; PORT A 仲裁转发仍走
    // AppEmailLed::setPortAOwnedByApp 静态接口 (无需实例)。

    // ── Idle screensaver WorkerAbility (always-on) ──
    // 20s 无操作后全屏显示时间/日期/天气, 碰屏幕即关闭。天气数据复用 AppHome
    // 状态栏已有的缓存, 不额外发请求。
    is_uptr->setHomeApp(home);
    int is_id = mooncake::GetMooncake().extensionManager()->createAbility(std::move(is_uptr));
    mooncake::GetMooncake().extensionManager()->resumeWorkerAbility(is_id);
#endif  // !PLATFORM_BUILD_WEB

    // WiFi / HA configuration moved to the home status-bar WiFi popup; the
    // red WiFi icon there signals a failed connection, so there's no longer a
    // Settings screen to auto-open on boot.
    (void)set_id;
}
