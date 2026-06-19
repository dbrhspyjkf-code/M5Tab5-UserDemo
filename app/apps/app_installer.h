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

    // ── Install (AppHome auto-opens via onCreate → open()) ──
    mooncake::GetMooncake().installApp(std::move(home_uptr));
    int ha_id = mooncake::GetMooncake().installApp(std::move(ha_uptr));
    int xz_id = mooncake::GetMooncake().installApp(std::move(xz_uptr));
    int set_id = mooncake::GetMooncake().installApp(std::move(set_uptr));
    int pa_id = mooncake::GetMooncake().installApp(std::move(pa_uptr));
    int up_id = mooncake::GetMooncake().installApp(std::move(up_uptr));

    // ── Wire close callbacks ──
    ha->setCloseCallback([home]() { home->restoreScreen(); });
    xz->setCloseCallback([home]() { home->restoreScreen(); });
    settings->setCloseCallback([home]() { home->restoreScreen(); });
    project_assistant->setCloseCallback([home]() { home->restoreScreen(); });
    unit_puzzle->setCloseCallback([home]() { home->restoreScreen(); });

    // 灯阵入口放到「工具」页第 2 行 (由 AppSettings 持有), 不再占 home 一格.
    settings->setPuzzleAppId(up_id);

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

    // WiFi / HA configuration moved to the home status-bar WiFi popup; the
    // red WiFi icon there signals a failed connection, so there's no longer a
    // Settings screen to auto-open on boot.
    (void)set_id;
}
