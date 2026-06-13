#include "xiaozhi_ctl.h"

#include "application.h"
#include "boards/common/board.h"
#include "tab5_bridge_lcd.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

static const char* TAG = "xiaozhi_ctl";
static TaskHandle_t s_task = nullptr;

static void xiaozhi_task_fn(void*)
{
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();
    vTaskDelete(nullptr);
}

extern "C" void xiaozhi_start_task(void)
{
    if (s_task != nullptr) return;
    ESP_LOGI(TAG, "starting xiaozhi task");
    // 16 KB stack: online mode does TLS/websocket + SetupUI builds the LVGL UI.
    xTaskCreate(xiaozhi_task_fn, "xiaozhi", 16384, nullptr, 5, &s_task);
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
