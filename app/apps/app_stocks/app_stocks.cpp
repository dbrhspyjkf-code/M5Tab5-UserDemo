#include "app_stocks.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <chrono>

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_log.h>
#include <esp_timer.h>
#include "../app_email_led/app_email_led.h"     // setPortAOwnedByApp (PORT A 仲裁)
#include "../app_unit_puzzle/font5x7.h"          // 共享 5x7 点阵字体
#endif

static const char* TAG = "stocks";

// ── 全覆盖中文字体 (与 app_ha/view, app_settings 一致) ────────────────────────
//   zh_font_lg() 30px (标题), zh_font_sm() 20px (表头/单元格/按钮).
//   设备上是 flash 里的 cbin blob 原地用; 桌面回退到链接进来的 20px C 数组字体.
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
extern const uint8_t font_puhui_common_20_4_bin_start[]
    asm("_binary_font_puhui_common_20_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_sm()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_20_4_bin_start);
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif

// 单调毫秒时钟 (两平台通用)
static int64_t mono_ms()
{
#ifdef PLATFORM_BUILD_DESKTOP
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    return (int64_t)(esp_timer_get_time() / 1000);
#endif
}

// 7 列的水平像素布局 (1280 宽屏). 代码/名称/现价/涨跌幅/涨跌额/换手率/量比.
static const char* COLS[]  = {"代码", "名称", "现价", "涨跌幅", "涨跌额", "换手率", "量比"};
static const int   COL_X[] = {30, 150, 370, 510, 670, 830, 970};

// ════════════════════════════════════════════════════════════════════════════
//  生命周期
// ════════════════════════════════════════════════════════════════════════════
AppStocks::AppStocks()
{
    setAppInfo().name = "自选股";
}

void AppStocks::onCreate()
{
    mclog::tagInfo(TAG, "created");
}

void AppStocks::onOpen()
{
    mclog::tagInfo(TAG, "opened");
    if (_scr) _destroyUi();  // 幂等: 清掉上次遗留的 UI (后台收尾后重开)
    _app_open.store(true);
#ifndef PLATFORM_BUILD_DESKTOP
    // 独占 PORT A: 先让后台邮件通知器释放 RMT/GPIO53, 再由本 app 接管.
    AppEmailLed::setPortAOwnedByApp(true);
    if (_stripInit() != ESP_OK) {
        mclog::tagWarn(TAG, "led init failed, app continues without ticker");
    }
#endif
    _buildUi();
    _fetchStocksAsync();  // 首次拉取
}

void AppStocks::onRunning()
{
    // mooncake 给每个已装 app 每轮都调 onRunning, 且 openApp(其他) 不会把本 app
    // 切到 GoClose → onClose 永不触发. 所以这里自己判断是否还在前台 (LVGL 活动屏
    // 是不是本 app 的 _scr). 一旦被切到后台 (上拨返回 / 唤醒词 / 屏保 等任何方式),
    // 就停 ticker + 交还 PORT A, 否则 LED 会一直放股票. 重新打开时 onOpen 会重建.
    bool foreground = (_scr != nullptr && lv_screen_active() == _scr);
    if (!foreground) {
        if (_app_open.load()) {   // 刚从前台被切走 → 收尾一次
            mclog::tagInfo(TAG, "lost foreground, releasing LED");
            _teardownHardware();
            _destroyUi();
        }
        return;
    }

    // 前台: 30s 轮询. 首次拉取已在 onOpen 触发, 这里只管周期刷新.
    int64_t last;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        last = _last_fetch_ms;
    }
    if (last != 0 && (mono_ms() - last) > FETCH_INTERVAL_MS) {
        _fetchStocksAsync();
    }
}

void AppStocks::onClose()
{
    // 安全网: 即便 onClose 没被 mooncake 触发 (见 _requestClose 注释), 这里也做一遍.
    mclog::tagInfo(TAG, "closed");
    _teardownHardware();
    _destroyUi();
}

// 退出唯一出口. mooncake openApp(工具页) 不会关闭当前 app, onClose 不一定触发,
// 所以收尾 (停 ticker / 释放 strip / 交还 PORT A) 必须在这里显式做.
void AppStocks::_requestClose()
{
    _teardownHardware();
    _destroyUi();
    if (_close_cb) _close_cb();
}

void AppStocks::_teardownHardware()
{
    _app_open.store(false);  // 先关门: 在途 worker 完成后不会再 _startTicker
#ifndef PLATFORM_BUILD_DESKTOP
    _stopTicker();
    _stripDeinit();
    AppEmailLed::setPortAOwnedByApp(false);  // 交还给后台邮件通知器
#endif
}

// ════════════════════════════════════════════════════════════════════════════
//  涨跌配色 (UI 详情卡 + LED ticker 共用)
// ════════════════════════════════════════════════════════════════════════════
uint32_t AppStocks::_chgColor(float chg)
{
    if (chg > 0.01f)  return C_UP;
    if (chg < -0.01f) return C_DOWN;
    return C_FLAT;
}

// ════════════════════════════════════════════════════════════════════════════
//  UI
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_buildUi()
{
    LvglLockGuard lock;

    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ────────────────────────────────────────────────────────
    _header = lv_obj_create(_scr);
    lv_obj_set_size(_header, SCREEN_W, HEADER_H);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);
    lv_obj_clear_flag(_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_label_create(_header);
    lv_label_set_text(back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(back, lv_color_hex(C_TEXT), 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    // 点击在 LVGL 线程上, _requestClose 会删 _scr (back 的祖先), 同步删会 UAF.
    // 用 lv_async_call 延后到事件处理完再关.
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
        lv_async_call([](void* u) { static_cast<AppStocks*>(u)->_requestClose(); }, self);
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* title = lv_label_create(_header);
    lv_label_set_text(title, "自选股");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 84, 0);

    _status_text = lv_label_create(_header);
    lv_label_set_text(_status_text, "● 加载中...");
    lv_obj_set_style_text_font(_status_text, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_status_text, lv_color_hex(C_DIM), 0);
    lv_obj_align(_status_text, LV_ALIGN_RIGHT_MID, -24, 0);

    // ── Column header ─────────────────────────────────────────────────
    _col_header = lv_obj_create(_scr);
    lv_obj_set_size(_col_header, SCREEN_W, 36);
    lv_obj_align(_col_header, LV_ALIGN_TOP_MID, 0, HEADER_H + 4);
    lv_obj_set_style_bg_color(_col_header, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_bg_opa(_col_header, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_col_header, 0, 0);
    lv_obj_set_style_radius(_col_header, 0, 0);
    lv_obj_set_style_pad_all(_col_header, 0, 0);
    lv_obj_clear_flag(_col_header, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 7; i++) {
        lv_obj_t* lbl = lv_label_create(_col_header);
        lv_label_set_text(lbl, COLS[i]);
        lv_obj_set_style_text_font(lbl, zh_font_sm(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, COL_X[i], 0);
    }

    // ── Data rows ─────────────────────────────────────────────────────
    const int rows_top = HEADER_H + 4 + 36 + 4;
    for (int i = 0; i < N_ROWS; i++) {
        _rows[i] = lv_obj_create(_scr);
        lv_obj_set_size(_rows[i], SCREEN_W, ROW_H);
        lv_obj_align(_rows[i], LV_ALIGN_TOP_MID, 0, rows_top + i * ROW_H);
        lv_obj_set_style_bg_color(_rows[i], lv_color_hex(C_HEADER), 0);
        lv_obj_set_style_bg_opa(_rows[i], i % 2 ? LV_OPA_30 : LV_OPA_10, 0);
        lv_obj_set_style_border_width(_rows[i], 0, 0);
        lv_obj_set_style_radius(_rows[i], 0, 0);
        lv_obj_set_style_pad_all(_rows[i], 0, 0);
        lv_obj_clear_flag(_rows[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(_rows[i], (void*)(intptr_t)i);
        lv_obj_add_event_cb(_rows[i], [](lv_event_t* e) {
            auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
            int idx = (int)(intptr_t)lv_obj_get_user_data(
                          lv_event_get_current_target_obj(e));
            self->_showDetail(idx);
        }, LV_EVENT_CLICKED, this);

        for (int c = 0; c < 7; c++) {
            lv_obj_t* cell = lv_label_create(_rows[i]);
            lv_label_set_text(cell, "--");
            lv_obj_set_style_text_font(cell, zh_font_sm(), 0);
            lv_obj_set_style_text_color(cell, lv_color_hex(C_DIM), 0);
            lv_obj_align(cell, LV_ALIGN_LEFT_MID, COL_X[c], 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);  // 点击冒泡到 row
        }
    }

    // ── Status bar ─────────────────────────────────────────────────────
    _status_bar = lv_obj_create(_scr);
    lv_obj_set_size(_status_bar, SCREEN_W, STATUS_H);
    lv_obj_align(_status_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_status_bar, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(_status_bar, 0, 0);
    lv_obj_set_style_radius(_status_bar, 0, 0);
    lv_obj_set_style_pad_all(_status_bar, 0, 0);
    lv_obj_clear_flag(_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    _status_dot = lv_label_create(_status_bar);
    lv_label_set_text(_status_dot, "LED 滚动播报中");
    lv_obj_set_style_text_font(_status_dot, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_status_dot, lv_color_hex(C_DIM), 0);
    lv_obj_align(_status_dot, LV_ALIGN_LEFT_MID, 24, 0);

    _refresh_btn = lv_button_create(_status_bar);
    lv_obj_set_size(_refresh_btn, 140, 40);
    lv_obj_align(_refresh_btn, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_set_style_bg_color(_refresh_btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(_refresh_btn, 10, 0);
    lv_obj_t* rlbl = lv_label_create(_refresh_btn);
    lv_label_set_text(rlbl, "刷新");
    lv_obj_set_style_text_font(rlbl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(rlbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(rlbl);
    lv_obj_add_event_cb(_refresh_btn, [](lv_event_t* e) {
        static_cast<AppStocks*>(lv_event_get_user_data(e))->_fetchStocksAsync();
    }, LV_EVENT_CLICKED, this);

    lv_screen_load(_scr);
}

void AppStocks::_destroyUi()
{
    LvglLockGuard lock;
    _closeDetail();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    for (auto& r : _rows) r = nullptr;
    _header = _status_bar = _status_dot = _status_text = nullptr;
    _refresh_btn = _col_header = nullptr;
    if (_poll_timer) { lv_timer_delete(_poll_timer); _poll_timer = nullptr; }
}

void AppStocks::_setCellText(lv_obj_t* row, int col, const char* text, uint32_t color)
{
    if (!row) return;
    uint32_t child_cnt = lv_obj_get_child_cnt(row);
    if ((uint32_t)col >= child_cnt) return;
    lv_obj_t* cell = lv_obj_get_child(row, col);
    if (!cell) return;
    lv_label_set_text(cell, text);
    lv_obj_set_style_text_color(cell, lv_color_hex(color), 0);
}

void AppStocks::_setStatus(bool online, const char* text)
{
    if (!_status_text) return;
    std::string s = online ? "● 在线" : "● 离线";
    if (text) { s += "  "; s += text; }
    lv_label_set_text(_status_text, s.c_str());
    lv_obj_set_style_text_color(_status_text,
        lv_color_hex(online ? C_UP : C_DOWN), 0);
}

void AppStocks::_showDetail(int row_idx)
{
    StockItem s;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        if (row_idx < 0 || row_idx >= (int)_items.size()) return;
        s = _items[row_idx];
    }
    LvglLockGuard lock;
    if (!_scr) return;
    _closeDetail();

    _detail_modal = lv_obj_create(_scr);
    lv_obj_set_size(_detail_modal, 600, 400);
    lv_obj_center(_detail_modal);
    lv_obj_set_style_bg_color(_detail_modal, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_color(_detail_modal, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(_detail_modal, 2, 0);
    lv_obj_set_style_radius(_detail_modal, 16, 0);
    lv_obj_set_style_pad_all(_detail_modal, 24, 0);
    lv_obj_clear_flag(_detail_modal, LV_OBJ_FLAG_SCROLLABLE);

    int y = 0;
    auto add_line = [&](const char* label, const std::string& val, uint32_t color) {
        lv_obj_t* l = lv_label_create(_detail_modal);
        lv_label_set_text_fmt(l, "%s   %s", label, val.c_str());
        lv_obj_set_style_text_font(l, zh_font_sm(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
        y += 40;
    };
    char buf[32];
    add_line("代码", s.code, C_TEXT);
    add_line("名称", s.name, C_TEXT);
    snprintf(buf, sizeof(buf), "%.2f", s.price);   add_line("现价", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%+.2f%%", s.chg);  add_line("涨跌幅", buf, _chgColor(s.chg));
    snprintf(buf, sizeof(buf), "%+.2f", s.pchg);   add_line("涨跌额", buf, _chgColor(s.chg));
    snprintf(buf, sizeof(buf), "%.2f%%", s.turnover); add_line("换手率", buf, C_DIM);
    snprintf(buf, sizeof(buf), "%.2f", s.liangbi);    add_line("量比", buf, C_DIM);

    lv_obj_t* close_btn = lv_button_create(_detail_modal);
    lv_obj_set_size(close_btn, 120, 48);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_t* cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "关闭");
    lv_obj_set_style_text_font(cl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        static_cast<AppStocks*>(lv_event_get_user_data(e))->_closeDetail();
    }, LV_EVENT_CLICKED, this);
}

void AppStocks::_closeDetail()
{
    if (_detail_modal) {
        lv_obj_delete(_detail_modal);
        _detail_modal = nullptr;
    }
}

void AppStocks::_refreshUiFromItems()
{
    LvglLockGuard lock;
    if (!_scr) return;
    std::vector<StockItem> snapshot;
    bool err;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        snapshot = _items;
        err = _fetch_error;
    }
    for (int i = 0; i < N_ROWS; i++) {
        if (i < (int)snapshot.size()) {
            const auto& s = snapshot[i];
            char buf[32];
            _setCellText(_rows[i], 0, s.code.c_str(), C_TEXT);
            _setCellText(_rows[i], 1, s.name.c_str(), C_TEXT);
            snprintf(buf, sizeof(buf), "%.2f", s.price);
            _setCellText(_rows[i], 2, buf, C_TEXT);
            snprintf(buf, sizeof(buf), "%+.2f%%", s.chg);
            _setCellText(_rows[i], 3, buf, _chgColor(s.chg));
            snprintf(buf, sizeof(buf), "%+.2f", s.pchg);
            _setCellText(_rows[i], 4, buf, _chgColor(s.chg));
            snprintf(buf, sizeof(buf), "%.2f%%", s.turnover);
            _setCellText(_rows[i], 5, buf, C_DIM);
            snprintf(buf, sizeof(buf), "%.2f", s.liangbi);
            _setCellText(_rows[i], 6, buf, C_DIM);
        } else {
            for (int c = 0; c < 7; c++) _setCellText(_rows[i], c, "--", C_DIM);
        }
    }
    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%u 只", (unsigned)snapshot.size());
    _setStatus(!err, count_buf);
}

// ════════════════════════════════════════════════════════════════════════════
//  数据拉取 (hermes :8766/api/stocks/portfolio, mx-selfselect 后端)
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_fetchStocksAsync()
{
    // app 实例随 mooncake 常驻 (从不 uninstall), 捕获 this 安全.
    if (GetHAL()->tryRunDetached([this]() { _doFetch(); })) {
        mclog::tagInfo(TAG, "fetch scheduled");
    } else {
        mclog::tagWarn(TAG, "fetch worker spawn failed, will retry next tick");
    }
}

void AppStocks::_doFetch()
{
    auto* hal = GetHAL();
    std::string svc_host = hal->getConfig("svc_host", "192.168.1.142");
    if (svc_host.empty()) svc_host = "192.168.1.142";
    std::string url = "http://" + svc_host + ":8766/api/stocks/portfolio";

    auto resp = hal->httpGet(url);
    if (!resp.ok || resp.status != 200) {
        {
            std::lock_guard<std::mutex> lk(_items_mu);
            _fetch_error = true;
            _fetch_error_msg = "HTTP " + std::to_string(resp.status);
            _last_fetch_ms = mono_ms();  // 失败也记时间, 避免每 tick 狂重试
        }
        mclog::tagWarn(TAG, "fetch failed: {}", _fetch_error_msg);
        _refreshUiFromItems();
        return;
    }
    _parseStocksJson(resp.body);
}

void AppStocks::_parseStocksJson(const std::string& body)
{
    try {
        auto j = nlohmann::json::parse(body);
        std::vector<StockItem> parsed;
        if (j.contains("items") && j["items"].is_array()) {
            for (const auto& it : j["items"]) {
                StockItem s;
                s.code     = it.value("code", std::string());
                s.name     = it.value("name", std::string());
                s.price    = it.value("price", 0.0f);
                s.chg      = it.value("chg", 0.0f);
                s.pchg     = it.value("pchg", 0.0f);
                s.turnover = it.value("turnover", 0.0f);
                s.liangbi  = it.value("liangbi", 0.0f);
                if (!s.code.empty()) parsed.push_back(std::move(s));
            }
        }
        size_t n;
        {
            std::lock_guard<std::mutex> lk(_items_mu);
            _items = std::move(parsed);
            _fetch_error = false;
            _last_fetch_ms = mono_ms();
            n = _items.size();
        }
        mclog::tagInfo(TAG, "fetched {} items", n);
        _refreshUiFromItems();
#ifndef PLATFORM_BUILD_DESKTOP
        if (n > 0 && _app_open.load()) _startTicker();
#endif
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lk(_items_mu);
            _fetch_error = true;
            _fetch_error_msg = e.what();
            _last_fetch_ms = mono_ms();
        }
        mclog::tagWarn(TAG, "parse failed: {}", e.what());
        _refreshUiFromItems();
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  LED strip (ESP-only) — 5 块 8x8 串成 40x8, 每块物理 90° 顺时针补偿
// ════════════════════════════════════════════════════════════════════════════
#ifndef PLATFORM_BUILD_DESKTOP
esp_err_t AppStocks::_stripInit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) return ESP_OK;

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num         = LED_DIN_GPIO;
    strip_cfg.max_leds               = LED_N;
    strip_cfg.led_model              = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz     = 10 * 1000 * 1000;
    rmt_cfg.mem_block_symbols = 96;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led_strip init failed (will retry): %s", esp_err_to_name(err));
        _strip = nullptr;
        return err;
    }
    led_strip_clear(_strip);
    return ESP_OK;
}

void AppStocks::_stripDeinit()
{
    std::lock_guard<std::mutex> lk(_strip_mu);
    if (_strip) {
        led_strip_clear(_strip);
        led_strip_refresh(_strip);
        led_strip_del(_strip);
        _strip = nullptr;
    }
}

// 逻辑 (x,y) → 灯带 index. 与「灯阵」app 完全一致: 面板物理逆时针偏转 90°,
// 顺时针补偿 → panel*64 + lx*8 + (7-y).
int AppStocks::_xy2i(int x, int y)
{
    int panel = x / LED_PANEL;
    int lx    = x % LED_PANEL;
    return panel * (LED_PANEL * LED_PANEL) + lx * LED_PANEL + (LED_PANEL - 1 - y);
}

void AppStocks::_setPixelXY(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_strip || x < 0 || x >= LED_W || y < 0 || y >= LED_H) return;
    // 全局亮度缩放 (与灯阵/邮件 LED 一致). 之前漏了, 导致全功率刺眼.
    r = (r * _brightness) >> 8;
    g = (g * _brightness) >> 8;
    b = (b * _brightness) >> 8;
    led_strip_set_pixel(_strip, _xy2i(x, y), r, g, b);
}

// ── ticker 任务 ──────────────────────────────────────────────────────────────
void AppStocks::_startTicker()
{
    if (!_app_open.load()) return;  // app 已退出, 不要再抢 PORT A
    if (_ticker_running.load()) return;
    if (!_strip && _stripInit() != ESP_OK) return;
    _ticker_index = 0;
    _ticker_seg_state = 0;
    _ticker_seg_start_ms = mono_ms();
    _ticker_seg_dur_ms = 0;  // 首段待按文字宽计算
    _ticker_done.store(false);
    _ticker_running.store(true);
    xTaskCreate(_tickerTask, "stocks-led", 8192, this, 1, &_ticker_task);
    mclog::tagInfo(TAG, "ticker started");
}

void AppStocks::_stopTicker()
{
    if (!_ticker_running.load()) return;
    _ticker_running.store(false);
    int waited = 0;
    while (!_ticker_done.load() && waited < 200) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    if (!_ticker_done.load() && _ticker_task) {
        mclog::tagWarn(TAG, "ticker did not stop in 200ms, forcing delete");
        vTaskDelete(_ticker_task);
    }
    _ticker_task = nullptr;
    {
        std::lock_guard<std::mutex> lk(_strip_mu);
        if (_strip) { led_strip_clear(_strip); led_strip_refresh(_strip); }
    }
    mclog::tagInfo(TAG, "ticker stopped");
}

void AppStocks::_tickerTask(void* arg)
{
    auto* self = static_cast<AppStocks*>(arg);
    int64_t last_frame_us = 0;
    while (self->_ticker_running.load()) {
        int64_t now_us = esp_timer_get_time();
        if (last_frame_us != 0 && now_us - last_frame_us < 33 * 1000) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        last_frame_us = now_us;
        self->_renderTickerFrame(now_us / 1000);
    }
    self->_ticker_done.store(true);
    vTaskDelete(nullptr);
}

// 每只股票一条信息一次性滚出来: "代码 现价 涨跌幅%" (例 "688018 122.50 -0.50%").
// 整条单一颜色: 涨=纯红, 跌=纯绿, 平=灰 (中国惯例). LED 用纯色 (不用 LCD 的祖母绿,
// 那个带蓝分量在灯上发蓝). 完整从右向左滚一遍 → 暂停黑屏 → 下一只.
void AppStocks::_renderTickerFrame(int64_t now_ms)
{
    size_t n;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        n = _items.size();
    }
    if (n == 0 || !_strip) return;

    int64_t seg_t = now_ms - _ticker_seg_start_ms;

    // 一只股票一个周期 = 滚完一遍 + 暂停. 周期到 → 下一只.
    if (_ticker_seg_dur_ms > 0 && seg_t >= _ticker_seg_dur_ms + STOCK_PAUSE_MS) {
        _ticker_index = (int)((_ticker_index + 1) % n);
        _ticker_seg_start_ms = now_ms; seg_t = 0; _ticker_seg_dur_ms = 0;
    }

    StockItem s;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        s = _items[_ticker_index % n];
    }
    // LED 纯色: 涨 红, 跌 绿, 平 灰. (祖母绿/绯红留给 LCD; 灯上用纯三原色才不偏色)
    uint8_t r, g, b;
    if (s.chg > 0.01f)       { r = 255; g = 0;   b = 0;   }  // 涨 → 纯红
    else if (s.chg < -0.01f) { r = 0;   g = 255; b = 0;   }  // 跌 → 纯绿
    else                     { r = 120; g = 120; b = 120; }  // 平 → 灰

    snprintf(_ticker_buf, sizeof(_ticker_buf), "%s %.2f %+.2f%%",
             s.code.c_str(), s.price, s.chg);
    int text_len = (int)strlen(_ticker_buf);
    int total_w  = text_len * CELL - CHAR_GAP;

    // 首帧 (或刚切股票) 按文字宽算出滚完一遍要多久.
    if (_ticker_seg_dur_ms == 0) {
        _ticker_seg_dur_ms = (int64_t)(total_w + LED_W) * SCROLL_MS_PER_PX;
    }

    // 滚完之后的暂停段: 黑屏 (等下一帧周期到切下一只).
    if (seg_t >= _ticker_seg_dur_ms) {
        std::lock_guard<std::mutex> lk(_strip_mu);
        for (int i = 0; i < LED_N; i++) led_strip_set_pixel(_strip, i, 0, 0, 0);
        led_strip_refresh(_strip);
        return;
    }

    std::lock_guard<std::mutex> lk(_strip_mu);
    // 注意: 不能用 led_strip_clear() —— 它内部 memset+refresh 会把灯先全灭再刷,
    // 与本帧末尾的 refresh 形成「黑一下→画字」两次刷新 = 闪. 改为只清缓冲不刷新,
    // 整帧只在最后 led_strip_refresh 一次.
    for (int i = 0; i < LED_N; i++) led_strip_set_pixel(_strip, i, 0, 0, 0);

    int x_start;
    if (total_w <= LED_W) {
        x_start = (LED_W - total_w) / 2;  // 放得下: 居中静止
    } else {
        int scroll_px = (int)(seg_t / SCROLL_MS_PER_PX);  // 0 → total_w+LED_W, 不 wrap
        x_start = LED_W - scroll_px;                        // 从右进、向左滚一遍
    }

    for (int ci = 0; ci < text_len; ci++) {
        int cx = x_start + ci * CELL;
        if (cx + CHAR_W <= 0) continue;
        if (cx >= LED_W) break;
        char c = _ticker_buf[ci];
        if (c >= 'a' && c <= 'z') c -= ('a' - 'A');
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t* bmp = font5x7[c - 0x20];
        for (int col = 0; col < CHAR_W; col++) {
            int x = cx + col;
            if (x < 0 || x >= LED_W) continue;
            for (int row = 0; row < CHAR_H; row++) {
                if (bmp[row] & (1 << (4 - col))) _setPixelXY(x, row, r, g, b);
            }
        }
    }
    led_strip_refresh(_strip);
}
#endif  // !PLATFORM_BUILD_DESKTOP
