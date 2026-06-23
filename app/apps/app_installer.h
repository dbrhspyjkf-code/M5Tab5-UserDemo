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
#include "app_project_assistant/app_project_assistant.h"
#include "app_voice_input/app_voice_input.h"
#include "app_unit_puzzle/app_unit_puzzle.h"
#include "app_lora_chat/app_lora_chat.h"
#include "app_email_led/app_email_led.h"
#include "app_stocks/app_stocks.h"
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
    // ── Create instances and save raw pointers before moving into Mooncake ──
    auto home_uptr = std::make_unique<AppHome>();
    AppHome* home  = home_uptr.get();

    auto ha_uptr   = std::make_unique<AppHA>();
    AppHA* ha      = ha_uptr.get();

    auto xz_uptr   = std::make_unique<AppXiaoZhi>();
    AppXiaoZhi* xz = xz_uptr.get();

    auto set_uptr  = std::make_unique<AppSettings>();
    AppSettings* settings = set_uptr.get();

    auto pa_uptr = std::make_unique<AppProjectAssistant>();
    AppProjectAssistant* project_assistant = pa_uptr.get();

    auto up_uptr = std::make_unique<AppUnitPuzzle>();
    AppUnitPuzzle* unit_puzzle = up_uptr.get();

    auto lc_uptr = std::make_unique<AppLoraChat>();
    AppLoraChat* lora_chat = lc_uptr.get();

    auto st_uptr = std::make_unique<AppStocks>();
    AppStocks* stocks = st_uptr.get();

    // ── Install (AppHome auto-opens via onCreate → open()) ──
    mooncake::GetMooncake().installApp(std::move(home_uptr));
    int ha_id = mooncake::GetMooncake().installApp(std::move(ha_uptr));
    int xz_id = mooncake::GetMooncake().installApp(std::move(xz_uptr));
    int set_id = mooncake::GetMooncake().installApp(std::move(set_uptr));
    int pa_id = mooncake::GetMooncake().installApp(std::move(pa_uptr));
    int up_id = mooncake::GetMooncake().installApp(std::move(up_uptr));
    int lc_id = mooncake::GetMooncake().installApp(std::move(lc_uptr));
    int st_id = mooncake::GetMooncake().installApp(std::move(st_uptr));

    // ── Wire close callbacks ──
    ha->setCloseCallback([home]() { home->restoreScreen(); });
    xz->setCloseCallback([home]() { home->restoreScreen(); });
    settings->setCloseCallback([home]() { home->restoreScreen(); });
    project_assistant->setCloseCallback([home]() { home->restoreScreen(); });
    // 灯阵是从「工具」页 (AppSettings) 打开的, 上拨返回到工具页而非主屏.
    unit_puzzle->setCloseCallback([settings, set_id]() {
        (void)settings;
        mooncake::GetMooncake().openApp(set_id);
    });
    // LoRa 聊天同样从「工具」页打开, 上拨返回工具页.
    lora_chat->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });
    // 自选股也从「工具」页打开, 返回 (← / 上拨) 回到工具页.
    stocks->setCloseCallback([set_id]() {
        mooncake::GetMooncake().openApp(set_id);
    });

    // 灯阵入口放到「工具」页第 2 行 (由 AppSettings 持有), 不再占 home 一格.
    settings->setPuzzleAppId(up_id);
    settings->setLoraChatAppId(lc_id);
    settings->setStocksAppId(st_id);

    // Status-bar mail icon → open the AppSettings email sub-page. The handler
    // (1) brings the AppSettings app to the foreground (its onOpen builds the
    // tools page) and (2) jumps straight to the email list. openApp() returns
    // false if the app is already the active one — that's fine, openEmailPage
    // is idempotent and will just stack the sub-page on the existing screen.
    home->setOpenEmailHandler([settings, set_id]() {
        mooncake::GetMooncake().openApp(set_id);
        settings->openEmailPage();
    });

    // ── Register apps in the home screen ──
    home->addApp("智能家居", ha_id);
    home->addApp("小  智", xz_id);
    home->addApp("工  具", set_id);
    home->addApp("Claude", pa_id);
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

    // WiFi / HA configuration moved to the home status-bar WiFi popup; the
    // red WiFi icon there signals a failed connection, so there's no longer a
    // Settings screen to auto-open on boot.
    (void)set_id;
}
