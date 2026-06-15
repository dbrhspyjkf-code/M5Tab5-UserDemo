/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app.h"
#include "hal/hal.h"
#include "apps/app_installer.h"
#include "screensaver/screensaver.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <lvgl.h>
#include <string>
#include <thread>

using namespace mooncake;

static const std::string _tag = "app";

void app::Init(InitCallback_t callback)
{
    mclog::tagInfo(_tag, "init");

    mclog::tagInfo(_tag, "hal injection");
    if (callback.onHalInjection) {
        callback.onHalInjection();
    }

    GetMooncake();

    on_startup_anim();
    on_install_apps();

    screensaver::init();
}

void app::Update()
{
    {
        LvglLockGuard lock;
        GetMooncake().update();
        // Idle-wallpaper screensaver. lv_display_get_inactive_time() tracks the
        // last touch across every app, so no app needs to be instrumented.
        screensaver::tick(lv_display_get_inactive_time(nullptr));
    }

#if defined(__APPLE__) && defined(__MACH__)
    lv_timer_handler();
#endif
}

bool app::IsDone()
{
    return false;
}

void app::Destroy()
{
    DestroyMooncake();
    hal::Destroy();
}
