#include "app_stocks.h"
#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <hal/hal.h>
#include "../app_email_led/app_email_led.h"  // for setPortAOwnedByApp
#endif

static const char* TAG = "stocks";

AppStocks::AppStocks() {}

void AppStocks::onCreate() {
    mclog::tagInfo(TAG, "created");
}

void AppStocks::onOpen() {
    mclog::tagInfo(TAG, "opened");
    // Task 3: UI build
    // Task 4: data fetch
    // Task 5: LED claim
    // Task 6: ticker start
    _buildUi();
}

void AppStocks::onRunning() {
    // Task 4: poll cadence
}

void AppStocks::onClose() {
    mclog::tagInfo(TAG, "closed");
    // Task 6: ticker stop
    // Task 5: LED release
    _destroyUi();
}

// All other methods are implemented in later tasks.
// Empty stubs here so the file compiles after this task:
void AppStocks::_buildUi() {}
void AppStocks::_destroyUi() {}
void AppStocks::_setCellText(lv_obj_t*, int, const char*, uint32_t) {}
void AppStocks::_setStatus(bool, const char*) {}
void AppStocks::_showDetail(int) {}
void AppStocks::_closeDetail() {}
void AppStocks::_fetchStocksAsync() {}
void AppStocks::_doFetch() {}
void AppStocks::_parseStocksJson(const std::string&) {}
void AppStocks::_refreshUiFromItems() {}

#ifndef PLATFORM_BUILD_DESKTOP
esp_err_t AppStocks::_stripInit() { return ESP_OK; }
void AppStocks::_stripDeinit() {}
void AppStocks::_setPixelXY(int, int, uint8_t, uint8_t, uint8_t) {}
int AppStocks::_xy2i(int x, int y) {
    int panel = x / LED_PANEL;
    int lx    = x % LED_PANEL;
    return panel * (LED_PANEL * LED_PANEL) + lx * LED_PANEL + (LED_PANEL - 1 - y);
}
void AppStocks::_startTicker() {}
void AppStocks::_stopTicker() {}
void AppStocks::_tickerTask(void*) {}
void AppStocks::_renderTickerFrame(int64_t) {}
void AppStocks::_renderStockSegment(const char*, int, int64_t, uint8_t, uint8_t, uint8_t) {}
uint32_t AppStocks::_chgColor(float chg) {
    if (chg > 0.01f)  return C_UP;
    if (chg < -0.01f) return C_DOWN;
    return C_FLAT;
}
#endif
