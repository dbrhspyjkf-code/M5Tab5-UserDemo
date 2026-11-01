#include "app_lottery.h"

#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include "../utils/ui/canvas_draw.h"
#include "../utils/font5x7.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include "../utils/assets/assets_partition.h"
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
// 30px 全字库 (标题/按钮)
static const lv_font_t* zh30()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
// 20px res 分区 (小字/球号)
static const lv_font_t* zh20()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh30() { return &font_puhui_20_4; }
static const lv_font_t* zh20() { return &font_puhui_20_4; }
#endif

static const char* TAG = "lottery";

// 单调毫秒 (两平台通用, app_stocks 同款)
static int64_t mono_ms()
{
#ifdef PLATFORM_BUILD_DESKTOP
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    return (int64_t)(esp_timer_get_time() / 1000);
#endif
}

// ── 金额格式化: >=1e8 → "X.XX 亿"; >=1e4 → "X.XX 万"; 否则千分位 ──────────
static std::string fmt_yi(long v)
{
    char buf[32];
    if (v >= 100000000L) {
        snprintf(buf, sizeof(buf), "%.2f 亿", v / 1e8);
    } else if (v >= 10000L) {
        snprintf(buf, sizeof(buf), "%.2f 万", v / 1e4);
    } else {
        snprintf(buf, sizeof(buf), "%ld", v);
    }
    return buf;
}
// 千分位整数 (奖金明细)
static std::string fmt_thousands(long v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", v);
    std::string s = buf;
    for (int pos = (int)s.size() - 3; pos > 0; pos -= 3) s.insert(pos, ",");
    return s;
}

// ── 5x7 点阵绘制 (canvas 上画遗漏数字/期号/柱值; label 无法量产) ──────────
static inline void pix_px(void* pix, int W, int H, int x, int y, uint32_t col)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    ((uint32_t*)pix)[y * W + x] = col | 0xFF000000u;
}

// (x,y) = 字形左上角
static void pix_glyph(void* pix, int W, int H, int x, int y, char ch,
                      uint32_t col, int scale = 1)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < 0x20 || ch > 0x7E) return;
    const uint8_t* rows = font5x7[ch - 0x20];
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++)
            if (rows[r] & (1 << (4 - c)))
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        pix_px(pix, W, H, x + c * scale + sx, y + r * scale + sy, col);
}

// 数字/字符串按 5x7 点阵以 (cx,cy) 为中心绘制 (字距 1px×scale)
static void pix_text_c(void* pix, int W, int H, int cx, int cy,
                       const std::string& s, uint32_t col, int scale = 1)
{
    const int gw = (int)s.size() * 6 * scale - scale;
    const int gh = 7 * scale;
    int x = cx - gw / 2;
    const int y = cy - gh / 2;
    for (char ch : s) {
        pix_glyph(pix, W, H, x, y, ch, col, scale);
        x += 6 * scale;
    }
}
static void pix_num_c(void* pix, int W, int H, int cx, int cy, int val,
                      uint32_t col, int scale = 1)
{
    pix_text_c(pix, W, H, cx, cy, std::to_string(val), col, scale);
}

// 实心圆 (逐像素判定, 半径 ≤13 级别足够快)
static void pix_circle(void* pix, int W, int H, int cx, int cy, int r, uint32_t col)
{
    const int r2 = r * r;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r2) pix_px(pix, W, H, cx + dx, cy + dy, col);
}

// 点虚线横线 (每 6px 画 3px)
static void pix_dash_line(void* pix, int W, int H, int x0, int x1, int y, uint32_t col)
{
    for (int x = x0; x <= x1; x += 6)
        for (int k = 0; k < 3 && x + k <= x1; k++) pix_px(pix, W, H, x + k, y, col);
}

// ════════════════════════════════════════════════════════════════════════════
//  生命周期
// ════════════════════════════════════════════════════════════════════════════
AppLottery::AppLottery()
{
    setAppInfo().name = "双色球";
}

void AppLottery::onOpen()
{
    mclog::tagInfo(TAG, "opened");
    if (_scr) _destroyUi();   // 幂等: 清掉上次遗留 UI
    // 手势 alive 旗标成对铁律 (b275e71): onClose 置 false 后 onOpen 必须重建,
    // 否则第二次进入上拨静默失效。
    _alive = std::make_shared<bool>(true);
    _buildUi();
    lv_screen_load(_scr);   // 必须: 不 load 则活动屏还是旧屏, onRunning 会误判失焦
    _installSwipeGesture();
    _fetchAsync();   // 首次拉取

    // 桌面调试: TAB5_LOTTERY_TAB=0..3 直开指定 tab; TAB5_LOTTERY_PICK=
    // "3,11,17,22,23,29|9" 预填选号并自动判定 (SDL 合成点击不可靠, 同
    // TAB5_HEALTH_DETAIL 惯例)
#ifdef PLATFORM_BUILD_DESKTOP
    if (const char* t = getenv("TAB5_LOTTERY_TAB")) {
        int tab = atoi(t);
        if (tab >= 0 && tab <= 3) _setTab(tab);
    }
    if (const char* p = getenv("TAB5_LOTTERY_PICK")) {
        std::string s = p;
        const size_t bar = s.find('|');
        // "r,r,r,r,r,r|b" → 红球集 | 蓝球集
        size_t st = 0;
        bool blue_part = false;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == ',' || s[i] == '|') {
                if (i > st) {
                    int v = atoi(s.substr(st, i - st).c_str());
                    if (blue_part) {
                        if (v >= 1 && v <= 16) _pick_blue[v - 1] = true;
                    } else {
                        if (v >= 1 && v <= 33) _pick_red[v - 1] = true;
                    }
                }
                st = i + 1;
                if (i < s.size() && s[i] == '|') blue_part = true;
            }
        }
        _updatePickUi();
        if (bar != std::string::npos) _pick_autorun = true;   // 数据到位后自动判定
    }
#endif
}

void AppLottery::onRunning()
{
    // 失焦收尾 (mooncake openApp 其他 app 不触发 onClose, 自己检测, health 同款;
    // async 删屏防「删除活动屏」崩溃, app_calendar 模式)
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
        _removeSwipeGesture();
        if (_trend_pixels) {
            free(_trend_pixels);
            _trend_pixels = nullptr;
        }
        if (_stat_pixels) {
            free(_stat_pixels);
            _stat_pixels = nullptr;
        }
        lv_obj_t* scr = _scr;
        _nullUiPtrs();
        _scr = nullptr;
        lv_async_call([](void* p) { lv_obj_delete(static_cast<lv_obj_t*>(p)); }, scr);
        return;
    }
    if (!_scr) return;

    if (_data_ready.exchange(false)) _applyData();

    // toast (拍照识别占位等短提示), 3s 自清
    if (_toast_lbl) {
        if (_toast.empty()) {
            lv_obj_add_flag(_toast_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(_toast_lbl, _toast.c_str());
            lv_obj_clear_flag(_toast_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!_toast.empty()) {
        if (_toast_start == 0) _toast_start = mono_ms();
        else if (mono_ms() - _toast_start > 3000) {
            _toast.clear();
            _toast_start = 0;
        }
    }

    // 60 分钟重拉 (开奖一天一次, 服务端常驻缓存, 兜底跨天场景)
    if (_last_fetch_ms != 0 && mono_ms() - _last_fetch_ms >= 3600ULL * 1000) {
        _fetchAsync();
    }
}

void AppLottery::onClose()
{
    mclog::tagInfo(TAG, "closed");
    _destroyUi();
    _alive = std::make_shared<bool>(false);   // 作废在途 worker
}

void AppLottery::_requestExitOnce()
{
    int64_t now = mono_ms();
    if (_last_exit_ms != 0 && now - _last_exit_ms < 500) return;   // 双轨去重
    _last_exit_ms = now;
    _requestClose();
}

// ════════════════════════════════════════════════════════════════════════════
//  数据 (worker 只写数据 + 原子旗标; LVGL 全在 UI 线程)
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_fetchAsync()
{
    bool expected = false;
    if (!_fetch_in_flight.compare_exchange_strong(expected, true)) return;

    _last_fetch_ms = mono_ms();
    auto alive = _alive;
    bool ok = GetHAL()->tryRunDetached([this, alive]() {
        auto* hal = GetHAL();
        std::string base = "http://" + hal->getConfig("svc_host", "192.168.1.200") + ":8766";
        auto resp = hal->httpGet(base + "/api/ssq/history");
        {
            std::lock_guard<std::mutex> lk(_mu);
            _data_ok = false;
            _data_err.clear();
            if (!alive || !*alive) {
                _fetch_in_flight.store(false);
                return;
            }
            if (resp.status != 200 || resp.body.empty()) {
                _data_err = "HTTP " + std::to_string(resp.status);
            } else {
                try {
                    auto j = nlohmann::json::parse(resp.body);
                    if (!j.value("ok", false)) {
                        _data_err = j.value("error", "服务端无数据");
                    } else {
                        std::vector<ssq::DrawIssue> issues;
                        for (const auto& it : j["issues"]) {
                            ssq::DrawIssue d;
                            d.code = it.value("code", "");
                            d.date = it.value("date", "");
                            d.week = it.value("week", "");
                            d.blue = it.value("blue", 0);
                            d.sales = it.value("sales", 0L);
                            d.pool = it.value("pool", 0L);
                            const auto ra = it["reds"];
                            int n = 0;
                            if (ra.is_array())
                                for (size_t k = 0; k < ra.size() && n < 6; k++)
                                    d.reds[n++] = ra[k].get<int>();
                            for (const auto& g : it.value("grades", nlohmann::json::array())) {
                                d.grades.push_back({g.value("level", 0),
                                                    g.value("money", 0L),
                                                    g.value("num", 0L)});
                            }
                            if (n == 6 && d.blue >= 1 && d.blue <= 16 && !d.code.empty())
                                issues.push_back(std::move(d));
                        }
                        if (issues.empty()) {
                            _data_err = "期数据为空";
                        } else {
                            _issues = std::move(issues);
                            _recomputeTables();
                            _data_ok = true;
                        }
                    }
                } catch (const std::exception& e) {
                    _data_err = std::string("解析失败: ") + e.what();
                }
            }
        }
        _fetch_in_flight.store(false);
        _data_ready.store(true);
    }, 16 * 1024);
    if (!ok) {
        _fetch_in_flight.store(false);
        std::lock_guard<std::mutex> lk(_mu);
        _data_err = "worker 启动失败";
        _data_ready.store(true);
    }
}

// 遗漏/频次预计算 (调用方已持 _mu)
void AppLottery::_recomputeTables()
{
    const int N = (int)_issues.size();
    const int cap = std::min(N, 100);   // 服务端上限 100 期
    int last_red[33], last_blue[16];
    for (int k = 0; k < 33; k++) last_red[k] = -1;
    for (int k = 0; k < 16; k++) last_blue[k] = -1;
    // issues 倒序 (0=最新): i 期的遗漏 = i - 上次出现期下标 - 1; 该期出现则遗漏 0 (画球)
    for (int i = cap - 1; i >= 0; i--) {   // 从最旧往最新扫
        const auto& d = _issues[i];
        for (int j = 0; j < 6; j++) {
            int k = d.reds[j] - 1;
            if (k < 0 || k >= 33) continue;
            for (int t = last_red[k] + 1; t <= i; t++) _omit_red[t][k] = i - t;
            last_red[k] = i;
        }
        int kb = d.blue - 1;
        if (kb >= 0 && kb < 16) {
            for (int t = last_blue[kb] + 1; t <= i; t++) _omit_blue[t][kb] = i - t;
            last_blue[kb] = i;
        }
    }
    // 比最旧期更早的遗漏: 用期下标低位估计 (100 期外不可知, 显示值 ≥ 实际遗漏下界)
    for (int k = 0; k < 33; k++)
        for (int t = last_red[k] + 1; t < cap; t++) _omit_red[t][k] = t + 1;
    for (int k = 0; k < 16; k++)
        for (int t = last_blue[k] + 1; t < cap; t++) _omit_blue[t][k] = t + 1;
    // cap 之外 (理论不可达) 清零防越界读
    for (int i = cap; i < 100; i++) {
        memset(_omit_red[i], 0, sizeof(_omit_red[i]));
        memset(_omit_blue[i], 0, sizeof(_omit_blue[i]));
    }
}

// 数据到位 → 填 UI (UI 线程)
void AppLottery::_applyData()
{
    if (!_scr) return;
    bool ok;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(_mu);
        ok = _data_ok;
        err = _data_err;
    }
    if (ok) {
        _updateDrawPage();
        if (_tab == 1) _drawTrend();
        if (_tab == 2) _drawStat();
        _updatePickUi();
        if (_pick_autorun) {
            _pick_autorun = false;
            _judge();
        }
    } else if (_draw_status) {
        std::string s = "数据加载失败: " + err;
        lv_label_set_text(_draw_status, s.c_str());
        lv_obj_clear_flag(_draw_status, LV_OBJ_FLAG_HIDDEN);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  UI 骨架
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG()), 0);

    // 顶部 tab 栏: 4 个 CHECKABLE 按钮 (stocks 分时/日K 同款)
    const char* names[4] = {"开奖", "走势", "统计", "对奖"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = lv_button_create(_scr);
        lv_obj_set_size(btn, 180, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 40 + i * 196, 8);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACC()), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM()), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            self->_setTab((int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, names[i]);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_center(l);
        _tab_btns[i] = btn;
    }

    // 4 个全宽页面容器 (y=64 起)
    for (int i = 0; i < 4; i++) {
        lv_obj_t* pg = lv_obj_create(_scr);
        lv_obj_set_size(pg, SCREEN_W, SCREEN_H - TAB_H);
        lv_obj_set_pos(pg, 0, TAB_H);
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        _pages[i] = pg;
    }

    _buildDrawPage();
    _buildTrendPage();
    _buildStatPage();
    _buildPickPage();

    // toast (底部居中)
    _toast_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_toast_lbl, zh30(), 0);
    lv_obj_set_style_text_color(_toast_lbl, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(_toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(_toast_lbl, LV_OBJ_FLAG_HIDDEN);

    _setTab(0);
}

// 全部 UI 指针置空 (除 _scr; 屏对象由调用方决定同步/async 删除)
void AppLottery::_nullUiPtrs()
{
    for (auto& b : _tab_btns) b = nullptr;
    for (auto& p : _pages) p = nullptr;
    _draw_head = _draw_list = _draw_status = nullptr;
    _trend_canvas = _trend_pg_lbl = _trend_prev = _trend_next = nullptr;
    _trend_status = nullptr;
    _trend_btns[0] = _trend_btns[1] = nullptr;
    _stat_canvas = nullptr;
    _stat_status = _stat_title_r = _stat_title_b = nullptr;
    for (auto& b : _stat_btns) b = nullptr;
    for (auto& b : _pick_red_btns) b = nullptr;
    for (auto& b : _pick_blue_btns) b = nullptr;
    _target_lbl = _target_draws = _pick_summary = _judge_btn = _photo_btn = nullptr;
    _result_modal = nullptr;
    _toast_lbl = nullptr;
}

void AppLottery::_destroyUi()
{
    _removeSwipeGesture();
    if (_trend_pixels) {
        free(_trend_pixels);
        _trend_pixels = nullptr;
    }
    if (_stat_pixels) {
        free(_stat_pixels);
        _stat_pixels = nullptr;
    }
    lv_obj_t* scr = _scr;
    _nullUiPtrs();
    _scr = nullptr;
    if (scr) lv_obj_delete(scr);
}

void AppLottery::_setTab(int tab)
{
    _tab = tab;
    for (int i = 0; i < 4; i++) {
        if (i == tab) lv_obj_add_state(_tab_btns[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(_tab_btns[i], LV_STATE_CHECKED);
        lv_obj_add_flag(_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(_pages[tab], LV_OBJ_FLAG_HIDDEN);

    // canvas 像素按需分配/释放 (3MB×2 太贵, 离开 tab 即还; buffer 在分配时
    // 就地绑定到 canvas, 绘制函数不再管绑定)
    if (tab == 1 && !_trend_pixels) {
        _trend_pixels = malloc(CH_W * CH_H * 4);
        if (_trend_pixels) {
            canvas_draw::px_fill(_trend_pixels, CH_W, CH_H, C_BG());
            lv_canvas_set_buffer(_trend_canvas, _trend_pixels, CH_W, CH_H,
                                 LV_COLOR_FORMAT_ARGB8888);
            if (_data_ok) _drawTrend();
        }
    }
    if (tab != 1 && _trend_pixels) {
        free(_trend_pixels);
        _trend_pixels = nullptr;
    }
    if (tab == 2 && !_stat_pixels) {
        _stat_pixels = malloc(CH_W * CH_H * 4);
        if (_stat_pixels) {
            canvas_draw::px_fill(_stat_pixels, CH_W, CH_H, C_BG());
            lv_canvas_set_buffer(_stat_canvas, _stat_pixels, CH_W, CH_H,
                                 LV_COLOR_FORMAT_ARGB8888);
            if (_data_ok) _drawStat();
        }
    }
    if (tab != 2 && _stat_pixels) {
        free(_stat_pixels);
        _stat_pixels = nullptr;
    }
}

// 圆形球控件 (红/蓝底白字号码)
lv_obj_t* AppLottery::_makeBall(lv_obj_t* parent, int num, bool blue,
                                int size, const lv_font_t* font)
{
    lv_obj_t* ball = lv_obj_create(parent);
    lv_obj_set_size(ball, size, size);
    lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ball, lv_color_hex(blue ? C_BLUE() : C_RED()), 0);
    lv_obj_set_style_border_width(ball, 0, 0);
    lv_obj_set_style_pad_all(ball, 0, 0);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d", num);
    lv_obj_t* l = lv_label_create(ball);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return ball;
}

// ════════════════════════════════════════════════════════════════════════════
//  tab1 开奖
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildDrawPage()
{
    lv_obj_t* pg = _pages[0];

    _draw_status = lv_label_create(pg);
    lv_label_set_text(_draw_status, "数据加载中…");
    lv_obj_set_style_text_font(_draw_status, zh30(), 0);
    lv_obj_set_style_text_color(_draw_status, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_draw_status, LV_ALIGN_TOP_MID, 0, 120);

    // 最新期卡片与列表均由 _updateDrawPage 整卡重建 (数据到位后)
}

void AppLottery::_updateDrawPage()
{
    if (!_scr) return;
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }
    if (issues.empty() || !_pages[0]) return;
    const ssq::DrawIssue& lt = issues[0];

    if (_draw_status) lv_obj_add_flag(_draw_status, LV_OBJ_FLAG_HIDDEN);

    // ── 最新期卡片 (整卡重建) ──
    if (_draw_head) lv_obj_delete(_draw_head);
    _draw_head = lv_obj_create(_pages[0]);
    lv_obj_set_size(_draw_head, SCREEN_W, 244);
    lv_obj_set_pos(_draw_head, 0, 0);
    lv_obj_set_style_bg_color(_draw_head, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_radius(_draw_head, 20, 0);
    lv_obj_set_style_border_width(_draw_head, 0, 0);
    lv_obj_set_style_pad_all(_draw_head, 0, 0);
    lv_obj_clear_flag(_draw_head, LV_OBJ_FLAG_SCROLLABLE);

    char buf[96];
    snprintf(buf, sizeof(buf), "%s 期 · %s 周%s", lt.code.c_str(),
             lt.date.c_str(), lt.week.c_str());
    lv_obj_t* title = lv_label_create(_draw_head);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 16);

    // 奖池 / 销售额 (右上)
    snprintf(buf, sizeof(buf), "奖池 %s   销售额 %s", fmt_yi(lt.pool).c_str(),
             fmt_yi(lt.sales).c_str());
    lv_obj_t* pool = lv_label_create(_draw_head);
    lv_label_set_text(pool, buf);
    lv_obj_set_style_text_font(pool, zh20(), 0);
    lv_obj_set_style_text_color(pool, lv_color_hex(C_DIM()), 0);
    lv_obj_align(pool, LV_ALIGN_TOP_RIGHT, -28, 20);

    // 球行: 6 红 + 1 蓝, 68px 球 (数字 zh30)
    {
        const int bs = 68, gap = 18;
        const int total = 7 * bs + 6 * gap;
        const int x0 = (SCREEN_W - total) / 2;
        for (int i = 0; i < 6; i++) {
            lv_obj_t* b = _makeBall(_draw_head, lt.reds[i], false, bs, zh30());
            lv_obj_set_pos(b, x0 + i * (bs + gap), 66);
        }
        lv_obj_t* b = _makeBall(_draw_head, lt.blue, true, bs, zh30());
        lv_obj_set_pos(b, x0 + 6 * (bs + gap), 66);
    }

    // 一二等奖行 (浮动奖金 + 注数)
    long m1 = 0, n1 = 0, m2 = 0, n2 = 0;
    for (const auto& g : lt.grades) {
        if (g.level == 1) { m1 = g.money; n1 = g.num; }
        if (g.level == 2) { m2 = g.money; n2 = g.num; }
    }
    snprintf(buf, sizeof(buf), "一等奖 %s × %ld 注    二等奖 %s × %ld 注",
             m1 > 0 ? fmt_yi(m1).c_str() : "浮动", n1,
             m2 > 0 ? fmt_yi(m2).c_str() : "浮动", n2);
    lv_obj_t* g12 = lv_label_create(_draw_head);
    lv_label_set_text(g12, buf);
    lv_obj_set_style_text_font(g12, zh20(), 0);
    lv_obj_set_style_text_color(g12, lv_color_hex(C_DIM()), 0);
    lv_obj_align(g12, LV_ALIGN_BOTTOM_MID, 0, -14);

    // ── 近 10 期列表 ──
    if (_draw_list) lv_obj_delete(_draw_list);
    _draw_list = lv_obj_create(_pages[0]);
    lv_obj_set_size(_draw_list, SCREEN_W, SCREEN_H - TAB_H - 250);
    lv_obj_set_pos(_draw_list, 0, 250);
    lv_obj_set_style_bg_opa(_draw_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_draw_list, 0, 0);
    lv_obj_set_style_pad_all(_draw_list, 0, 0);
    lv_obj_set_style_pad_bottom(_draw_list, 60, 0);   // 底缘留给上拨退出带
    lv_obj_update_layout(_draw_list);

    const int rows = std::min((int)issues.size() - 1, 10);
    for (int r = 1; r <= rows; r++) {
        const ssq::DrawIssue& d = issues[r];
        lv_obj_t* row = lv_obj_create(_draw_list);
        lv_obj_set_size(row, SCREEN_W - 32, 52);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, (r - 1) * 58);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD()), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        snprintf(buf, sizeof(buf), "%s 期", d.code.c_str());
        lv_obj_t* c = lv_label_create(row);
        lv_label_set_text(c, buf);
        lv_obj_set_style_text_font(c, zh20(), 0);
        lv_obj_set_style_text_color(c, lv_color_hex(C_DIM()), 0);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, 16, 0);

        snprintf(buf, sizeof(buf), "%s 周%s", d.date.c_str(), d.week.c_str());
        lv_obj_t* dt = lv_label_create(row);
        lv_label_set_text(dt, buf);
        lv_obj_set_style_text_font(dt, zh20(), 0);
        lv_obj_set_style_text_color(dt, lv_color_hex(C_DIM()), 0);
        lv_obj_align(dt, LV_ALIGN_LEFT_MID, 150, 0);

        // 小球 36px (数字 zh20)
        for (int i = 0; i < 6; i++) {
            lv_obj_t* b = _makeBall(row, d.reds[i], false, 36, zh20());
            lv_obj_set_pos(b, 360 + i * 44, 8);
        }
        lv_obj_t* bb = _makeBall(row, d.blue, true, 36, zh20());
        lv_obj_set_pos(bb, 360 + 6 * 44, 8);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  tab2 走势 (canvas 遗漏打点图)
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildTrendPage()
{
    lv_obj_t* pg = _pages[1];

    // 工具行: 红球/蓝球子切换 (左) + ◀ 页码 ▶ (右)
    const char* sub[2] = {"红球", "蓝球"};
    for (int i = 1; i >= 0; i--) {
        lv_obj_t* btn = lv_button_create(pg);
        lv_obj_set_size(btn, 120, 44);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 16 + i * 130, 4);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(i == 0 ? C_RED() : C_BLUE()),
                                  LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM()), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            self->_trend_color = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            self->_trend_pg = 0;
            self->_drawTrend();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, sub[i]);
        lv_obj_set_style_text_font(l, zh20(), 0);
        lv_obj_center(l);
        _trend_btns[i] = btn;
        if (i == _trend_color) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    auto mk_pg = [&](const char* txt, int dx, int delta) {
        lv_obj_t* b = lv_button_create(pg);
        lv_obj_set_size(b, 88, 44);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, dx, 4);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_user_data(b, (void*)(intptr_t)delta);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            int delta = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            self->_trend_pg = std::max(0, self->_trend_pg + delta);
            self->_drawTrend();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_center(l);
        return b;
    };
    _trend_next = mk_pg("下页", -16, +1);   // 右 = 翻更旧页 (_trend_pg+1)
    _trend_prev = mk_pg("上页", -216, -1);  // 左 = 翻回更新页
    // 页码 label (◀ 与 ▶ 之间)
    _trend_pg_lbl = lv_label_create(pg);
    lv_obj_set_style_text_font(_trend_pg_lbl, zh20(), 0);
    lv_obj_set_style_text_color(_trend_pg_lbl, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_trend_pg_lbl, LV_ALIGN_TOP_RIGHT, -228, 16);
    lv_label_set_text(_trend_pg_lbl, "");

    _trend_canvas = lv_canvas_create(pg);
    lv_obj_align(_trend_canvas, LV_ALIGN_TOP_LEFT, 0, TOOL_H);
    // buffer 在 _setTab(1) 时挂 (按需分配)

    // 加载状态 (canvas 之后建 → 叠在上层; 数据到后隐藏)
    _trend_status = lv_label_create(pg);
    lv_label_set_text(_trend_status, "数据加载中…");
    lv_obj_set_style_text_font(_trend_status, zh30(), 0);
    lv_obj_set_style_text_color(_trend_status, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_trend_status, LV_ALIGN_TOP_MID, 0, TOOL_H + CH_H / 2 - 10);
}

void AppLottery::_drawTrend()
{
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }
    if (!_trend_pixels || issues.empty()) return;
    if (!_trend_canvas) return;
    if (_trend_btns[0]) {
        if (_trend_color == 0) lv_obj_add_state(_trend_btns[0], LV_STATE_CHECKED);
        else lv_obj_remove_state(_trend_btns[0], LV_STATE_CHECKED);
        if (_trend_color == 1) lv_obj_add_state(_trend_btns[1], LV_STATE_CHECKED);
        else lv_obj_remove_state(_trend_btns[1], LV_STATE_CHECKED);
    }

    const int N = (int)issues.size();
    const int max_pg = (N + TREND_COLS - 1) / TREND_COLS - 1;
    if (_trend_pg > max_pg) _trend_pg = max_pg;
    const int base = _trend_pg * TREND_COLS;   // 该页最旧期下标 = base
    const int cnt = std::min(TREND_COLS, N - base);

    void* pix = _trend_pixels;   // buffer 已由 _setTab 分配时绑定
    const uint32_t bg = C_BG(), card = C_CARD(), dim = C_DIM(), text = C_TEXT();
    canvas_draw::px_fill(pix, CH_W, CH_H, bg);

    const bool red_mode = (_trend_color == 0);
    const int rows = red_mode ? 33 : 16;
    const uint32_t ball_col = red_mode ? C_RED() : C_BLUE();

    // 布局: 顶部期号行 16px; 左号码列 38px; 列宽 41
    constexpr int HEAD_H = 16, NUM_W = 38, GX0 = NUM_W;
    const int avail_h = CH_H - HEAD_H;
    const int rh = red_mode ? avail_h / 33 : avail_h / 16;
    const int cw = (CH_W - GX0 - 4) / TREND_COLS;

    // 网格线: 每列 1px 极暗, 每 5 列亮一档; 行分隔线
    for (int c = 0; c <= cnt; c++) {
        const uint32_t lc = (c % 5 == 0) ? 0x2A2C30 : 0x1A1C20;
        canvas_draw::px_line(pix, CH_W, CH_H, GX0 + c * cw, HEAD_H,
                             GX0 + c * cw, HEAD_H + rows * rh, lc);
    }
    for (int r = 0; r <= rows; r++)
        canvas_draw::px_line(pix, CH_W, CH_H, GX0, HEAD_H + r * rh,
                             GX0 + cnt * cw, HEAD_H + r * rh, 0x1A1C20);

    // 左号码列 + 顶部期号行
    for (int r = 0; r < rows; r++)
        pix_num_c(pix, CH_W, CH_H, NUM_W / 2, HEAD_H + r * rh + rh / 2, r + 1, dim, 1);
    for (int c = 0; c < cnt; c += 5) {
        const ssq::DrawIssue& d = issues[base + (cnt - 1 - c)];   // 列 c (左旧右新)
        const std::string tail = d.code.size() >= 3 ? d.code.substr(d.code.size() - 3) : d.code;
        pix_text_c(pix, CH_W, CH_H, GX0 + c * cw + cw / 2, HEAD_H / 2, tail, dim, 1);
    }

    // 格子: 出号球 / 遗漏数字
    for (int r = 0; r < rows; r++) {
        const int cy = HEAD_H + r * rh + rh / 2;
        for (int c = 0; c < cnt; c++) {
            const int i = base + (cnt - 1 - c);
            const int cx = GX0 + c * cw + cw / 2;
            bool hit = false;
            if (red_mode) {
                for (int j = 0; j < 6; j++)
                    if (issues[i].reds[j] == r + 1) { hit = true; break; }
            } else {
                hit = (issues[i].blue == r + 1);
            }
            if (hit) {
                const int br = red_mode ? rh / 2 - 1 : std::min(rh / 2 - 2, 13);
                pix_circle(pix, CH_W, CH_H, cx, cy, br, ball_col);
                pix_num_c(pix, CH_W, CH_H, cx, cy, r + 1, 0xFFFFFF,
                          red_mode ? 1 : 2);
            } else {
                const int omit = red_mode ? _omit_red[i][r] : _omit_blue[i][r];
                pix_num_c(pix, CH_W, CH_H, cx, cy, omit, dim, 1);
            }
        }
    }

    if (_trend_status) lv_obj_add_flag(_trend_status, LV_OBJ_FLAG_HIDDEN);

    // 页码 label: "第 x/y 页 · 新2026124 … 旧2026095"
    if (_trend_pg_lbl) {
        char buf[80];
        snprintf(buf, sizeof(buf), "第 %d/%d 页 · %s … %s", _trend_pg + 1, max_pg + 1,
                 issues[base + cnt - 1].code.c_str(), issues[base].code.c_str());
        lv_label_set_text(_trend_pg_lbl, buf);
    }
    // 翻页按钮可用态 (到边界隐藏)
    if (_trend_prev) {
        if (_trend_pg <= 0) lv_obj_add_flag(_trend_prev, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(_trend_prev, LV_OBJ_FLAG_HIDDEN);
    }
    if (_trend_next) {
        if (_trend_pg >= max_pg) lv_obj_add_flag(_trend_next, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(_trend_next, LV_OBJ_FLAG_HIDDEN);
    }
    (void)text; (void)card;
}

// ════════════════════════════════════════════════════════════════════════════
//  tab3 统计 (canvas 冷热频次柱状图)
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildStatPage()
{
    lv_obj_t* pg = _pages[2];

    // 期数选择 (30/50/100)
    for (int i = 2; i >= 0; i--) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d 期", SPANS[i]);
        lv_obj_t* btn = lv_button_create(pg);
        lv_obj_set_size(btn, 110, 44);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 16 + i * 120, 4);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACC()), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM()), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            int idx = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            self->_stat_span = SPANS[idx];
            self->_drawStat();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_font(l, zh20(), 0);
        lv_obj_center(l);
        _stat_btns[i] = btn;
        if (SPANS[i] == _stat_span) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    _stat_canvas = lv_canvas_create(pg);
    lv_obj_align(_stat_canvas, LV_ALIGN_TOP_LEFT, 0, TOOL_H);

    // 中文标题 (canvas 点阵只有 ASCII, 标题用 label 叠加在 canvas 上层)
    _stat_title_r = lv_label_create(pg);
    lv_obj_set_style_text_font(_stat_title_r, zh20(), 0);
    lv_obj_set_style_text_color(_stat_title_r, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(_stat_title_r, LV_ALIGN_TOP_LEFT, 0, TOOL_H + 4);
    lv_label_set_text(_stat_title_r, "");
    _stat_title_b = lv_label_create(pg);
    lv_obj_set_style_text_font(_stat_title_b, zh20(), 0);
    lv_obj_set_style_text_color(_stat_title_b, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(_stat_title_b, LV_ALIGN_TOP_LEFT, 0, TOOL_H + 392);
    lv_label_set_text(_stat_title_b, "");

    _stat_status = lv_label_create(pg);
    lv_label_set_text(_stat_status, "数据加载中…");
    lv_obj_set_style_text_font(_stat_status, zh30(), 0);
    lv_obj_set_style_text_color(_stat_status, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_stat_status, LV_ALIGN_TOP_MID, 0, TOOL_H + CH_H / 2 - 10);
}

void AppLottery::_drawStat()
{
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }
    if (!_stat_pixels || issues.empty()) return;
    if (!_stat_canvas) return;

    void* pix = _stat_pixels;   // buffer 已由 _setTab 分配时绑定
    const uint32_t bg = C_BG(), dim = C_DIM(), text = C_TEXT();
    canvas_draw::px_fill(pix, CH_W, CH_H, bg);

    const int N = std::min((int)issues.size(), _stat_span);
    int freq_red[33] = {}, freq_blue[16] = {};
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 6; j++) {
            int k = issues[i].reds[j] - 1;
            if (k >= 0 && k < 33) freq_red[k]++;
        }
        int kb = issues[i].blue - 1;
        if (kb >= 0 && kb < 16) freq_blue[kb]++;
    }
    int max_r = 1, max_b = 1;
    for (int k = 0; k < 33; k++) max_r = std::max(max_r, freq_red[k]);
    for (int k = 0; k < 16; k++) max_b = std::max(max_b, freq_blue[k]);

    // ── 红球区: 标题 (label 叠加) + 绘图区 + 轴行 ──
    char buf[80];
    if (_stat_title_r) {
        snprintf(buf, sizeof(buf), "红球频次 · 近 %d 期 (虚线=均值 %.1f)",
                 N, (float)N * 6 / 33);
        lv_label_set_text(_stat_title_r, buf);
    }
    if (_stat_title_b) {
        snprintf(buf, sizeof(buf), "蓝球频次 · 近 %d 期 (虚线=均值 %.1f)",
                 N, (float)N / 16);
        lv_label_set_text(_stat_title_b, buf);
    }
    constexpr int R_TOP = 8, R_BOT = 360;   // 红球绘图区 (canvas 内 y)
    const int RX0 = 40, cw = (CH_W - RX0 - 12) / 33;
    const int bar_w = cw - 10;
    const float avg_r = (float)N * 6 / 33;
    const int avg_y_r = R_BOT - (int)(avg_r / max_r * (R_BOT - R_TOP - 10));
    pix_dash_line(pix, CH_W, CH_H, RX0, CH_W - 12, avg_y_r, 0x888888);
    for (int k = 0; k < 33; k++) {
        const int x0 = RX0 + k * cw + 5;
        const int h = freq_red[k] * (R_BOT - R_TOP - 10) / max_r;
        // 冷热: 暗红 → 亮红 (色系内部过渡, 自选股同款偏好)
        const uint32_t col = canvas_draw::px_blend(0x5A1E1E, 0xE53935,
                                                   (float)freq_red[k] / max_r);
        if (h > 0) canvas_draw::px_rect(pix, CH_W, CH_H, x0, R_BOT - h, x0 + bar_w, R_BOT, col);
        pix_num_c(pix, CH_W, CH_H, x0 + bar_w / 2, R_BOT - h - 7, freq_red[k],
                  freq_red[k] >= avg_r ? text : dim, 1);
        if ((k + 1) % 5 == 0 || k == 32)
            pix_num_c(pix, CH_W, CH_H, x0 + bar_w / 2, R_BOT + 10, k + 1, dim, 1);
    }

    // ── 蓝球区: y 400..570 + 轴 ──
    constexpr int B_TOP = 400, B_BOT = 560;
    const int bx0 = 40, bcw = (CH_W - bx0 - 12) / 16;
    const int bbar_w = bcw - 18;
    const float avg_b = (float)N / 16;
    const int avg_y_b = B_BOT - (int)(avg_b / max_b * (B_BOT - B_TOP - 10));
    pix_dash_line(pix, CH_W, CH_H, bx0, CH_W - 12, avg_y_b, 0x888888);
    for (int k = 0; k < 16; k++) {
        const int x0 = bx0 + k * bcw + 9;
        const int h = freq_blue[k] * (B_BOT - B_TOP - 10) / max_b;
        const uint32_t col = canvas_draw::px_blend(0x14243E, 0x1E88E5,
                                                   (float)freq_blue[k] / max_b);
        if (h > 0) canvas_draw::px_rect(pix, CH_W, CH_H, x0, B_BOT - h, x0 + bbar_w, B_BOT, col);
        pix_num_c(pix, CH_W, CH_H, x0 + bbar_w / 2, B_BOT - h - 7, freq_blue[k],
                  freq_blue[k] >= avg_b ? text : dim, 1);
        if ((k + 1) % 5 == 0 || k == 15)
            pix_num_c(pix, CH_W, CH_H, x0 + bbar_w / 2, B_BOT + 10, k + 1, dim, 1);
    }

    // 底部行 (蓝轴下): 期号范围 (点阵数字)
    if (!issues.empty()) {
        pix_text_c(pix, CH_W, CH_H, CH_W / 2, CH_H - 10,
                   "No." + issues[0].code + "-" + issues[N - 1].code, dim, 1);
    }

    if (_stat_status) lv_obj_add_flag(_stat_status, LV_OBJ_FLAG_HIDDEN);
    (void)buf; (void)bg;
}

// ════════════════════════════════════════════════════════════════════════════
//  tab4 对奖
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildPickPage()
{
    lv_obj_t* pg = _pages[3];

    // 顶部行: ◀ 对奖期 ▶ + 该期开奖号
    auto mk_shift = [&](const char* txt, int dx, int delta) {
        lv_obj_t* b = lv_button_create(pg);
        lv_obj_set_size(b, 76, 56);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, dx, 8);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_user_data(b, (void*)(intptr_t)delta);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            self->_targetShift((int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e)));
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_center(l);
    };
    mk_shift("前", 16, -1);    // 更新 (朝最新)
    mk_shift("后", 96 + 16 + 8, +1);   // 更旧

    _target_lbl = lv_label_create(pg);
    lv_obj_set_style_text_font(_target_lbl, zh30(), 0);
    lv_obj_set_style_text_color(_target_lbl, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(_target_lbl, LV_ALIGN_TOP_LEFT, 216, 20);
    lv_label_set_text(_target_lbl, "对奖期 --");

    _target_draws = lv_obj_create(pg);   // 开奖号小行容器 (整卡重建)
    lv_obj_set_size(_target_draws, 700, 72);
    lv_obj_align(_target_draws, LV_ALIGN_TOP_RIGHT, -20, 0);
    lv_obj_set_style_bg_opa(_target_draws, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_target_draws, 0, 0);
    lv_obj_set_style_pad_all(_target_draws, 0, 0);
    lv_obj_clear_flag(_target_draws, LV_OBJ_FLAG_SCROLLABLE);

    // 红球网格 11×3 (76x60, gap 8) → 宽 11*84-8=836, 居中
    for (int k = 0; k < 33; k++) {
        lv_obj_t* btn = lv_button_create(pg);
        const int col = k % 11, row = k / 11;
        lv_obj_set_size(btn, 76, 60);
        lv_obj_set_pos(btn, (SCREEN_W - 836) / 2 + col * 84, 104 + row * 68);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_RED()), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM()), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 30, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)k);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            int k = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            self->_pick_red[k] = !self->_pick_red[k];
            self->_updatePickUi();
        }, LV_EVENT_CLICKED, this);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d", k + 1);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_center(l);
        _pick_red_btns[k] = btn;
    }

    // 蓝球行 16 个 (60x52, gap 12) → 宽 16*72-12=1140
    for (int k = 0; k < 16; k++) {
        lv_obj_t* btn = lv_button_create(pg);
        lv_obj_set_size(btn, 60, 52);
        lv_obj_set_pos(btn, (SCREEN_W - 1140) / 2 + k * 72, 316);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_BLUE()), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_DIM()), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 26, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)k);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            int k = (int)(intptr_t)lv_obj_get_user_data(
                lv_event_get_current_target_obj(e));
            self->_pick_blue[k] = !self->_pick_blue[k];
            self->_updatePickUi();
        }, LV_EVENT_CLICKED, this);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d", k + 1);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_font(l, zh20(), 0);
        lv_obj_center(l);
        _pick_blue_btns[k] = btn;
    }

    // 底部操作行 y 400..470
    _pick_summary = lv_label_create(pg);
    lv_obj_set_style_text_font(_pick_summary, zh20(), 0);
    lv_obj_set_style_text_color(_pick_summary, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_pick_summary, LV_ALIGN_TOP_LEFT, 24, 416);
    lv_label_set_text(_pick_summary, "红 0 + 蓝 0");

    auto mk_action = [&](const char* txt, int w, int x, uint32_t col,
                         lv_event_cb_t cb, void* ud) {
        lv_obj_t* b = lv_button_create(pg);
        lv_obj_set_size(b, w, 60);
        lv_obj_set_pos(b, x, 408);
        lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_center(l);
        return b;
    };
    mk_action("清空", 130, 24, 0x2A2C30,
              [](lv_event_t* e) {
                  auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
                  memset(self->_pick_red, 0, sizeof(self->_pick_red));
                  memset(self->_pick_blue, 0, sizeof(self->_pick_blue));
                  self->_updatePickUi();
              }, this);
    _photo_btn = mk_action("拍照识别", 220, 178, 0x2A2C30,
                           [](lv_event_t* e) {
                               auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
                               self->_toast = "拍照识别即将上线 (OCR 链路部署中)";
                           }, this);
    _judge_btn = mk_action("判  定", 260, 990, C_ACC(),
                           [](lv_event_t* e) {
                               static_cast<AppLottery*>(lv_event_get_user_data(e))->_judge();
                           }, this);
    _updatePickUi();
}

void AppLottery::_targetShift(int d)
{
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }
    if (issues.empty()) return;
    const int max_idx = (int)issues.size() - 1;
    _target_idx = std::min(max_idx, std::max(0, _target_idx + d));
    _updatePickUi();
}

void AppLottery::_updatePickUi()
{
    if (!_scr) return;
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }

    // 红球按钮选中态
    for (int k = 0; k < 33; k++) {
        lv_obj_t* b = _pick_red_btns[k];
        if (!b) continue;
        if (_pick_red[k]) lv_obj_add_state(b, LV_STATE_CHECKED);
        else lv_obj_remove_state(b, LV_STATE_CHECKED);
    }
    for (int k = 0; k < 16; k++) {
        lv_obj_t* b = _pick_blue_btns[k];
        if (!b) continue;
        if (_pick_blue[k]) lv_obj_add_state(b, LV_STATE_CHECKED);
        else lv_obj_remove_state(b, LV_STATE_CHECKED);
    }

    int nr = 0, nb = 0;
    for (bool v : _pick_red) nr += v;
    for (bool v : _pick_blue) nb += v;

    if (_pick_summary) {
        const char* mode = (nr == 6 && nb == 1) ? "单式" : "复式";
        char buf[64];
        snprintf(buf, sizeof(buf), "红 %d + 蓝 %d · %s", nr, nb, mode);
        lv_label_set_text(_pick_summary, buf);
    }
    if (_judge_btn) {
        if (nr >= 6 && nr <= 20 && nb >= 1)
            lv_obj_clear_state(_judge_btn, LV_STATE_DISABLED);
        else
            lv_obj_add_state(_judge_btn, LV_STATE_DISABLED);
    }

    // 目标期 label + 开奖号小行
    if (_target_lbl && !issues.empty() && _target_idx < (int)issues.size()) {
        const ssq::DrawIssue& d = issues[_target_idx];
        char buf[64];
        snprintf(buf, sizeof(buf), "对奖期 %s", d.code.c_str());
        lv_label_set_text(_target_lbl, buf);
        if (_target_draws) {
            lv_obj_delete(_target_draws);
            _target_draws = lv_obj_create(_pages[3]);
            lv_obj_set_size(_target_draws, 700, 72);
            lv_obj_align(_target_draws, LV_ALIGN_TOP_RIGHT, -20, 0);
            lv_obj_set_style_bg_opa(_target_draws, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(_target_draws, 0, 0);
            lv_obj_set_style_pad_all(_target_draws, 0, 0);
            lv_obj_clear_flag(_target_draws, LV_OBJ_FLAG_SCROLLABLE);
            for (int i = 0; i < 6; i++) {
                lv_obj_t* b = _makeBall(_target_draws, d.reds[i], false, 48, zh20());
                lv_obj_set_pos(b, 700 - 8 * 56 + i * 56, 12);
            }
            lv_obj_t* bb = _makeBall(_target_draws, d.blue, true, 48, zh20());
            lv_obj_set_pos(bb, 700 - 8 * 56 + 6 * 56, 12);
        }
    }
}

void AppLottery::_judge()
{
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }
    if (issues.empty() || _target_idx >= (int)issues.size()) {
        _toast = "暂无开奖数据";
        return;
    }
    ssq::Pick p;
    for (int k = 0; k < 33; k++)
        if (_pick_red[k]) p.reds.push_back(k + 1);
    for (int k = 0; k < 16; k++)
        if (_pick_blue[k]) p.blues.push_back(k + 1);
    const int nr = (int)p.reds.size(), nb = (int)p.blues.size();
    if (nr < 6 || nr > 20 || nb < 1 || nb > 16) {
        _toast = "选号不合法 (红 6-20, 蓝 1-16)";
        return;
    }
    const ssq::DrawIssue& d = issues[_target_idx];
    if (nr == 6 && nb == 1) _showSingleResult(d, p);
    else _showMultiResult(d, p);
}

void AppLottery::_showSingleResult(const ssq::DrawIssue& d, const ssq::Pick& p)
{
    LvglLockGuard lock;
    if (!_scr) return;
    _closeResult();

    int reds6[6];
    for (int i = 0; i < 6; i++) reds6[i] = p.reds[i];
    ssq::PrizeResult r = ssq::judgeOne(d, reds6, p.blues[0]);

    _result_modal = lv_obj_create(_scr);
    lv_obj_set_size(_result_modal, 760, 460);
    lv_obj_align(_result_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_result_modal, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_radius(_result_modal, 24, 0);
    lv_obj_set_style_border_width(_result_modal, 0, 0);
    lv_obj_set_style_pad_all(_result_modal, 0, 0);
    lv_obj_clear_flag(_result_modal, LV_OBJ_FLAG_SCROLLABLE);

    char buf[96];
    snprintf(buf, sizeof(buf), "对奖结果 · %s 期", d.code.c_str());
    lv_obj_t* title = lv_label_create(_result_modal);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_DIM()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    // 选号球行
    for (int i = 0; i < 6; i++) {
        lv_obj_t* b = _makeBall(_result_modal, p.reds[i], false, 52, zh20());
        lv_obj_set_pos(b, 760 / 2 - (7 * 52 + 6 * 10) / 2 + i * 62, 90);
    }
    {
        lv_obj_t* b = _makeBall(_result_modal, p.blues[0], true, 52, zh20());
        lv_obj_set_pos(b, 760 / 2 - (7 * 52 + 6 * 10) / 2 + 6 * 62, 90);
    }

    static const char* names[7] = {"", "一等奖", "二等奖", "三等奖", "四等奖", "五等奖", "六等奖"};
    lv_obj_t* big = lv_label_create(_result_modal);
    if (r.level == 0) {
        lv_label_set_text(big, "未中奖");
        lv_obj_set_style_text_color(big, lv_color_hex(C_DIM()), 0);
    } else {
        snprintf(buf, sizeof(buf), "红中 %d %s · 中%s!", r.red_hits,
                 r.blue_hit ? "+蓝" : "", names[r.level]);
        lv_label_set_text(big, buf);
        lv_obj_set_style_text_color(big, lv_color_hex(0xFFB300), 0);
    }
    lv_obj_set_style_text_font(big, zh30(), 0);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 190);

    if (r.level > 0) {
        snprintf(buf, sizeof(buf), r.money > 0 ? "奖金 ¥%s" : "奖金按当期公告 (浮动)",
                 fmt_thousands(r.money).c_str());
        lv_obj_t* money = lv_label_create(_result_modal);
        lv_label_set_text(money, buf);
        lv_obj_set_style_text_font(money, zh30(), 0);
        lv_obj_set_style_text_color(money, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(money, LV_ALIGN_TOP_MID, 0, 260);
    }

    lv_obj_t* close = lv_button_create(_result_modal);
    lv_obj_set_size(close, 220, 64);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_ACC()), 0);
    lv_obj_set_style_radius(close, 14, 0);
    lv_obj_add_event_cb(close, [](lv_event_t* e) {
        static_cast<AppLottery*>(lv_event_get_user_data(e))->_closeResult();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* cl = lv_label_create(close);
    lv_label_set_text(cl, "知道了");
    lv_obj_set_style_text_font(cl, zh30(), 0);
    lv_obj_center(cl);
}

void AppLottery::_showMultiResult(const ssq::DrawIssue& d, const ssq::Pick& p)
{
    LvglLockGuard lock;
    if (!_scr) return;
    _closeResult();

    ssq::MultiResult r = ssq::judgeMulti(d, p);

    _result_modal = lv_obj_create(_scr);
    lv_obj_set_size(_result_modal, 860, 560);
    lv_obj_align(_result_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_result_modal, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_radius(_result_modal, 24, 0);
    lv_obj_set_style_border_width(_result_modal, 0, 0);
    lv_obj_set_style_pad_all(_result_modal, 0, 0);
    lv_obj_clear_flag(_result_modal, LV_OBJ_FLAG_SCROLLABLE);

    char buf[128];
    snprintf(buf, sizeof(buf), "复式对奖 · %s 期 · 共 %ld 注 (投注 ¥%ld)",
             d.code.c_str(), r.bet_count, r.bet_money);
    lv_obj_t* title = lv_label_create(_result_modal);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, zh20(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_DIM()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // 各奖级行
    static const char* names[7] = {"未中", "一等奖", "二等奖", "三等奖", "四等奖", "五等奖", "六等奖"};
    lv_obj_t* rows = lv_obj_create(_result_modal);
    lv_obj_set_size(rows, 780, 300);
    lv_obj_align(rows, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_update_layout(rows);
    bool any = false;
    int y = 0;
    for (int lv = 6; lv >= 1; lv--) {
        if (r.level_counts[lv] == 0) continue;
        any = true;
        long gm = ssq::gradeMoney(lv, d) * r.level_counts[lv];
        snprintf(buf, sizeof(buf), "%s × %ld 注%s%s", names[lv], r.level_counts[lv],
                 gm > 0 ? "   ¥" : "", gm > 0 ? fmt_thousands(gm).c_str() : "");
        lv_obj_t* l = lv_label_create(rows);
        lv_label_set_text(l, buf);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(lv >= 4 ? C_TEXT() : C_DIM()), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 20, y);
        y += 48;
    }
    if (!any) {
        lv_obj_t* l = lv_label_create(rows);
        lv_label_set_text(l, "全部注数未中奖");
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM()), 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 100);
    }

    // 总计
    lv_obj_t* total = lv_label_create(_result_modal);
    snprintf(buf, sizeof(buf), r.total_money > 0 ? "总奖金 ¥%s" : "未中奖",
             fmt_thousands(r.total_money).c_str());
    lv_label_set_text(total, buf);
    lv_obj_set_style_text_font(total, zh30(), 0);
    lv_obj_set_style_text_color(total,
                                lv_color_hex(r.total_money > 0 ? 0xFFB300 : C_DIM()), 0);
    lv_obj_align(total, LV_ALIGN_BOTTOM_LEFT, 24, -28);
    if (r.has_floating) {
        lv_obj_t* note = lv_label_create(_result_modal);
        lv_label_set_text(note, "* 含一/二等奖, 金额以当期公告为准");
        lv_obj_set_style_text_font(note, zh20(), 0);
        lv_obj_set_style_text_color(note, lv_color_hex(C_DIM()), 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 24, -4);
    }

    lv_obj_t* close = lv_button_create(_result_modal);
    lv_obj_set_size(close, 220, 64);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -24, -20);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_ACC()), 0);
    lv_obj_set_style_radius(close, 14, 0);
    lv_obj_add_event_cb(close, [](lv_event_t* e) {
        static_cast<AppLottery*>(lv_event_get_user_data(e))->_closeResult();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* cl = lv_label_create(close);
    lv_label_set_text(cl, "知道了");
    lv_obj_set_style_text_font(cl, zh30(), 0);
    lv_obj_center(cl);
}

void AppLottery::_closeResult()
{
    if (_result_modal) {
        lv_obj_delete(_result_modal);
        _result_modal = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  手势 (上拨退出; 底缘 y>650 起手 + 前台守卫 + 双轨 500ms 去重)
// ════════════════════════════════════════════════════════════════════════════
namespace {
struct SwipeCtx {
    AppLottery* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
};
}  // namespace

void AppLottery::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x;
                    ctx->press_y = p.y;   // 起点只在 PRESSED 记一次 (铁律①)
                    return;
                }
                if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
                    if (ctx->app->_result_modal) return;   // 结果弹窗期间屏蔽
                    if (lv_screen_active() != ctx->app->_scr) return;   // 前台守卫
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    // 上拨退出: 只认屏幕最下缘起手 (y>650)
                    if (ctx->press_y > 650 && dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* c = new SwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* cc = static_cast<SwipeCtx*>(u);
                            auto a = cc->alive.lock();
                            if (a && *a) cc->app->_requestExitOnce();
                            delete cc;
                        }, c);
                    }
                    return;
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (ctx->app->_result_modal) return;
                if (lv_screen_active() != ctx->app->_scr) return;   // 前台守卫
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    if (ctx->press_y <= 650) return;
                    auto* c = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* cc = static_cast<SwipeCtx*>(u);
                        auto a = cc->alive.lock();
                        if (a && *a) cc->app->_requestExitOnce();
                        delete cc;
                    }, c);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppLottery::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
}
