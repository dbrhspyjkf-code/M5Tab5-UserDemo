#include "xiaozhi_ctl.h"

#include "application.h"
#include "boards/common/board.h"
#include "tab5_bridge_lcd.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

static const char* TAG = "xiaozhi_ctl";
static TaskHandle_t s_task = nullptr;

// Set true once app.Initialize() has returned, i.e. the event group + audio
// service exist. Scheduling work (suspend/resume) before this would crash.
static volatile bool s_initialized = false;
// True while the audio pipeline is running; false after a suspend.
static volatile bool s_running = false;

// Shared battery state updated by the main app via xiaozhi_set_battery_percent().
static int s_battery_percent = -1;

static void xiaozhi_task_fn(void*)
{
    auto& app = Application::GetInstance();
    app.Initialize();
    s_initialized = true;
    s_running = true;
    app.Run();
    vTaskDelete(nullptr);
}

extern "C" void xiaozhi_start_task(void)
{
    if (s_task != nullptr) return;
    ESP_LOGI(TAG, "starting xiaozhi task");
    // 32 KB stack: TLS/websocket + LVGL UI setup + esp_srmodel_init (SPIFFS mount).
    xTaskCreate(xiaozhi_task_fn, "xiaozhi", 32768, nullptr, 5, &s_task);
}

extern "C" void xiaozhi_suspend(void)
{
    if (!s_initialized || !s_running) return;
    s_running = false;
    ESP_LOGI(TAG, "suspend: stopping audio service + closing protocol");
    auto& app = Application::GetInstance();
    // Mirror the OTA-failure path (Application::UpgradeFirmware): just stop the
    // audio service (frees the input/output/opus task stacks + stops the mic/AFE),
    // keeping the protocol ALIVE. Do NOT ResetProtocol(): with protocol_ == null,
    // HandleWakeWordDetectedEvent()/ToggleChat() early-return, so a reset would
    // make xiaozhi unable to wake or talk after resume. Runs on the xiaozhi main
    // task so it is serialized with the event loop.
    app.Schedule([&app]() {
        app.AbortSpeaking(kAbortReasonNone);
        app.GetAudioService().Stop();   // stop input/output/opus tasks, clear queues
    });
}

extern "C" void xiaozhi_resume(void)
{
    if (!s_initialized || s_running) return;
    s_running = true;
    ESP_LOGI(TAG, "resume: restarting audio service");
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        auto& as = app.GetAudioService();
        as.Start();                      // recreate input/output/opus tasks
        as.EnableWakeWordDetection(true);// start listening for "你好小智" again
        app.SetDeviceState(kDeviceStateIdle);
    });
}

extern "C" void xiaozhi_activate_screen(void)
{
    auto* display = static_cast<Tab5BridgeLcdDisplay*>(
        Board::GetInstance().GetDisplay()
    );
    if (display) {
        display->ActivateScreen();
    }
}

extern "C" void xiaozhi_set_battery_percent(int percent)
{
    s_battery_percent = percent;
}

extern "C" int xiaozhi_get_battery_percent(void)
{
    return s_battery_percent;
}
