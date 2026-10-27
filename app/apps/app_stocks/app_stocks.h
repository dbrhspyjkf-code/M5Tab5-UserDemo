#pragma once
#include "../utils/theme/theme.h"
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <memory>

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
        std::string one_sentence;
        std::string analysis_summary;
        std::string analysis_date;
    };

    // ── Layout ─────────────────────────────────────────────────────────
    static constexpr int SCREEN_W    = 1280;
    static constexpr int SCREEN_H    = 720;
    // ── 指数看板页 (2026-10-27, 与自选股同风格, 左右拨切换) ──
    struct IndexDef { const char* name; const char* tencent; };  // tencent = qt.gtimg 代码
    static constexpr IndexDef kIndices[9] = {
        {"上证指数", "sh000001"}, {"深证成指", "sz399001"}, {"创业板指", "sz399006"},
        {"科创综指", "sh000688"}, {"沪深300",  "sh000300"}, {"恒生指数", "r_hkHSI"},
        {"道琼斯",   "usDJI"},    {"纳斯达克", "usIXIC"},   {"标普500",  "usINX"},
    };
    struct IndexQuote {
        bool ok = false;
        float price{0}, chg_amt{0}, chg_pct{0};
    };
    IndexQuote _index_quotes[9] = {};      // guarded by _dash…复用 _chart_mu? 用独立 _index_mu
    std::mutex _index_mu;
    std::atomic<bool> _index_ready{false};
    lv_obj_t* _index_cards[9]   {nullptr};
    lv_obj_t* _index_name[9]    {nullptr};
    lv_obj_t* _index_price[9]   {nullptr};
    lv_obj_t* _index_amt[9]     {nullptr};
    lv_obj_t* _index_pct[9]     {nullptr};
    lv_obj_t* _index_page       {nullptr};  // 指数页容器
    lv_obj_t* _stocks_page      {nullptr};  // 自选股页容器
    int        _page            = 0;        // 0=自选股 1=指数
    uint32_t   _last_index_ms   = 0;
    void _buildIndexPage();
    void _fetchIndicesAsync();
    void _applyIndices();
    void _indexFetchWorker();
    void _setPage(int page);

    // 卡片网格 4 列 (2026-10-27, 参考工具页卡片风格); 行数随持仓数动态
    // 扩展, 纵向滚动容器承载. 超过一屏的持仓上下拨滚动查看 (2026-10-27
    // 用户指定); 卡位按持仓数动态创建, 上限 40 (MX 列表现实规模).
    static constexpr int GRID_COLS   = 4;
    static constexpr int MAX_CARDS   = 40;
    static constexpr int CARD_W      = 294;
    static constexpr int CARD_H      = 213;
    static constexpr int GRID_X0     = 28;   // (1280-4*294-3*16)/2
    static constexpr int GRID_GAP    = 16;

    // ── Color palette (must match app_home / app_ha) ──────────────────
    // 配色: 实时读全局主题 (utils/theme/theme.h), 函数形式便于调用点零快照
    static uint32_t C_BG()       { return 0x000000; }
    static uint32_t C_HEADER()   { return theme::cur().card[1]; }
    static uint32_t C_TEXT()     { return theme::cur().text_hi; }
    static uint32_t C_DIM()      { return theme::cur().text_dim; }
    // 中国 A 股惯例: 涨=红, 跌=绿 (与西方相反). LCD 表格 + LED ticker 共用.
    static uint32_t C_UP()       { return theme::cur().danger; }  // 涨 → 红
    static uint32_t C_DOWN()     { return theme::cur().ok; }      // 跌 → 绿
    static uint32_t C_FLAT()     { return theme::cur().text_dim; }  // 平 → 灰

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
    // 滚动速度: 每段时长 = (文字宽+40) * 此值, 滚完整段再切. 越大越慢. 50ms/px.
    static constexpr int SCROLL_MS_PER_PX = 50;
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
    // 同一时间只允许一个 HTTP 请求，避免刷新边界上重复创建 worker。
    std::atomic<bool>      _fetch_in_flight {false};
    static constexpr int   FETCH_INTERVAL_MS = 5 * 1000;   // 2026-10-27 用户指定 60s→5s (仅交易时段生效)
    static constexpr int   FETCH_TIMEOUT_MS  = 5 * 1000;

    // ── LVGL handles ───────────────────────────────────────────────────
    lv_obj_t* _scr         {nullptr};
    std::vector<lv_obj_t*> _cards;      // 卡位随持仓数动态创建
    std::vector<lv_obj_t*> _card_name;
    std::vector<lv_obj_t*> _card_price;
    std::vector<lv_obj_t*> _card_chg;
    std::vector<lv_obj_t*> _card_amt;   // 涨跌额 (2026-10-27 用户加)
    lv_obj_t* _stocks_scroll {nullptr};  // 纵向滚动容器 (承载卡片网格)
    int         _n_cards     = 0;        // 当前实际卡位数
    lv_obj_t* _detail_modal{nullptr};

    // ── 分时图 / K线图 (腾讯行情接口, 2026-10-27) ──
    struct ChartData {
        bool ok = false;
        std::vector<float> minute_prices;    // 分时: 全天分钟价, [0]=昨收基准
        std::vector<float> day_open, day_close, day_high, day_low;  // 日K: 最近 N=60 根
        std::vector<std::string> day_date;
    };
    ChartData      _chart;                   // guarded by _chart_mu
    std::mutex     _chart_mu;
    std::atomic<bool> _chart_in_flight{false};
    lv_obj_t*      _chart_canvas  {nullptr};  // 944x420 ARGB8888
    lv_obj_t*      _chart_tab     {nullptr};  // 当前页 ("分时"/"日K")
    lv_obj_t*      _chart_status  {nullptr};
    void*          _chart_pixels  {nullptr};  // canvas 像素堆缓冲 (chart 绘制专用)
    std::atomic<bool> _chart_ready{false}; // worker 完成 → onRunning 消费
    std::string    _chart_code;             // 当前弹窗的股票代码

    lv_timer_t* _poll_timer{nullptr};

    // Swipe-up-to-exit gesture, registered on the (persistent, global) pointer
    // indev instead of a back button. Guards against a stale callback firing
    // after teardown with an alive flag rather than relying on
    // lv_indev_remove_event_cb_with_user_data actually winning the race —
    // see app_ha/view/view.cpp for the crash this pattern was hardened against.
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev{nullptr};
    void*       _gesture_ctx{nullptr};  // opaque SwipeCtx*, defined in app_stocks.cpp

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
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _showDetail(int row);
    void _closeDetail();
    void _applyCharts();   // worker 完成 → UI 线程绘制 (onRunning 检查 _chart_ready)
    void _chartFetchWorker(std::string code, std::shared_ptr<bool> alive);
    void _fetchStocksAsync();
    void _doFetch();
    void _parseStocksJson(const std::string& body);
    void _refreshUiFromItems();
    void _ensureCards(int n);   // 按需创建卡位到滚动容器
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
