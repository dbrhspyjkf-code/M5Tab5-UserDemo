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
#include "app_email_led/app_email_led.h"
#include "app_stocks/app_stocks.h"
#include "app_rfid/app_rfid.h"
#include "app_ssh/app_ssh.h"
#include "app_radio/app_radio.h"
#include "app_player/app_player.h"
#include "app_timer/app_timer.h"
#include "app_calendar/app_calendar.h"
#include "app_health/app_health.h"
#include <cstdlib>
#include "app_idle_screen/app_idle_screen.h"
#include "xiaozhi_ctl.h"
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

    // ── Create instances and save raw pointers before moving into Mooncake ──
    auto home_uptr = std::make_unique<AppHome>();
    AppHome* home  = home_uptr.get();

    auto ha_uptr   = std::make_unique<AppHA>();
    AppHA* ha      = ha_uptr.get();

    auto xz_uptr   = std::make_unique<AppXiaoZhi>();
    AppXiaoZhi* xz = xz_uptr.get();

    auto set_uptr  = std::make_unique<AppSettings>();
    AppSettings* settings = set_uptr.get();

    auto lc_uptr = std::make_unique<AppLoraChat>();
    AppLoraChat* lora_chat = lc_uptr.get();

    auto st_uptr = std::make_unique<AppStocks>();
    AppStocks* stocks = st_uptr.get();

    auto re_uptr = std::make_unique<AppReachy>();
    AppReachy* reachy = re_uptr.get();

    auto rf_uptr = std::make_unique<AppRfid>();
    AppRfid* rfid = rf_uptr.get();

    auto sh_uptr = std::make_unique<AppSsh>();
    AppSsh* ssh = sh_uptr.get();        // SSH 终端: 工具页入口

    auto ra_uptr = std::make_unique<AppRadio>();
    AppRadio* radio = ra_uptr.get();

    auto pl_uptr = std::make_unique<AppPlayer>();
    AppPlayer* player = pl_uptr.get();   // 播放器: Sonos/HomePad,主页入口

    auto ti_uptr = std::make_unique<AppTimer>();
    AppTimer* timer = ti_uptr.get();     // 计时器: 主页入口

    auto cal_uptr = std::make_unique<AppCalendar>();
    AppCalendar* calendar = cal_uptr.get();  // 万年历: 工具页入口

    auto hl_uptr = std::make_unique<AppHealth>();
    AppHealth* health = hl_uptr.get();       // 健康: 工具页入口

    auto is_uptr = std::make_unique<AppIdleScreen>();
    AppIdleScreen* idle = is_uptr.get();

    // ── Install (AppHome auto-opens via onCreate → open()) ──
    mooncake::GetMooncake().installApp(std::move(home_uptr));
    int ha_id = mooncake::GetMooncake().installApp(std::move(ha_uptr));
    int xz_id = mooncake::GetMooncake().installApp(std::move(xz_uptr));
    int set_id = mooncake::GetMooncake().installApp(std::move(set_uptr));
    int lc_id = mooncake::GetMooncake().installApp(std::move(lc_uptr));
    int st_id = mooncake::GetMooncake().installApp(std::move(st_uptr));
    int re_id = mooncake::GetMooncake().installApp(std::move(re_uptr));
    int rf_id = mooncake::GetMooncake().installApp(std::move(rf_uptr));
    int sh_id = mooncake::GetMooncake().installApp(std::move(sh_uptr));
    int ra_id = mooncake::GetMooncake().installApp(std::move(ra_uptr));
    int pl_id = mooncake::GetMooncake().installApp(std::move(pl_uptr));
    int ti_id = mooncake::GetMooncake().installApp(std::move(ti_uptr));
    int cal_id = mooncake::GetMooncake().installApp(std::move(cal_uptr));
    int hl_id  = mooncake::GetMooncake().installApp(std::move(hl_uptr));

    // ── Wire close callbacks ──
    ha->setCloseCallback([home]() { home->restoreScreen(); });
    xz->setCloseCallback([home]() { home->restoreScreen(); });
    settings->setCloseCallback([home]() { home->restoreScreen(); });
    // LoRa 聊天同样从「工具」页打开, 上拨返回工具页.
    lora_chat->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 自选股也从「工具」页打开, 返回 (← / 上拨) 回到工具页.
    stocks->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 万年历同样从「工具」页打开, 上拨返回工具页.
    calendar->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 健康 (hermes /api/health) 同从「工具」页打开, 上拨返回工具页.
    health->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // Reachy 同样从「工具」页打开, 返回 (← / 上拨) 回到工具页.
    reachy->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // RFID 读写器 (Unit NFC, PORT A I2C) 同样从「工具」页打开, 上拨返回工具页.
    rfid->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // SSH 终端同从「工具」页打开, 上拨返回工具页.
    ssh->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 收音机是主页入口(非工具页), 关闭直接回主屏
    radio->setCloseCallback([home]() { home->restoreScreen(); });
    // 播放器(Sonos/HomePod)同为主页入口
    player->setCloseCallback([home]() { home->restoreScreen(); });
    // 计时器同为主页入口
    timer->setCloseCallback([home]() { home->restoreScreen(); });
    // 独占模式: 收音机打开期间抑制屏保(流媒体 app, 不被时钟遮盖/省轮询)
    radio->setSuppressor([idle](bool s) { idle->setSuppressed(s); });
    // 后台播放: 主页状态栏音符图标读收音机流状态 (在播→显示, 点按回收音机)
    home->setNowPlayingGetter([radio]() -> std::string { return radio->streamInfo(); });

    settings->setLoraChatAppId(lc_id);
    settings->setStocksAppId(st_id);
    settings->setCalendarAppId(cal_id);
    settings->setHealthAppId(hl_id);
    settings->setReachyAppId(re_id);
    settings->setRfidAppId(rf_id);
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

    // ── Global wake-word callback ─────────────────────────────────────
    // When the always-on AFE detects "你好小智", switch to the xiaozhi app
    // regardless of which screen is currently active. The callback runs on
    // xiaozhi's main task, so defer to LVGL thread via lv_async_call.
    struct WakeOpenContext {
        AppIdleScreen* idle;
        int xz_id;
    };
    static WakeOpenContext wake_open_ctx;
    wake_open_ctx = {idle, xz_id};
    xiaozhi_register_wake_callback([](const std::string&) {
        lv_async_call([](void* ud) {
            auto* ctx = static_cast<WakeOpenContext*>(ud);
            if (ctx && ctx->idle) ctx->idle->dismiss();
            if (ctx) mooncake::GetMooncake().openApp(ctx->xz_id);
        }, &wake_open_ctx);
    });

    // ── Register apps in the home screen ──
    home->addApp("智能家居", ha_id);
    home->addApp("小  智", xz_id);
    home->addApp("金融", st_id);   // 2026-10-28: 入口从工具页移到主页底部
    home->addApp("收音机", ra_id);
    home->addApp("播放器", pl_id);
    home->addApp("计时器", ti_id);
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

    // ── Voice input WorkerAbility (always-on, not a user-launchable app) ──
    int vi_id = mooncake::GetMooncake().extensionManager()->createAbility(
        std::make_unique<AppVoiceInput>());
    mooncake::GetMooncake().extensionManager()->resumeWorkerAbility(vi_id);

    // ── Email LED notifier WorkerAbility (always-on) ──
    // 后台轮询未读邮件, 有未读时在 PORT A 的 8x8 矩阵滚动蓝色 "NEW EMAIL".
    // 与「灯阵」/「LoRa」app 通过 AppEmailLed::setPortAOwnedByApp 协调 GPIO53.
    int el_id = mooncake::GetMooncake().extensionManager()->createAbility(
        std::make_unique<AppEmailLed>());
    mooncake::GetMooncake().extensionManager()->resumeWorkerAbility(el_id);

    // ── Idle screensaver WorkerAbility (always-on) ──
    // 20s 无操作后全屏显示时间/日期/天气, 碰屏幕即关闭。天气数据复用 AppHome
    // 状态栏已有的缓存, 不额外发请求。
    is_uptr->setHomeApp(home);
    int is_id = mooncake::GetMooncake().extensionManager()->createAbility(std::move(is_uptr));
    mooncake::GetMooncake().extensionManager()->resumeWorkerAbility(is_id);

    // WiFi / HA configuration moved to the home status-bar WiFi popup; the
    // red WiFi icon there signals a failed connection, so there's no longer a
    // Settings screen to auto-open on boot.
    (void)set_id;
}
