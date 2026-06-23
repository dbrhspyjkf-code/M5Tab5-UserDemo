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
    // 中国 A 股惯例: 涨=红, 跌=绿 (与西方相反). LCD 表格 + LED ticker 共用.
    static constexpr uint32_t C_UP       = 0xE74C3C;  // 涨 → 红
    static constexpr uint32_t C_DOWN     = 0x2ECC71;  // 跌 → 绿
    static constexpr uint32_t C_FLAT     = 0x95A5A6;  // 平 → 灰
    static constexpr uint32_t C_ACCENT   = 0x4FA3FF;

    // ── LED ────────────────────────────────────────────────────────────
    static constexpr int LED_W = 40;
    static constexpr int LED_H = 8;
    static constexpr int LED_PANEL = 8;
    static constexpr int LED_N = LED_W * LED_H;  // 5 panels * 64 = 320
    // PORT A Signal 真身 = GPIO53 (与「灯阵」/「邮件 LED」完全一致, 见 app_unit_puzzle.h).
    static constexpr int LED_DIN_GPIO = 53;      // Grove PORT A signal
    static constexpr int CHAR_W = 5;
    static constexpr int CHAR_H = 7;
    static constexpr int CHAR_GAP = 1;
    static constexpr int CELL = CHAR_W + CHAR_GAP;       // 6 px per char
    // 滚动速度: 每段时长 = (文字宽+40) * 此值, 滚完整段再切. 40ms/px ≈ 5s 滚完一只.
    static constexpr int SCROLL_MS_PER_PX = 40;
    static constexpr int STOCK_PAUSE_MS = 1000;           // 两段之间黑屏间隔

    // ── State ──────────────────────────────────────────────────────────
    std::function<void()> _close_cb;
    std::vector<StockItem> _items;
    std::mutex             _items_mu;
    bool                   _fetch_error {false};
    std::string            _fetch_error_msg;
    int64_t                _last_fetch_ms {0};
    // app 是否在前台. 退出时先置 false, 防止在途的 fetch worker 完成后
    // 又去 _startTicker() 抢回已交还给邮件通知器的 PORT A.
    std::atomic<bool>      _app_open {false};
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
    uint8_t             _brightness    {48};   // 全局亮度 (out of 256) ~19%, 与邮件 LED 一致
    TaskHandle_t        _ticker_task   {nullptr};
    std::atomic<bool>   _ticker_running{false};
    std::atomic<bool>   _ticker_done   {false};
    int                 _ticker_index  {0};
    int64_t             _ticker_seg_start_ms {0};
    int64_t             _ticker_seg_dur_ms {0};  // 当前段滚完一遍的时长 (按文字宽算)
    int                 _ticker_seg_state {0};  // 0=代码+涨跌幅, 1=代码+现价
#endif

    // ── Methods ─────────────────────────────────────────────────────────
    // 退出唯一出口 (← 返回按钮). 注意: 灯阵/LoRa 同款坑 —— 从工具页 openApp
    // 进来, 返回也走 openApp(工具页), mooncake 不会关闭当前 app, 所以 onClose
    // 不一定触发. 因此停 ticker / 释放 LED / 把 PORT A 交还邮件通知器的收尾
    // 必须在这里做, 否则 setPortAOwnedByApp(false) 永不复位, 邮件 LED 永久失灵.
    void _requestClose();
    void _teardownHardware();  // 停 ticker + 释放 strip + 交还 PORT A (幂等)
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
    // 涨绿 / 跌红 / 平灰 — UI 详情卡 + LED ticker 共用, 两平台都要.
    uint32_t _chgColor(float chg);

#ifndef PLATFORM_BUILD_DESKTOP
    char _ticker_buf[32] = "";  // ticker 当前段文本缓冲

    esp_err_t _stripInit();
    void      _stripDeinit();
    void      _setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b);
    int       _xy2i(int x, int y);
    void      _startTicker();
    void      _stopTicker();
    static void _tickerTask(void* arg);
    void      _renderTickerFrame(int64_t now_ms);
#endif
};
