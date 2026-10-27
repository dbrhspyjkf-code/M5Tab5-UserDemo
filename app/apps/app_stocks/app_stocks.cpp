#include "app_stocks.h"
#include "../utils/theme/theme.h"
#include <cstdlib>              // malloc/free (两平台统一; 设备
                                // SPIRAM_USE_MALLOC=y, >16KB 自动落 PSRAM)
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
// 颜色线性插值: t=0 → a, t=1 → b (指数卡/自选股卡渐变共用)
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

static const lv_font_t* stock_row_font()
{
    static lv_font_t font = []() {
        lv_font_t copy = font_stocks_36;
        copy.fallback = zh_font_lg();
        return copy;
    }();
    return &font;
}

// 走势图画布: 全屏弹窗 (1280x720), 顶部 64px 标题/切换tab行, 画布铺满其余
// (与 AppStocks::SCREEN_W/H 一致, 那俩是 private 拿不出来)
static constexpr int CHART_W = 1280;
static constexpr int CHART_H = 720 - 64;

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
    if (_index_ready.exchange(false)) {
        _applyIndices();
    }
    // 指数页在前台时 60s 刷新
    if (_page == 1 && _scr && lv_screen_active() == _scr) {
        if (_last_index_ms == 0 || mono_ms() - _last_index_ms >= 60000) {
            _last_index_ms = mono_ms();
            _fetchIndicesAsync();
        }
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

// 上拨退出去重: GESTURE (拨动中途) 与 RELEASED 位移 (抬起) 双轨都会对同一次
// 上拨命中, 500ms 内只认第一次 —— 连续两次 _requestClose 会在关闭过程中再调
// close()/openApp, 把 mooncake 状态机搞乱 (2026-10-27 实锤: 上拨退出失效).
void AppStocks::_requestExitOnce()
{
    const int64_t now = mono_ms();
    if (now - _last_exit_ms < 500) return;
    _last_exit_ms = now;
    _requestClose();
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

// 指数卡片长按 → 走势图 (行情未到时标题只带名称+代码)
void AppStocks::_openIndexChart(int i)
{
    IndexQuote q;
    {
        std::lock_guard<std::mutex> lk(_index_mu);
        q = _index_quotes[i];
    }
    char title[160];
    if (q.ok)
        std::snprintf(title, sizeof(title), "%s  %s   %.2f  %+.2f%%",
                      kIndices[i].name, kIndices[i].tencent, q.price, q.chg_pct);
    else
        std::snprintf(title, sizeof(title), "%s  %s",
                      kIndices[i].name, kIndices[i].tencent);
    // ifzq 代码映射: qt 的 r_hkHSI → hkHSI (去 r_ 前缀), 其余原样
    std::string tencent = kIndices[i].tencent;
    if (tencent.rfind("r_", 0) == 0) tencent.erase(0, 2);
    _openChart(title, tencent);
}

// 卡片描边闪烁 160ms 后执行 open (长按反馈, 用户要求"闪烁一下再打开").
// 闪烁走 outline 而非 bg: 卡底是按涨跌幅度动态重刷的渐变色, 快照恢复会把它
// 覆盖成中性底 (2026-10-27 实锤: 开图后卡片"回不去原色"). 一次性定时器自毁.
void AppStocks::_flashCardThen(lv_obj_t* card, uint32_t flash_col,
                               std::function<void()> open)
{
    if (!card) return;
    struct Ctx {
        lv_obj_t* card;
        std::function<void()> open;
    };
    auto* ctx = new Ctx{card, std::move(open)};
    lv_obj_set_style_outline_color(card, lv_color_hex(flash_col), 0);
    lv_obj_set_style_outline_width(card, 6, 0);
    lv_timer_t* t = lv_timer_create([](lv_timer_t* t) {
        auto* c = static_cast<Ctx*>(lv_timer_get_user_data(t));
        lv_obj_set_style_outline_width(c->card, 0, 0);
        c->open();
        lv_timer_del(t);
        delete c;
    }, 160, ctx);
}

// 按需创建卡位到滚动容器 (n = 目标数量). 只增不减, 多余卡位在刷新时隐藏.
void AppStocks::_ensureCards(int n)
{
    LvglLockGuard lock;
    if (!_stocks_scroll) return;
    if (n > MAX_CARDS) n = MAX_CARDS;
    for (int i = (int)_cards.size(); i < n; i++) {
        const int col = i % GRID_COLS, row = i / GRID_COLS;
        lv_obj_t* card = lv_obj_create(_stocks_scroll);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, GRID_X0 + col * (CARD_W + GRID_GAP),
                       24 + row * (CARD_H + GRID_GAP));
        lv_obj_set_style_bg_color(card, lv_color_hex(C_HEADER()), 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)-1);   // 尚未绑定数据
        // 长按 0.5s 开走势图 (用户要求: 闪烁一下再打开; 短按无动作,
        // 避免误触 + 与翻页拨动共存). indev 长按阈值在 _installSwipeGesture 设 500ms.
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
            const int idx = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            float chg = 0;   // 闪烁色按涨跌方向: 涨红/跌绿 (与卡片渐变同语义)
            {
                std::lock_guard<std::mutex> lk(self->_items_mu);
                if (idx >= 0 && idx < (int)self->_items.size())
                    chg = self->_items[idx].chg;
            }
            self->_flashCardThen(lv_event_get_current_target_obj(e),
                                 self->_chgColor(chg),
                                 [self, idx]() { self->_showDetail(idx); });
        }, LV_EVENT_LONG_PRESSED, this);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, "--");
        lv_obj_set_style_text_font(name, zh_font_lg(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 16);

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

        _cards.push_back(card);
        _card_name.push_back(name);
        _card_price.push_back(price);
        _card_chg.push_back(chg);
        _card_amt.push_back(amt);
    }
    // 滚动内容高度: 行数 × 行高 + 上下边距; 卡片绝对定位在容器坐标系里,
    // 容器按内容设置滚动范围 (内容不足一屏时自然不滚).
    const int rows = (n + GRID_COLS - 1) / GRID_COLS;
    const int content_h = 24 + rows * (CARD_H + GRID_GAP);
    lv_obj_set_size(_stocks_scroll, SCREEN_W, SCREEN_H);
    lv_obj_scroll_to_y(_stocks_scroll, 0, LV_ANIM_OFF);   // 数量变化后回到顶部
    (void)content_h;  // LVGL 自适应子对象范围 (cards 是容器子对象, 滚动范围自动)
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

    // 两页容器: 0=自选股 1=指数看板 (左右拨切换, _setPage)
    _stocks_page = lv_obj_create(_scr);
    lv_obj_set_size(_stocks_page, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_stocks_page, 0, 0);
    lv_obj_set_style_bg_opa(_stocks_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_stocks_page, 0, 0);
    lv_obj_set_style_pad_all(_stocks_page, 0, 0);
    lv_obj_clear_flag(_stocks_page, LV_OBJ_FLAG_SCROLLABLE);

    // ── 卡片网格 4 列 (参考工具页卡片风格) ──────────────────────────────
    // 纵向滚动容器承载: 行数随持仓数动态扩展, 超过一屏上下拨滚动查看;
    // 滚动发生在本容器, 切页/退出走全局 indev 手势 (方向不同, 互不冲突).
    _stocks_scroll = lv_obj_create(_stocks_page);
    lv_obj_set_size(_stocks_scroll, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_stocks_scroll, 0, 0);
    lv_obj_set_style_bg_opa(_stocks_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_stocks_scroll, 0, 0);
    lv_obj_set_style_pad_all(_stocks_scroll, 0, 0);
    lv_obj_set_scroll_dir(_stocks_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_stocks_scroll, LV_SCROLLBAR_MODE_AUTO);

    // 卡位在 _refreshUiFromItems 里按持仓数动态创建 (_ensureCards)

    _buildIndexPage();
    {
        // 桌面调试: TAB5_PAGE=1 直接落在指数页 (设备忽略, 恒 0)
        int page0 = 0;
#ifdef PLATFORM_BUILD_DESKTOP
        if (const char* e = getenv("TAB5_PAGE")) page0 = atoi(e);
#endif
        _setPage(page0);
    }

    lv_screen_load(_scr);
    _installSwipeGesture();
}

// ════════════════════════════════════════════════════════════════════════════
//  指数看板 (2026-10-27): 9 指数 3×3 卡片, qt.gtimg.cn 实时接口一次拉全
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_buildIndexPage()
{
    _index_page = lv_obj_create(_scr);
    lv_obj_set_size(_index_page, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_index_page, SCREEN_W, 0);   // 右侧待命 (左拨滑入)
    lv_obj_set_style_bg_opa(_index_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_index_page, 0, 0);
    lv_obj_set_style_pad_all(_index_page, 0, 0);
    lv_obj_clear_flag(_index_page, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int IC_W = 392, IC_H = 214, IG_X = 20, IG_Y = 18, IG_M = 32;
    for (int i = 0; i < 9; i++) {
        const int col = i % 3, row = i / 3;
        lv_obj_t* card = lv_obj_create(_index_page);
        lv_obj_set_size(card, IC_W, IC_H);
        lv_obj_set_pos(card, IG_M + col * (IC_W + IG_X), 20 + row * (IC_H + IG_Y));
        lv_obj_set_style_bg_color(card, lv_color_hex(C_HEADER()), 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        // 长按 0.5s 开指数走势图 (与个股一致: 闪烁一下再打开; 短按无动作)
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)i);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
            const int i = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            float chg = 0;   // 闪烁色按涨跌方向
            {
                std::lock_guard<std::mutex> lk(self->_index_mu);
                chg = self->_index_quotes[i].chg_pct;
            }
            self->_flashCardThen(lv_event_get_current_target_obj(e),
                                 self->_chgColor(chg),
                                 [self, i]() { self->_openIndexChart(i); });
        }, LV_EVENT_LONG_PRESSED, this);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, kIndices[i].name);
        lv_obj_set_style_text_font(name, zh_font_lg(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(theme::cur().text_soft), 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 22);

        lv_obj_t* price = lv_label_create(card);
        lv_label_set_text(price, "--");
        lv_obj_set_style_text_font(price, &lv_font_montserrat_38, 0);
        lv_obj_set_style_text_color(price, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(price, LV_ALIGN_TOP_MID, 0, 70);

        lv_obj_t* amt = lv_label_create(card);
        lv_label_set_text(amt, "");
        lv_obj_set_style_text_font(amt, &lv_font_montserrat_30, 0);
        lv_obj_set_style_text_color(amt, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(amt, LV_ALIGN_TOP_MID, 0, 120);

        lv_obj_t* pct = lv_label_create(card);
        lv_label_set_text(pct, "");
        lv_obj_set_style_text_font(pct, &lv_font_montserrat_30, 0);
        lv_obj_set_style_text_color(pct, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(pct, LV_ALIGN_TOP_MID, 0, 162);

        _index_cards[i] = card;
        _index_name[i]  = name;
        _index_price[i] = price;
        _index_amt[i]   = amt;
        _index_pct[i]   = pct;
    }
}

void AppStocks::_setPage(int page)
{
    // 横向拨动由 GESTURE (快速甩动, 拨动中途触发) 与 RELEASED 位移 (慢速拨动,
    // 抬起触发) 双轨判据承载: 同一次快速拨动两轨都会命中, 500ms 内只认第一次,
    // 否则翻过去又翻回来 (2026-10-27 实锤: 看得到对页、松手弹回).
    // 否则翻过去又翻回来 (2026-10-27 实锤: 看得到对页、松手弹回).
    // 去重只拦「翻页」; 摆位始终执行 (构建首调 _page 未变也要把两页摆到位).
    if (page != _page) {
        const int64_t now = mono_ms();
        if (now - _last_flip_ms < 500) return;
        _last_flip_ms = now;
        _page = page;
    }
    if (!_scr || !_stocks_page || !_index_page) return;
    lv_obj_set_pos(_stocks_page, page == 0 ? 0 : -SCREEN_W, 0);
    lv_obj_set_pos(_index_page, page == 1 ? 0 : SCREEN_W, 0);
    // 指数页第一次进入时拉一次
    if (page == 1 && _last_index_ms == 0) _fetchIndicesAsync();
}

// GBK → UTF-8 这里不需要: qt.gtimg 返回的字段只取数字, 名字用本地 kIndices 表
void AppStocks::_indexFetchWorker()
{
    auto resp = GetHAL()->httpGet(
        "https://qt.gtimg.cn/q=sh000001,sz399001,sz399006,sh000688,sh000300,"
        "r_hkHSI,usDJI,usIXIC,usINX");
    IndexQuote quotes[9];
    bool any = false;
    if (resp.ok) {
        // 返回体: v_sh000001="..."; v_sz399001="..."; …  GBK 编码但只解析 ASCII 字段
        size_t pos = 0;
        int idx = 0;
        while (idx < 9) {
            const std::string key = "v_" + std::string(kIndices[idx].tencent) + "=\"";
            const size_t p = resp.body.find(key, pos);
            if (p == std::string::npos) { idx++; continue; }
            const size_t v0 = p + key.size();
            const size_t v1 = resp.body.find('"', v0);
            if (v1 == std::string::npos) break;
            // 字段: [3]=现价 [4]=昨收 [31]=涨跌额 [32]=涨跌幅
            auto field = [&](int n) -> std::string {
                size_t f = v0;
                for (int k = 0; k < n; k++) {
                    f = resp.body.find('~', f);
                    if (f == std::string::npos || f >= v1) return "";
                    f++;
                }
                size_t e = resp.body.find('~', f);
                if (e == std::string::npos || e > v1) e = v1;
                return resp.body.substr(f, e - f);
            };
            try {
                quotes[idx].price   = std::stof(field(3));
                const float prev    = std::stof(field(4));
                quotes[idx].chg_amt = std::stof(field(31));
                quotes[idx].chg_pct = std::stof(field(32));
                quotes[idx].ok      = quotes[idx].price > 0 && prev > 0;
                any = any || quotes[idx].ok;
            } catch (...) {}
            pos = v1;
            idx++;
        }
    }
    {
        std::lock_guard<std::mutex> lk(_index_mu);
        for (int i = 0; i < 9; i++) _index_quotes[i] = quotes[i];
    }
    _index_ready.store(any);
}

void AppStocks::_applyIndices()
{
    LvglLockGuard lock;
    IndexQuote q[9];
    {
        std::lock_guard<std::mutex> lk(_index_mu);
        for (int i = 0; i < 9; i++) q[i] = _index_quotes[i];
    }
    // 渐变端点: 本页可见指数中的最大涨幅/最大跌幅 (同自选股卡片设计)
    float max_chg = 0, min_chg = 0;
    for (int i = 0; i < 9; i++) {
        if (!q[i].ok) continue;
        max_chg = q[i].chg_pct > max_chg ? q[i].chg_pct : max_chg;
        min_chg = q[i].chg_pct < min_chg ? q[i].chg_pct : min_chg;
    }
    for (int i = 0; i < 9; i++) {
        if (!_index_cards[i]) continue;
        if (!q[i].ok) {
            lv_obj_set_style_text_color(_index_price[i], lv_color_hex(C_DIM()), 0);
            lv_label_set_text(_index_price[i], "--");
            continue;
        }
        const bool up = q[i].chg_pct > 0.005f, down = q[i].chg_pct < -0.005f;
        // 卡底随涨跌幅度渐变, 只在红/绿色系内部; 幅度方向与首版相反 (同自选股):
        // 涨幅王暗红, 涨幅最小的满红; 跌幅王暗绿, 跌幅最少的满绿. 彩底白字.
        constexpr uint32_t UP_DARK   = 0x8A2B20;
        constexpr uint32_t DOWN_DARK = 0x1F6B4E;
        uint32_t bg = C_HEADER();
        if (up || down) {
            const float max_up   = max_chg > 0 ? max_chg : 0;
            const float max_down = min_chg < 0 ? -min_chg : 0;
            const float t = up ? (max_up > 0 ? 1 - q[i].chg_pct / max_up : 1)
                               : (max_down > 0 ? 1 - (-q[i].chg_pct / max_down) : 1);
            bg = lerp_color(up ? UP_DARK : DOWN_DARK, up ? C_UP() : C_DOWN(), t);
        }
        const uint32_t fg  = (up || down) ? 0xFFFFFF : C_TEXT();
        const uint32_t fg2 = (up || down) ? 0xFFFFFF : C_DIM();
        lv_obj_set_style_bg_color(_index_cards[i], lv_color_hex(bg), 0);
        lv_obj_set_style_text_color(_index_name[i],  lv_color_hex(fg2), 0);
        lv_obj_set_style_text_color(_index_price[i], lv_color_hex(fg), 0);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", q[i].price);
        lv_label_set_text(_index_price[i], buf);
        // %+.2f 自带符号: 涨 +x.xx / 跌 -x.xx, 无需前缀
        snprintf(buf, sizeof(buf), "%+.2f", q[i].chg_amt);
        lv_label_set_text(_index_amt[i], buf);
        snprintf(buf, sizeof(buf), "%+.2f%%", q[i].chg_pct);
        lv_label_set_text(_index_pct[i], buf);
        lv_obj_set_style_text_color(_index_amt[i], lv_color_hex(fg), 0);
        lv_obj_set_style_text_color(_index_pct[i],  lv_color_hex(fg2), 0);
    }
}

void AppStocks::_fetchIndicesAsync()
{
    GetHAL()->tryRunDetached([this]() { _indexFetchWorker(); });
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
    _cards.clear(); _card_name.clear(); _card_price.clear();
    _card_chg.clear(); _card_amt.clear();
    _n_cards = 0;
    _stocks_scroll = nullptr;
    for (int i = 0; i < 9; i++)
        _index_cards[i] = _index_name[i] = _index_price[i] =
        _index_amt[i] = _index_pct[i] = nullptr;
    _stocks_page = _index_page = nullptr;
    _page = 0;
    _last_index_ms = 0;
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
    int press_x = -1, press_y = -1;   // 横向切页位移判定起点
};
}  // namespace

void AppStocks::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            // 卡片长按开走势图阈值: 用户指定 0.5s (默认 400ms)
            lv_indev_set_long_press_time(indev, 500);
            // 横向切页: LVGL 的 GESTURE 依赖速度阈值, 合成/慢速滑动常常不触发.
            // 这里用 PRESSING 记录起点 + RELEASE 判定位移, 与 GESTURE 双轨并行.
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
                    // 起点只在按下瞬间记一次: PRESSING 每个 tick 都发, 若在
                    // PRESSING 里刷新起点, 到抬起时起点已被刷成释放前最后位置,
                    // 位移恒≈0 (2026-10-27 实锤: 翻页位移判别因此失效).
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x; ctx->press_y = p.y;
                    ctx->app->_touch_start = p;   // 供 _showDetail/上拨退出判定用
                    return;
                }
                if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
                    // 走势图弹窗打开期间: 屏内拨动不再翻页/退出 (全屏弹窗下
                    // 误翻页在背后不可见, 上拨还会误退 app)
                    if (ctx->app->_detail_modal) return;
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    // 横向位移 > 90px 且明显大于纵向 → 切页
                    if (abs(dx) > 90 && abs(dx) > abs(dy) * 2) {
                        auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* c = static_cast<SwipeCtx*>(u);
                            auto a = c->alive.lock();
                            if (a && *a) c->app->_setPage(c->app->_page == 0 ? 1 : 0);
                            delete c;
                        }, app_ctx);
                        return;
                    }
                    // 上拨退出: 只认「从屏幕底部往上拨」—— 起点在屏幕下缘
                    // 1/4 (y>540) 且纵向为主、位移>120px. 其余位置的纵向拖动
                    // 一律归列表滚动, 不退出 (2026-10-27 用户反馈: 任意上下
                    // 拖动都误退, 滚动位判定修不住, 改按起点位置判定).
                    const bool from_bottom = ctx->press_y > 540;
                    if (from_bottom && dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* c = static_cast<SwipeCtx*>(u);
                            auto a = c->alive.lock();
                            if (a && *a) c->app->_requestExitOnce();
                            delete c;
                        }, app_ctx);
                    }
                    return;
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (ctx->app->_detail_modal) return;   // 弹窗期间手势全屏蔽
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_dir_t dir = lv_indev_get_gesture_dir(dev);
                if (dir == LV_DIR_TOP) {
                    // 只认「从屏幕底部往上拨」(起点在屏幕下缘 1/4); 其余位置
                    // 的上拨全部归列表滚动, 不退出 (与 RELEASED 路径同判据).
                    if (ctx->app->_touch_start.y <= 540) return;
                    // Defer: don't tear down _scr synchronously from inside indev dispatch.
                    auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestExitOnce();
                        delete c;
                    }, app_ctx);
                } else if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
                    // 左拨 → 指数看板, 右拨 → 自选股 (两页横向切换)
                    auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_setPage(c->app->_page == 0 ? 1 : 0);
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
    // 两页切换是 obj_set_pos 而非滚动容器, 横向拨动时抬起会落在卡片上并被
    // LVGL 当 CLICKED (2026-10-27 实锤: 拨到指数页必弹分时图). 用位移量排除:
    // 与 _installSwipeGesture 同判据 — 横向 >90px 且远大于纵向 = 翻页, 不是点击.
    if (lv_indev_t* indev = lv_indev_active()) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        const int dx = p.x - _touch_start.x, dy = p.y - _touch_start.y;
        if (abs(dx) > 90 && abs(dx) > abs(dy) * 2) return;
    }
    StockItem s;
    {
        std::lock_guard<std::mutex> lk(_items_mu);
        if (row_idx < 0 || row_idx >= (int)_items.size()) return;
        s = _items[row_idx];
    }
    // 个股 ifzq 代码: 6 开头 = 沪(sh), 其余(0/3) = 深(sz)
    char title[160];
    std::snprintf(title, sizeof(title), "%s  %s   %.2f  %+.2f%%",
                  s.name.c_str(), s.code.c_str(), s.price, s.chg);
    _openChart(title, (s.code.rfind("6", 0) == 0 ? "sh" : "sz") + s.code);
}

// 全屏走势图弹窗 (个股/指数共用): 顶部 64px = 标题 + 分时/日K tab + 关闭,
// 画布铺满其余部分. ifzq_code 由调用方映射好 (个股 sh/sz+6位; 指数原样或去 r_).
void AppStocks::_openChart(const std::string& title, const std::string& ifzq_code)
{
    LvglLockGuard lock;
    if (!_scr) return;
    _closeDetail();

    _detail_modal = lv_obj_create(_scr);
    lv_obj_set_size(_detail_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_detail_modal, 0, 0);
    lv_obj_set_style_bg_color(_detail_modal, lv_color_hex(C_HEADER()), 0);
    lv_obj_set_style_border_width(_detail_modal, 0, 0);
    lv_obj_set_style_radius(_detail_modal, 0, 0);
    lv_obj_set_style_pad_all(_detail_modal, 0, 0);
    lv_obj_clear_flag(_detail_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_lbl = lv_label_create(_detail_modal);
    lv_label_set_text(title_lbl, title.c_str());
    lv_obj_set_style_text_font(title_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 16, 14);

    // 分时 / 日K 切换 tab (CHECKABLE: 选中态 accent 底白字)
    const char* tab_names[2] = {"分时", "日K"};
    for (int i = 1; i >= 0; i--) {   // 右起建, 让出最右给关闭钮
        lv_obj_t* btn = lv_button_create(_detail_modal);
        lv_obj_set_size(btn, 150, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -174 - (1 - i) * 158, 10);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x33363A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::cur().accent), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM()), 0);            // 继承给 label
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppStocks*>(lv_event_get_user_data(e));
            lv_obj_t* tab = lv_event_get_current_target_obj(e);
            self->_onChartTab((int)(intptr_t)lv_obj_get_user_data(tab), tab);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_names[i]);
        lv_obj_set_style_text_font(lbl, zh_font_sm(), 0);
        lv_obj_center(lbl);
        _chart_tab_btn[i] = btn;
        if (i == _chart_mode) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    // 均线图例 (颜色与 _drawChartCanvas 的 ma_col 对应, 仅日K 视图显示)
    {
        const uint32_t leg_col[3] = {0xF1C40F, 0xE91E63, 0x3498DB};  // 黄/品红/蓝
        const char* leg_txt[3] = {"MA5", "MA10", "MA20"};
        int lx = 16;
        for (int k = 0; k < 3; k++) {
            lv_obj_t* leg = lv_label_create(_detail_modal);
            lv_label_set_text(leg, leg_txt[k]);
            lv_obj_set_style_text_font(leg, zh_font_sm(), 0);
            lv_obj_set_style_text_color(leg, lv_color_hex(leg_col[k]), 0);
            lv_obj_align(leg, LV_ALIGN_TOP_LEFT, lx, 68);
            lv_obj_add_flag(leg, LV_OBJ_FLAG_HIDDEN);   // 默认分时视图
            _chart_ma_leg[k] = leg;
            lx += 64;
        }
    }

    // canvas: 1280x656 = 全屏 - 顶部行; ARGB8888 (堆缓冲, worker 绘制在 UI 线程做)
    // 3.4MB 大块: 设备端 SPIRAM_USE_MALLOC=y, malloc 自动落 PSRAM;
    // 勿用 heap_caps_malloc(MALLOC_CAP_SPIRAM) —— SPIRAM_USE_CAPS_ALLOC
    // 未开时该 cap 分配必返回 NULL (2026-10-27 实锤: 分时图不显示根因).
    _chart_pixels = malloc(CHART_W * CHART_H * 4);
    if (_chart_pixels) {
        memset(_chart_pixels, 0, CHART_W * CHART_H * 4);
    } else {
        mclog::tagWarn(TAG, "chart canvas alloc failed (3.4MB)");
    }
    _chart_canvas = lv_canvas_create(_detail_modal);
    lv_canvas_set_buffer(_chart_canvas, _chart_pixels, CHART_W, CHART_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_align(_chart_canvas, LV_ALIGN_TOP_LEFT, 0, 64);

    _chart_status = lv_label_create(_detail_modal);
    lv_label_set_text(_chart_status, "行情加载中…");
    lv_obj_set_style_text_font(_chart_status, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_chart_status, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_chart_status, LV_ALIGN_TOP_MID, 0, 64 + CHART_H / 2 - 10);

    lv_obj_t* close_btn = lv_button_create(_detail_modal);
    lv_obj_set_size(close_btn, 150, 48);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -16, 10);   // 日K 右侧 (用户指定)
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
        _chart_code = ifzq_code;
    }
    _chart_ready.store(false);
    _trySpawnChartWorker(ifzq_code);
}

// 派生分时/K线 worker. 已有 worker 在飞则跳过 (worker 结束时会检查期间产生的新
// 请求并补拉); 派生失败必须还旗标, 否则 _chart_in_flight 永远 true, 之后所有
// 弹窗都卡「行情加载中」(2026-10-27 实锤: 旗标置位后从来没人复位).
void AppStocks::_trySpawnChartWorker(const std::string& code)
{
    if (_chart_in_flight.exchange(true)) return;
    std::shared_ptr<bool> alive = _alive;
    if (!GetHAL()->tryRunDetached([this, code, alive]() {
            _chartFetchWorker(code, alive);
        })) {
        _chart_in_flight.store(false);
    }
}

void AppStocks::_closeDetail()
{
    if (_detail_modal) {
        lv_obj_delete(_detail_modal);
        _detail_modal = nullptr;
    }
    _chart_canvas = _chart_status = nullptr;
    for (lv_obj_t*& b : _chart_tab_btn) b = nullptr;
    for (lv_obj_t*& l : _chart_ma_leg) l = nullptr;
    if (_chart_pixels) {           // canvas 缓冲是堆分配, 弹窗关了就还
        free(_chart_pixels);
        _chart_pixels = nullptr;
    }
    _chart_ready.store(false);
    {   // 清当前请求: 在飞的 worker 结束时据此把结果作废、不再补拉
        std::lock_guard<std::mutex> lk(_chart_mu);
        _chart_code.clear();
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  分时图 / K线图 (腾讯行情接口, 免 key; 2026-10-27)
//  worker 只填 _chart + 置 _chart_ready, 绘制在 UI 线程 (_applyCharts)。
// ════════════════════════════════════════════════════════════════════════════
void AppStocks::_chartFetchWorker(std::string code, std::shared_ptr<bool> alive)
{
    // code 即 ifzq 代码, 调用方已映射: 个股 sh/sz+6位; 指数 A 股原样、
    // r_hkHSI→hkHSI (去 r_)、usDJI/usIXIC/usINX 原样
    const std::string& prefixed = code;

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

    // ── 日K: fqkline/get, 最近 120 根前复权 (2026-10-27 用户 60→120) ──
    {
        auto resp = GetHAL()->httpGet(
            "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=" +
            prefixed + ",day,,,120,qfq");
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
    mclog::tagInfo(TAG, "chart fetched: minute={} kline={} ok={}",
                   local.minute_prices.size(), local.day_close.size(), local.ok);
    // 只把「仍被请求」的结果交给 UI: 在飞期间用户换了股票 (_chart_code 变了)
    // 或已关弹窗 (_chart_code 清空) 时, 本次结果作废; 若有新请求则补拉.
    bool current;
    std::string pending;
    {
        std::lock_guard<std::mutex> lk(_chart_mu);
        current = (_chart_code == code);
        pending = current ? std::string() : _chart_code;
        if (current) _chart = std::move(local);
    }
    _chart_in_flight.store(false);
    if (!pending.empty()) {
        _trySpawnChartWorker(pending);   // 在飞时点开的新股票, 现在补拉
    } else if (current) {
        _chart_ready.store(true);        // onRunning 在 UI 线程消费
    }
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
    _drawChartCanvas(c);
}

// 无锁绘制 (调用方须已在 UI 线程持有 LVGL 锁; _onChartTab 从点击回调直接调,
// LvglLockGuard 走 lvgl_port_lock 非递归, 重入必死锁). 按 _chart_mode 选
// 分时/日K, 请求的序列缺数据时回退到另一序列 (worker ok=true 保证非空).
void AppStocks::_drawChartCanvas(const ChartData& c)
{
    if (!_chart_canvas || !_chart_pixels) return;

    constexpr int W = CHART_W, H = CHART_H;
    void* pix = _chart_pixels;
    const uint32_t COL_BG    = 0x0A1610;   // 图区底
    const uint32_t COL_GRID  = 0x24402F;
    const uint32_t COL_UP    = 0xE74C3C;   // A 股: 涨红
    const uint32_t COL_DOWN  = 0x2ECC71;   // 跌绿
    const uint32_t COL_AXIS  = 0x6E9486;

    const bool have_min = !c.minute_prices.empty();
    const bool have_day = !c.day_close.empty();
    bool draw_minute = have_min;
    if (_chart_mode == 1 && have_day) draw_minute = false;
    if (_chart_mode == 0 && !have_min && have_day) draw_minute = false;

    // 均线图例仅日K视图显示
    for (lv_obj_t* leg : _chart_ma_leg) {
        if (!leg) continue;
        if (draw_minute) lv_obj_add_flag(leg, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(leg, LV_OBJ_FLAG_HIDDEN);
    }

    // ── 分时整幅 / 日K整幅, 由 tab 切换 (_chart_mode), 缺数据回退另一序列 ──
    px_fill(pix, W, H, COL_BG);

    if (draw_minute) {
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
    } else if (have_day) {
        // 日K: 最近 120 根蜡烛 + MA5/10/20 均线
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
        // MA5/10/20 均线 (收盘价简单均值, 滑窗; 前 p-1 根无值不画).
        // 收盘价必然落在 [lo,hi] 内 (high/low 包络), 无越界.
        const uint32_t ma_col[3] = {0xF1C40F, 0xE91E63, 0x3498DB};  // 黄/品红/蓝
        const int ma_period[3] = {5, 10, 20};
        for (int k = 0; k < 3; k++) {
            const int p = ma_period[k];
            if (n < p) continue;
            float sum = 0;
            for (int i = 0; i < p; i++) sum += c.day_close[i];
            int mx = -1, my = -1;
            for (int i = p - 1; i < n; i++) {
                if (i > p - 1) sum += c.day_close[i] - c.day_close[i - p];
                const int x = L + step * i + step / 2;
                const int y = y_of(sum / (float)p);
                if (mx >= 0) px_line(pix, W, H, mx, my, x, y, ma_col[k]);
                mx = x; my = y;
            }
        }
    }

    lv_obj_invalidate(_chart_canvas);
}

// 分时/日K tab 点击 (UI 线程, 已持 LVGL 锁): 换模式 + 同步两钮选中态 + 重绘.
// CHECKABLE 会把点中的钮自动翻转, 点已选中的要把状态翻回选中.
void AppStocks::_onChartTab(int mode, lv_obj_t* btn)
{
    if (!btn) return;
    if (mode == _chart_mode) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        return;
    }
    _chart_mode = mode;
    for (lv_obj_t* b : _chart_tab_btn)
        if (b) lv_obj_clear_state(b, LV_STATE_CHECKED);
    lv_obj_add_state(btn, LV_STATE_CHECKED);
    ChartData c;
    {
        std::lock_guard<std::mutex> lk(_chart_mu);
        c = _chart;
    }
    _drawChartCanvas(c);
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

    // 卡位数 = min(持仓数, MAX_CARDS), 按需创建 (2026-10-27: 超 12 只
    // 全部显示, 滚动容器纵向扩展)
    const int n = std::min((int)snapshot.size(), MAX_CARDS);
    if (n <= 0) return;
    _ensureCards(n);
    _n_cards = n;

    // 按涨跌幅降序; 卡片从左到右、从上到下依次绑定
    std::vector<int> order(snapshot.size());
    for (int i = 0; i < (int)order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return snapshot[a].chg > snapshot[b].chg; });

    // 渐变端点: 可见卡中的最大涨幅 / 最大跌幅
    float max_chg = 0, min_chg = 0;
    for (int i = 0; i < (int)order.size() && i < n; i++) {
        max_chg = snapshot[order[i]].chg > max_chg ? snapshot[order[i]].chg : max_chg;
        min_chg = snapshot[order[i]].chg < min_chg ? snapshot[order[i]].chg : min_chg;
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t* card = _cards[i];
        lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
        const auto& st = snapshot[order[i]];
        lv_obj_set_user_data(card, (void*)(intptr_t)order[i]);  // 点击 → 原始下标

        // 中国 A 股惯例: 涨=红 跌=绿, 彩底白字.
        // 颜色随涨跌幅度渐变 (2026-10-27 用户指定, 幅度方向与首版相反):
        // 涨幅最大的卡用暗红, 涨幅最小的卡用满红; 跌方同理 (满绿给跌得最少的).
        // 渐变只在红/绿色系内部进行 (浅端 = 满红/满绿, 深端 = 暗红/暗绿).
        const bool up   = st.chg > 0.01f;
        const bool down = st.chg < -0.01f;
        constexpr uint32_t UP_DARK   = 0x8A2B20;  // 渐变深端: 暗红 (给涨得最多的)
        constexpr uint32_t DOWN_DARK = 0x1F6B4E;  // 渐变深端: 暗绿 (给跌得最多的)
        uint32_t bg = C_HEADER();
        if (up || down) {
            const float max_up   = max_chg > 0 ? max_chg : 0;
            const float max_down = min_chg < 0 ? -min_chg : 0;
            const float t = up ? (max_up > 0 ? 1 - st.chg / max_up : 1)
                               : (max_down > 0 ? 1 - (-st.chg / max_down) : 1);
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
