/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include "hal/components/hal_tab5_keyboard.h"
#include <app.h>
#include <hal/hal.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void app_main(void)
{
    // 应用层初始化回调
    app::InitCallback_t callback;

    callback.onHalInjection = []() {
        // 注入桌面平台的硬件抽象
        hal::Inject(std::make_unique<HalEsp32>());
    };

    // 应用层启动
    app::Init(callback);
    // Bring up the M5Stack Tab5 official keyboard (TCA8418 over the
    // external I2C bus) and start forwarding key events into the LVGL
    // keyboard group. No-op if the keyboard isn't attached.
    hal_tab5_keyboard_init();
    while (!app::IsDone()) {
        app::Update();
        vTaskDelay(1);
    }
    app::Destroy();
}
