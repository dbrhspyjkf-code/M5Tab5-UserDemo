#include "app_stocks.h"
#include "../utils/theme/theme.h"
#ifdef PLATFORM_BUILD_DESKTOP
#include <cstdlib>              // malloc/free (桌面)
#else
#include <esp_heap_caps.h>      // PSRAM 优先分配 (设备)
#endif
#include "stock_market_hours.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_log.h>
#include <esp_timer.h>
#include "../app_email_led/app_email_led.h"     // setPortAOwnedByApp (PORT A 仲裁)
#include "../app_unit_puzzle/font5x7.h"          // 共享 5x7 点阵字体
#endif

static const char* TAG = "stocks";

// ── 全覆盖中文字体 (与 app_ha/view, app_settings 一致) ────────────────────────
//   stock_row_font() 36px (标题/表头/数据行), zh_font_sm() 20px (状态/按钮).
//   股票字体只包含表格常用字符，未知股票名自动回退到 30px 全字库。
//   30px 是固件内嵌 cbin blob; 20px 从 assets 分区 mmap 原址加载
//   (pack_assets.py 打包); 桌面回退到链接进来的 20px C 数组字体.
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include "../utils/assets/assets_partition.h"
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_sm()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif

extern "C" const lv_font_t font_stocks_36;
static const lv_font_t* stock_row_font()
{
    static lv_font_t font = []() {
        lv_font_t copy = font_stocks_36;
        copy.fallback = zh_font_lg();
        return copy;
    }();
    return &font;
}

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
    if (_chart_ready.exchange(false)) {
        _applyCharts();
    }
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

    // App 打开时已经无条件拉取一次；周期刷新只在 A 股交易时段执行。
    if (!stocks_refresh::isAshareTradingTime(std::time(nullptr))) return;

    int64_t last;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        last = _last_fetch_ms;
    }
    if (last != 0 && (mono_ms() - last) >= FETCH_INTERVAL_MS) {
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
    // Do NOT _destroyUi()/delete _scr here. _close_cb is
    // mooncake::GetMooncake().openApp(tools_id) — that's async (just flags
    // StateGoOpen; the tools app's onOpen()/lv_screen_load() doesn't run
    // until the next mooncake tick), so _scr would still be the *active*
    // screen at the moment of deletion — LVGL warns "the active screen was
    // deleted" and a later layout pass on the dangling pointer panics
    // (Guru Meditation, lv_obj_align_to/lv_obj_update_layout). Matches
    // AppUnitPuzzle::_requestClose(), which has the same openApp()-based
    // close_cb and the same reasoning. Leave _scr in place — harmless, gets
    // covered by the tools screen once it loads — and let the existing
    // idempotent `if (_scr) _destroyUi();` guard at the top of onOpen()
    // reap it next time this app is reopened.
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
    if (chg > 0.01f)  return C_UP();
    if (chg < -0.01f) return C_DOWN();
    return C_FLAT();
}

// ════════════════════════════════════════════════════════════════════════════
//  UI
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_buildUi()
{
    LvglLockGuard lock;

    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG()), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 卡片网格 4×3 (2026-10-27 用户指定, 参考工具页卡片) ──────────────
    // 每只股票一张卡: 名称 / 股价 / 涨跌幅; 涨=红底 跌=绿底;
    // 排布顺序 = 涨跌幅降序 (在 _refreshUiFromItems 里排序绑定).
    for (int i = 0; i < N_CARDS; i++) {
        const int col = i % GRID_COLS, row = i / GRID_COLS;
        lv_obj_t* card = lv_obj_create(_scr);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, GRID_X0 + col * (CARD_W + GRID_GAP),
                       GRID_Y0 + row * (CARD_H + GRID_GAP));
        lv_obj_set_style_bg_color(card, lv_color_hex(C_HEADER()), 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)-1);   // 尚未绑定数据
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
            int idx = (int)(intptr_t)lv_obj_get_user_data(
                          lv_event_get_current_target_obj(e));
            self->_showDetail(idx);
        }, LV_EVENT_CLICKED, this);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, "--");
        lv_obj_set_style_text_font(name, zh_font_lg(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 20);

        lv_obj_t* price = lv_label_create(card);
        lv_label_set_text(price, "--");
        lv_obj_set_style_text_font(price, &lv_font_montserrat_38, 0);
        lv_obj_set_style_text_color(price, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(price, LV_ALIGN_TOP_MID, 0, 66);

        lv_obj_t* chg = lv_label_create(card);
        lv_label_set_text(chg, "");
        lv_obj_set_style_text_font(chg, &lv_font_montserrat_30, 0);
        lv_obj_set_style_text_color(chg, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(chg, LV_ALIGN_TOP_MID, 0, 124);

        // 涨跌额 (24px, 略淡一档)
        lv_obj_t* amt = lv_label_create(card);
        lv_label_set_text(amt, "");
        lv_obj_set_style_text_font(amt, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(amt, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(amt, LV_ALIGN_TOP_MID, 0, 166);

        _cards[i]      = card;
        _card_name[i]  = name;
        _card_price[i] = price;
        _card_chg[i]   = chg;
        _card_amt[i]   = amt;
    }

    lv_screen_load(_scr);
    _installSwipeGesture();
}

void AppStocks::_destroyUi()
{
    LvglLockGuard lock;
    *_alive = false;  // any stale/in-flight gesture callback must no-op from here on
    _removeSwipeGesture();
    _closeDetail();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    for (int i = 0; i < N_CARDS; i++)
        _cards[i] = _card_name[i] = _card_price[i] = _card_chg[i] =
        _card_amt[i] = nullptr;
    if (_poll_timer) { lv_timer_delete(_poll_timer); _poll_timer = nullptr; }
    _alive = std::make_shared<bool>(true);  // fresh flag for the next _buildUi/_installSwipeGesture
}

// Swipe-up-to-exit, registered on the persistent global pointer indev instead
// of a back button (see the comment where the old button used to be built).
// user_data is a heap SwipeCtx (app + weak_ptr<bool> alive), not a raw `this`:
// lv_indev_remove_event_cb_with_user_data isn't guaranteed to win the race
// against an event already in flight, and touching `app` from a stale
// callback after teardown is exactly the crash this whole change fixes.
// SwipeCtx is intentionally never freed — see app_ha/view/view.cpp's
// ExitCbCtx for the identical, already-proven pattern and rationale.
namespace {
struct SwipeCtx {
    AppStocks* app;
    std::weak_ptr<bool> alive;
};
}  // namespace

void AppStocks::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // Defer: don't tear down _scr synchronously from inside indev dispatch.
                    auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestClose();
                        delete c;
                    }, app_ctx);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppStocks::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // _gesture_ctx intentionally not freed — see its allocation site.
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
    lv_obj_set_size(_detail_modal, 1000, 620);
    lv_obj_center(_detail_modal);
    lv_obj_set_style_bg_color(_detail_modal, lv_color_hex(C_HEADER()), 0);
    lv_obj_set_style_border_color(_detail_modal, lv_color_hex(theme::cur().accent), 0);
    lv_obj_set_style_border_width(_detail_modal, 2, 0);
    lv_obj_set_style_radius(_detail_modal, 16, 0);
    lv_obj_set_style_pad_all(_detail_modal, 20, 0);
    lv_obj_clear_flag(_detail_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_detail_modal);
    lv_label_set_text_fmt(title, "%s  %s   %.2f  %+.2f%%", s.name.c_str(),
                          s.code.c_str(), s.price, s.chg);
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

    // canvas: 944x420 ARGB8888 (堆缓冲, worker 绘制在 UI 线程做)
    #ifdef PLATFORM_BUILD_DESKTOP
    _chart_pixels = malloc(944 * 420 * 4);
#else
    // 先试 PSRAM (大缓冲), 失败退内部 RAM
    _chart_pixels = heap_caps_malloc(944 * 420 * 4, MALLOC_CAP_SPIRAM);
    if (!_chart_pixels) _chart_pixels = heap_caps_malloc(944 * 420 * 4, MALLOC_CAP_8BIT);
#endif
    if (_chart_pixels) memset(_chart_pixels, 0, 944 * 420 * 4);
    _chart_canvas = lv_canvas_create(_detail_modal);
    lv_canvas_set_buffer(_chart_canvas, _chart_pixels, 944, 420, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_align(_chart_canvas, LV_ALIGN_TOP_LEFT, 0, 56);

    _chart_status = lv_label_create(_detail_modal);
    lv_label_set_text(_chart_status, "行情加载中…");
    lv_obj_set_style_text_font(_chart_status, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_chart_status, lv_color_hex(C_DIM()), 0);
    lv_obj_center(_chart_status);

    lv_obj_t* hint = lv_label_create(_detail_modal);
    lv_label_set_text(hint, "分时 / 日K");
    lv_obj_set_style_text_font(hint, zh_font_sm(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM()), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -4, 8);

    lv_obj_t* close_btn = lv_button_create(_detail_modal);
    lv_obj_set_size(close_btn, 150, 54);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(theme::cur().accent), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_t* cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "关闭");
    lv_obj_set_style_text_font(cl, zh_font_sm(), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t* e) {
        static_cast<AppStocks*>(lv_event_get_user_data(e))->_closeDetail();
    }, LV_EVENT_CLICKED, this);

    // 异步拉取分时 + 日K (worker 不碰 LVGL)
    {
        std::lock_guard<std::mutex> lk(_chart_mu);
        _chart = ChartData{};
        _chart_code = s.code;
    }
    _chart_ready.store(false);
    if (!_chart_in_flight.exchange(true)) {
        std::shared_ptr<bool> alive = _alive;
        GetHAL()->tryRunDetached([this, code = s.code, alive]() {
            _chartFetchWorker(code, alive);
        });
    }
}

void AppStocks::_closeDetail()
{
    if (_detail_modal) {
        lv_obj_delete(_detail_modal);
        _detail_modal = nullptr;
    }
    _chart_canvas = _chart_status = nullptr;
    if (_chart_pixels) {           // canvas 缓冲是堆分配, 弹窗关了就还
#ifdef PLATFORM_BUILD_DESKTOP
        free(_chart_pixels);
#else
        heap_caps_free(_chart_pixels);
#endif
        _chart_pixels = nullptr;
    }
    _chart_ready.store(false);
}

// ════════════════════════════════════════════════════════════════════════════
//  分时图 / K线图 (腾讯行情接口, 免 key; 2026-10-27)
//  worker 只填 _chart + 置 _chart_ready, 绘制在 UI 线程 (_applyCharts)。
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_chartFetchWorker(std::string code, std::shared_ptr<bool> alive)
{
    // code 前缀: 6 开头 = 沪(sh), 其余(0/3) = 深(sz)
    const std::string prefixed = (code.rfind("6", 0) == 0 ? "sh" : "sz") + code;

    ChartData local;
    auto parse_ok = [](const std::string& body) {
        try { auto j = nlohmann::json::parse(body); return j.value("code", -1) == 0; }
        catch (...) { return false; }
    };

    // ── 分时: web.ifzq.gtimg.cn/appstock/app/minute/query ──
    {
        auto resp = GetHAL()->httpGet(
            "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=" + prefixed);
        if (resp.ok && parse_ok(resp.body)) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                const auto& arr = j["data"][prefixed]["data"]["data"];
                if (arr.is_array()) {
                    for (auto& row : arr) {
                        // 每行: "HHMM price cum_volume cum_amount"
                        const std::string line = row.get<std::string>();
                        const size_t sp1 = line.find(' ');
                        const size_t sp2 = line.find(' ', sp1 + 1);
                        if (sp1 == std::string::npos || sp2 == std::string::npos) continue;
                        local.minute_prices.push_back(std::stof(line.substr(sp1 + 1, sp2 - sp1 - 1)));
                    }
                }
            } catch (...) {}
        }
    }

    // ── 日K: fqkline/get, 最近 60 根前复权 ──
    {
        auto resp = GetHAL()->httpGet(
            "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=" +
            prefixed + ",day,,,60,qfq");
        if (resp.ok && parse_ok(resp.body)) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                auto node = j["data"][prefixed];
                // 有 qfqday 用 qfqday, 否则 day
                const auto* arr = node.contains("qfqday") ? &node["qfqday"] : &node["day"];
                if (arr->is_array()) {
                    for (auto& row : *arr) {
                        if (!row.is_array() || row.size() < 5) continue;
                        local.day_date.push_back(row[0].get<std::string>());
                        local.day_open.push_back(std::stof(row[1].get<std::string>()));
                        local.day_close.push_back(std::stof(row[2].get<std::string>()));
                        local.day_high.push_back(std::stof(row[3].get<std::string>()));
                        local.day_low.push_back(std::stof(row[4].get<std::string>()));
                    }
                }
            } catch (...) {}
        }
    }

    local.ok = !local.minute_prices.empty() || !local.day_close.empty();
    {
        std::lock_guard<std::mutex> lk(_chart_mu);
        _chart = std::move(local);
    }
    _chart_ready.store(true);   // onRunning 在 UI 线程消费
    (void)alive;                // 生命周期由 _chart_in_flight + UI 判空兜底
}

// ── 绘制辅助: 在 canvas 像素上画横线/矩形/折线 (ARGB8888, XRGB 值即可) ──
namespace {
inline void px_fill(void* pix, int W, int H, uint32_t color)
{
    uint32_t* p = (uint32_t*)pix;
    for (int i = 0; i < W * H; i++) p[i] = color | 0xFF000000;
}
inline void px_rect(void* pix, int W, int H, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= W) x1 = W - 1;
    if (y1 >= H) y1 = H - 1;
    for (int y = y0; y <= y1; y++) {
        uint32_t* row = (uint32_t*)pix + y * W;
        for (int x = x0; x <= x1; x++) row[x] = color | 0xFF000000;
    }
}
inline void px_line(void* pix, int W, int H, int x0, int y0, int x1, int y1, uint32_t color)
{
    // Bresenham
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H)
            ((uint32_t*)pix)[y0 * W + x0] = color | 0xFF000000;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
}  // namespace

void AppStocks::_applyCharts()
{
    LvglLockGuard lock;
    if (!_chart_canvas || !_chart_pixels) return;   // 弹窗已关

    ChartData c;
    {
        std::lock_guard<std::mutex> lk(_chart_mu);
        c = _chart;
    }
    if (!c.ok) {
        if (_chart_status)
            lv_label_set_text(_chart_status, "行情获取失败");
        return;
    }
    if (_chart_status) {
        lv_obj_add_flag(_chart_status, LV_OBJ_FLAG_HIDDEN);
        _chart_status = nullptr;
    }

    constexpr int W = 944, H = 420;
    void* pix = _chart_pixels;
    const uint32_t COL_BG    = 0x0A1610;   // 图区底
    const uint32_t COL_GRID  = 0x24402F;
    const uint32_t COL_UP    = 0xE74C3C;   // A 股: 涨红
    const uint32_t COL_DOWN  = 0x2ECC71;   // 跌绿
    const uint32_t COL_AXIS  = 0x6E9486;

    // ── 有分时画分时, 否则画日K; 两者都有画分时(左 55%) + 日K(右 45%) ──
    // 简化: 先分时整幅; 无分时再日K整幅 (弹窗只有一个 canvas)
    px_fill(pix, W, H, COL_BG);

    if (!c.minute_prices.empty()) {
        // 分时: 昨收基准 = 第一根分钟价之前的均线……腾讯接口无昨收字段,
        // 用全天均价近似: 以开盘(第一值)为基准线即可表达强弱.
        const float base = c.minute_prices.front();
        float hi = base, lo = base;
        for (float v : c.minute_prices) { hi = v > hi ? v : hi; lo = v < lo ? v : lo; }
        if (hi - lo < 0.01f) { hi = base + 0.05f; lo = base - 0.05f; }
        const float pad = (hi - lo) * 0.08f; hi += pad; lo -= pad;

        const int L = 46, R = W - 14, T = 16, B = H - 30;
        // 网格: 中轴 + 上下边
        px_rect(pix, W, H, L, (T + B) / 2, R, (T + B) / 2, COL_GRID);
        px_rect(pix, W, H, L, T, R, T, COL_GRID);
        px_rect(pix, W, H, L, B, R, B, COL_GRID);
        // 价格→y
        auto y_of = [&](float v) {
            return (int)((B - T) * (hi - v) / (hi - lo)) + T;
        };
        // 涨跌区着色 (base 之上淡红 / 之下淡绿)
        const int yb = y_of(base);
        if (yb > T)      px_rect(pix, W, H, L, T, R, yb - 1, 0x1C2420);       // 下方淡绿区
        if (yb + 1 <= B) px_rect(pix, W, H, L, yb + 1, R, B, 0x2A1E1C);       // 上方淡红区
        px_rect(pix, W, H, L, yb, R, yb, COL_AXIS);
        // 分时折线 (涨红跌绿, 对基准)
        const uint32_t line_col = c.minute_prices.back() >= base ? COL_UP : COL_DOWN;
        const int n = (int)c.minute_prices.size();
        int px_ = L, py_ = y_of(c.minute_prices[0]);
        for (int i = 1; i < n; i++) {
            const int x = L + (R - L) * i / (n - 1 > 0 ? n - 1 : 1);
            const int y = y_of(c.minute_prices[i]);
            px_line(pix, W, H, px_, py_, x, y, line_col);
            px_ = x; py_ = y;
        }
    } else if (!c.day_close.empty()) {
        // 日K: 最近 60 根蜡烛
        const int n = (int)c.day_close.size();
        float hi = -1e9f, lo = 1e9f;
        for (int i = 0; i < n; i++) {
            hi = c.day_high[i] > hi ? c.day_high[i] : hi;
            lo = c.day_low[i]  < lo ? c.day_low[i]  : lo;
        }
        const float pad = (hi - lo) * 0.06f; hi += pad; lo -= pad;
        const int L = 20, R = W - 20, T = 16, B = H - 30;
        auto y_of = [&](float v) { return (int)((B - T) * (hi - v) / (hi - lo)) + T; };
        px_rect(pix, W, H, L, T, R, T, COL_GRID);
        px_rect(pix, W, H, L, B, R, B, COL_GRID);
        const int step = (R - L) / n;
        const int body_w = step * 3 / 5 > 3 ? step * 3 / 5 : 3;
        for (int i = 0; i < n; i++) {
            const int x = L + step * i + step / 2;
            const bool up = c.day_close[i] >= c.day_open[i];
            const uint32_t col = up ? COL_UP : COL_DOWN;
            const int yh = y_of(c.day_high[i]), yl = y_of(c.day_low[i]);
            int yo = y_of(c.day_open[i]), yc = y_of(c.day_close[i]);
            if (yo > yc) { int t = yo; yo = yc; yc = t; }
            px_line(pix, W, H, x, yh, x, yl, col);          // 影线
            px_rect(pix, W, H, x - body_w / 2, yo, x + body_w / 2, yc > yo ? yc : yo + 1, col);
        }
    }

    lv_obj_invalidate(_chart_canvas);
}



// 颜色线性插值: t=0 → a, t=1 → b
static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    const int br = (b >> 16) & 0xFF, bg_ = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((uint32_t)(ar + (br - ar) * t) << 16) |
           ((uint32_t)(ag + (bg_ - ag) * t) << 8) |
           (uint32_t)(ab + (bb - ab) * t);
}

void AppStocks::_refreshUiFromItems()
{
    LvglLockGuard lock;
    if (!_scr) return;
    std::vector<StockItem> snapshot;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        snapshot = _items;
    }

    // 按涨跌幅降序; 卡片从左到右、从上到下依次绑定
    std::vector<int> order(snapshot.size());
    for (int i = 0; i < (int)order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return snapshot[a].chg > snapshot[b].chg; });

    // 渐变端点: 可见卡中的最大涨幅 / 最大跌幅
    float max_chg = 0, min_chg = 0;
    for (int i = 0; i < (int)order.size() && i < N_CARDS; i++) {
        max_chg = snapshot[order[i]].chg > max_chg ? snapshot[order[i]].chg : max_chg;
        min_chg = snapshot[order[i]].chg < min_chg ? snapshot[order[i]].chg : min_chg;
    }

    for (int i = 0; i < N_CARDS; i++) {
        lv_obj_t* card = _cards[i];
        if (i >= (int)order.size()) {
            if (!snapshot.empty()) {          // 数据比卡位少 → 多余的卡藏掉
                lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            // 完全无数据 (首拉失败等): 显示中性 "--" 卡
            lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_user_data(card, (void*)(intptr_t)-1);
            lv_obj_set_style_bg_color(card, lv_color_hex(C_HEADER()), 0);
            lv_obj_set_style_text_color(_card_name[i],  lv_color_hex(C_DIM()), 0);
            lv_obj_set_style_text_color(_card_price[i], lv_color_hex(C_DIM()), 0);
            lv_obj_set_style_text_color(_card_chg[i],   lv_color_hex(C_DIM()), 0);
            lv_obj_set_style_text_color(_card_amt[i],   lv_color_hex(C_DIM()), 0);
            lv_label_set_text(_card_name[i],  "--");
            lv_label_set_text(_card_price[i], "--");
            lv_label_set_text(_card_chg[i],   "");
            lv_label_set_text(_card_amt[i],   "");
            continue;
        }

        lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
        const auto& st = snapshot[order[i]];
        lv_obj_set_user_data(card, (void*)(intptr_t)order[i]);  // 点击 → 原始下标

        // 中国 A 股惯例: 涨=红 跌=绿, 彩底白字.
        // 颜色随涨跌幅度渐变 (2026-10-27 用户指定): 涨幅最大的卡用满红
        // C_UP, 跌幅最大的卡用满绿 C_DOWN, 其余按 |chg|/极值 过渡.
        // 渐变只在红/绿色系内部进行 (浅端 = 暗红/暗绿, 而非中性底),
        // 保证任何档位肉眼都明确是红或绿 (中性底会把红混成褐色).
        const bool up   = st.chg > 0.01f;
        const bool down = st.chg < -0.01f;
        constexpr uint32_t UP_DARK   = 0x8A2B20;  // 渐变浅端: 暗红
        constexpr uint32_t DOWN_DARK = 0x1F6B4E;  // 渐变浅端: 暗绿
        uint32_t bg = C_HEADER();
        if (up || down) {
            const float max_up   = max_chg > 0 ? max_chg : 0;
            const float max_down = min_chg < 0 ? -min_chg : 0;
            const float t = up ? (max_up > 0 ? st.chg / max_up : 1)
                               : (max_down > 0 ? -st.chg / max_down : 1);
            bg = lerp_color(up ? UP_DARK : DOWN_DARK, up ? C_UP() : C_DOWN(), t);
        }
        const uint32_t fg  = (up || down) ? 0xFFFFFF : C_TEXT();
        const uint32_t fg2 = (up || down) ? 0xFFFFFF : C_DIM();
        lv_obj_set_style_bg_color(card, lv_color_hex(bg), 0);
        lv_obj_set_style_text_color(_card_name[i],  lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(_card_price[i], lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(_card_chg[i],   lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(_card_amt[i],   lv_color_hex(fg2), 0);

        lv_label_set_text(_card_name[i], st.name.c_str());
        char buf[24];
        snprintf(buf, sizeof(buf), "%.2f", st.price);
        lv_label_set_text(_card_price[i], buf);
        snprintf(buf, sizeof(buf), "%+.2f%%", st.chg);
        lv_label_set_text(_card_chg[i], buf);
        snprintf(buf, sizeof(buf), "%+.2f", st.pchg);
        lv_label_set_text(_card_amt[i], buf);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  数据拉取 (hermes :8766/api/stocks/portfolio, mx-selfselect 后端)
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_fetchStocksAsync()
{
    bool expected = false;
    if (!_fetch_in_flight.compare_exchange_strong(expected, true)) {
        mclog::tagInfo(TAG, "fetch already in flight");
        return;
    }

    // app 实例随 mooncake 常驻 (从不 uninstall), 捕获 this 安全。
    if (GetHAL()->tryRunDetached([this]() {
            try {
                _doFetch();
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lk(_items_mu);
                    _fetch_error = true;
                    _fetch_error_msg = e.what();
                    _last_fetch_ms = mono_ms();
                }
                mclog::tagWarn(TAG, "fetch exception: {}", e.what());
                _refreshUiFromItems();
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lk(_items_mu);
                    _fetch_error = true;
                    _fetch_error_msg = "unknown fetch exception";
                    _last_fetch_ms = mono_ms();
                }
                mclog::tagWarn(TAG, "unknown fetch exception");
                _refreshUiFromItems();
            }
            _fetch_in_flight.store(false);
        })) {
        mclog::tagInfo(TAG, "fetch scheduled");
    } else {
        _fetch_in_flight.store(false);
        mclog::tagWarn(TAG, "fetch worker spawn failed");
    }
}

void AppStocks::_doFetch()
{
    auto* hal = GetHAL();
    std::string svc_host = hal->getConfig("svc_host", "192.168.1.200");
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
                s.one_sentence = it.value("one_sentence", std::string());
                s.analysis_summary = it.value("analysis_summary", std::string());
                s.analysis_date = it.value("analysis_date", std::string());
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
