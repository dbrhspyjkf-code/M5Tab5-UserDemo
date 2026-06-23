#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

#ifndef PLATFORM_BUILD_DESKTOP
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>
#endif

class AppStocks : public mooncake::AppAbility {
public:
    AppStocks();
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    struct StockItem {
        std::string code;
        std::string name;
        float price   {0};
        float chg     {0};   // 涨跌幅 %
        float pchg    {0};   // 涨跌额
        float turnover{0};
        float liangbi {0};
    };

    // ── Layout ─────────────────────────────────────────────────────────
    static constexpr int SCREEN_W    = 1280;
    static constexpr int SCREEN_H    = 720;
    static constexpr int HEADER_H    = 72;
    static constexpr int STATUS_H    = 60;
    static constexpr int ROW_H       = 52;
    static constexpr int N_ROWS      = 10;

    // ── Color palette (must match app_home / app_ha) ──────────────────
    static constexpr uint32_t C_BG       = 0x081522;
    static constexpr uint32_t C_HEADER   = 0x0D1F35;
    static constexpr uint32_t C_TEXT     = 0xEAF3FB;
    static constexpr uint32_t C_DIM      = 0x8AA0B5;
    static constexpr uint32_t C_UP       = 0x2ECC71;
    static constexpr uint32_t C_DOWN     = 0xE74C3C;
    static constexpr uint32_t C_FLAT     = 0x95A5A6;
    static constexpr uint32_t C_ACCENT   = 0x4FA3FF;

    // ── LED ────────────────────────────────────────────────────────────
    static constexpr int LED_W = 40;
    static constexpr int LED_H = 8;
    static constexpr int LED_PANEL = 8;
    static constexpr int LED_N = LED_W * LED_H;  // 5 panels * 64
    static constexpr int LED_DIN_GPIO = 6;       // Grove PORT A signal (same as unit_puzzle)
    static constexpr int CHAR_W = 5;
    static constexpr int CHAR_H = 7;
    static constexpr int CHAR_GAP = 1;
    static constexpr int CELL = CHAR_W + CHAR_GAP;       // 6 px per char
    static constexpr int SCROLL_MS_PER_PX = 150;          // match app_email_led
    static constexpr int STOCK_SEG1_MS = 2500;            // code+CHG segment
    static constexpr int STOCK_SEG2_MS = 2000;            // code+price segment
    static constexpr int STOCK_PAUSE_MS = 1000;           // black gap

    // ── State ──────────────────────────────────────────────────────────
    std::function<void()> _close_cb;
    std::vector<StockItem> _items;
    std::mutex             _items_mu;
    bool                   _fetch_error {false};
    std::string            _fetch_error_msg;
    int64_t                _last_fetch_ms {0};
    static constexpr int   FETCH_INTERVAL_MS = 30 * 1000;
    static constexpr int   FETCH_TIMEOUT_MS  = 5 * 1000;

    // ── LVGL handles ───────────────────────────────────────────────────
    lv_obj_t* _scr         {nullptr};
    lv_obj_t* _header      {nullptr};
    lv_obj_t* _status_bar  {nullptr};
    lv_obj_t* _status_dot  {nullptr};
    lv_obj_t* _status_text {nullptr};
    lv_obj_t* _refresh_btn {nullptr};
    lv_obj_t* _col_header  {nullptr};
    lv_obj_t* _rows[N_ROWS] {nullptr};
    lv_obj_t* _detail_modal{nullptr};
    lv_timer_t* _poll_timer{nullptr};

    // ── LED handles (ESP only) ─────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP
    led_strip_handle_t  _strip         {nullptr};
    std::mutex          _strip_mu;
    TaskHandle_t        _ticker_task   {nullptr};
    std::atomic<bool>   _ticker_running{false};
    std::atomic<bool>   _ticker_done   {false};
    int                 _ticker_index  {0};
    int64_t             _ticker_seg_start_ms {0};
    int                 _ticker_seg_state {0};  // 0=seg1_scroll, 1=seg2_scroll
#endif

    // ── Methods (filled in later tasks) ────────────────────────────────
    void _buildUi();
    void _destroyUi();
    void _setCellText(lv_obj_t* row, int col, const char* text, uint32_t color);
    void _setStatus(bool online, const char* text);
    void _showDetail(int row);
    void _closeDetail();
    void _fetchStocksAsync();
    void _doFetch();
    void _parseStocksJson(const std::string& body);
    void _refreshUiFromItems();

#ifndef PLATFORM_BUILD_DESKTOP
    esp_err_t _stripInit();
    void      _stripDeinit();
    void      _setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    int       _xy2i(int x, int y);
    void      _startTicker();
    void      _stopTicker();
    static void _tickerTask(void* arg);
    void      _renderTickerFrame(int64_t now_ms);
    void      _renderStockSegment(const char* text, int text_len,
                                  int64_t now_ms, uint8_t r, uint8_t g, uint8_t b);
    uint32_t  _chgColor(float chg);
#endif
};
