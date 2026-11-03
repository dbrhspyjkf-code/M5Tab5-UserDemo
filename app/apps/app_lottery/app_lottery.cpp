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
#include <esp_timer.h>
#include <esp_heap_caps.h>
#endif

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
#elif defined(PLATFORM_BUILD_WEB)
#include "web_fonts.h"
static const lv_font_t* zh30() { return webui::font_30(); }
static const lv_font_t* zh20() { return webui::font_20(); }
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

    // 桌面调试: TAB5_LOTTERY_TAB=0..2 直开指定 tab; TAB5_LOTTERY_PICK=
    // "3,11,17,22,23,29|9" 预填选号并自动判定 (SDL 合成点击不可靠, 同
    // TAB5_HEALTH_DETAIL 惯例)
#ifdef PLATFORM_BUILD_DESKTOP
    if (const char* t = getenv("TAB5_LOTTERY_TAB")) {
        int tab = atoi(t);
        if (tab >= 0 && tab <= 2) _setTab(tab);
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
        if (GetHAL()->isCameraCapturing()) GetHAL()->stopCameraCapture();   // 预览别跨页偷跑
        _removeSwipeGesture();
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
    if (_bets_ready.exchange(false)) _applyRemoteBets();

    // 相机 modal 开着但预览 task 已死 (init/alloc/ppa 失败走干净退出) →
    // 关 modal + toast 反馈 (失败原因看串口 [camera] 日志)
    if (_cam_modal && !GetHAL()->isCameraCapturing()) {
        _closeCamera();   // task 已死: stop 直接返回, 只删 modal
        _toast = "相机启动失败, 详见串口日志";
        _toast_start = 0;
    }

    // toast (拍照识别占位等短提示), 3s 自清 (持 LVGL 锁)
    if (_toast_lbl) {
        LvglLockGuard tlock;
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
    }, 32 * 1024);
    if (!ok) {
        _fetch_in_flight.store(false);
        std::lock_guard<std::mutex> lk(_mu);
        _data_err = "worker 启动失败";
        _data_ready.store(true);
    }
}

// 遗漏/频次预计算 (调用方已持 _mu)
// 数据到位 → 填 UI (onRunning=mooncake main 线程; LVGL 真正跑在 taskLVGL,
// 裸调 lv_* = 两线程并发改对象树 = heap corruption 间歇崩 — 2026-11-01
// coredump 三轮实锤, app_stocks 的 LvglLockGuard 同款铁律)
void AppLottery::_applyData()
{
    if (!_scr) return;
    LvglLockGuard lock;
    bool ok;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(_mu);
        ok = _data_ok;
        err = _data_err;
    }
    if (ok) {
        _updateDrawPage();
        if (_tab == 1) _drawStat();
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

    // 顶部 tab 栏: 3 个 CHECKABLE 按钮 (stocks 分时/日K 同款)
    const char* names[3] = {"开奖", "统计", "对奖"};
    for (int i = 0; i < 3; i++) {
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

    // 3 个全宽页面容器 (y=64 起)
    for (int i = 0; i < 3; i++) {
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
    _stat_canvas = nullptr;
    _stat_status = _stat_title_r = _stat_title_b = nullptr;
    for (auto& b : _stat_btns) b = nullptr;
    for (auto& b : _pick_red_btns) b = nullptr;
    for (auto& b : _pick_blue_btns) b = nullptr;
    _target_lbl = _target_draws = _pick_summary = _judge_btn = _photo_btn = nullptr;
    _result_modal = nullptr;
    _cam_modal = _cam_canvas = _ocr_wait_lbl = nullptr;
    _toast_lbl = nullptr;
}

void AppLottery::_destroyUi()
{
    _removeSwipeGesture();
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
    for (int i = 0; i < 3; i++) {
        if (i == tab) lv_obj_add_state(_tab_btns[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(_tab_btns[i], LV_STATE_CHECKED);
        lv_obj_add_flag(_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(_pages[tab], LV_OBJ_FLAG_HIDDEN);

    // canvas 像素按需分配/释放 (3MB 太贵, 离开 tab 即还; buffer 在分配时
    // 就地绑定到 canvas, 绘制函数不再管绑定)
    if (tab == 1 && !_stat_pixels) {
        _stat_pixels = malloc(CH_W * CH_H * 4);
        if (_stat_pixels) {
            canvas_draw::px_fill(_stat_pixels, CH_W, CH_H, C_BG());
            lv_canvas_set_buffer(_stat_canvas, _stat_pixels, CH_W, CH_H,
                                 LV_COLOR_FORMAT_ARGB8888);
            if (_data_ok) _drawStat();
        }
    }
    if (tab != 1 && _stat_pixels) {
        free(_stat_pixels);
        _stat_pixels = nullptr;
    }
}

// 圆形球控件 (红/蓝底白字号码)
lv_obj_t* AppLottery::_makeBall(lv_obj_t* parent, int num, bool blue,
                                int size, const lv_font_t* font)
{
    if (!parent) return nullptr;   // coredump 实锤: LVGL 低内存时 lv_obj_create
                                    // 返回 NULL, 空指针链上崩 (2026-11-01)
    lv_obj_t* ball = lv_obj_create(parent);
    if (!ball) return nullptr;
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
//  tab0 开奖
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

    // ── 最新期卡片 (整卡重建, 紧凑 188 高) ──
    if (_draw_head) lv_obj_delete(_draw_head);
    _draw_head = lv_obj_create(_pages[0]);
    lv_obj_set_size(_draw_head, SCREEN_W, 188);
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
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 10);

    // 奖池 / 销售额 (右上)
    snprintf(buf, sizeof(buf), "奖池 %s   销售额 %s", fmt_yi(lt.pool).c_str(),
             fmt_yi(lt.sales).c_str());
    lv_obj_t* pool = lv_label_create(_draw_head);
    lv_label_set_text(pool, buf);
    lv_obj_set_style_text_font(pool, zh20(), 0);
    lv_obj_set_style_text_color(pool, lv_color_hex(C_DIM()), 0);
    lv_obj_align(pool, LV_ALIGN_TOP_RIGHT, -28, 16);

    // 球行: 6 红 + 1 蓝, 54px 球 (数字 zh30)
    {
        const int bs = 54, gap = 12;
        const int total = 7 * bs + 6 * gap;
        const int x0 = (SCREEN_W - total) / 2;
        for (int i = 0; i < 6; i++) {
            lv_obj_t* b = _makeBall(_draw_head, lt.reds[i], false, bs, zh30());
            lv_obj_set_pos(b, x0 + i * (bs + gap), 52);
        }
        lv_obj_t* b = _makeBall(_draw_head, lt.blue, true, bs, zh30());
        lv_obj_set_pos(b, x0 + 6 * (bs + gap), 52);
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
    lv_obj_align(g12, LV_ALIGN_BOTTOM_MID, 0, -8);

    // ── 历史期列表 (总 10 期: 最新卡 1 + 列表 9; 一屏 4 卡 98 高,
    //    下方滚动查看, 底缘 pad 留给上拨退出带) ──
    if (_draw_list) lv_obj_delete(_draw_list);
    _draw_list = lv_obj_create(_pages[0]);
    lv_obj_set_size(_draw_list, SCREEN_W, SCREEN_H - TAB_H - 200);
    lv_obj_set_pos(_draw_list, 0, 200);
    lv_obj_set_style_bg_opa(_draw_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_draw_list, 0, 0);
    lv_obj_set_style_pad_all(_draw_list, 0, 0);
    lv_obj_set_style_pad_bottom(_draw_list, 60, 0);   // 底缘留给上拨退出带
    lv_obj_update_layout(_draw_list);

    const int rows = std::min((int)issues.size() - 1, 9);
    for (int r = 1; r <= rows; r++) {
        const ssq::DrawIssue& d = issues[r];
        lv_obj_t* row = lv_obj_create(_draw_list);
        lv_obj_set_size(row, SCREEN_W - 32, 98);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, (r - 1) * 110);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD()), 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        snprintf(buf, sizeof(buf), "%s 期", d.code.c_str());
        lv_obj_t* c = lv_label_create(row);
        lv_label_set_text(c, buf);
        lv_obj_set_style_text_font(c, zh20(), 0);
        lv_obj_set_style_text_color(c, lv_color_hex(C_DIM()), 0);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 24, 12);

        snprintf(buf, sizeof(buf), "%s 周%s", d.date.c_str(), d.week.c_str());
        lv_obj_t* dt = lv_label_create(row);
        lv_label_set_text(dt, buf);
        lv_obj_set_style_text_font(dt, zh20(), 0);
        lv_obj_set_style_text_color(dt, lv_color_hex(C_DIM()), 0);
        lv_obj_align(dt, LV_ALIGN_TOP_LEFT, 24, 46);

        // 小球 44px 居中 (数字 zh30)
        const int bx0 = (SCREEN_W - 32 - (6 * 52 + 44)) / 2;
        for (int i = 0; i < 6; i++) {
            lv_obj_t* b = _makeBall(row, d.reds[i], false, 44, zh30());
            lv_obj_set_pos(b, bx0 + i * 52, 14);
        }
        lv_obj_t* bb = _makeBall(row, d.blue, true, 44, zh30());
        lv_obj_set_pos(bb, bx0 + 6 * 52, 14);

        // 奖池 + 一二等奖注数 (与最新期卡同数据, 2026-11-01 用户要求)
        long n1 = 0, n2 = 0;
        for (const auto& g : d.grades) {
            if (g.level == 1) n1 = g.num;
            if (g.level == 2) n2 = g.num;
        }
        snprintf(buf, sizeof(buf), "奖池 %s · 一等奖 %ld 注 · 二等奖 %ld 注",
                 fmt_yi(d.pool).c_str(), n1, n2);
        lv_obj_t* info = lv_label_create(row);
        lv_label_set_text(info, buf);
        lv_obj_set_style_text_font(info, zh20(), 0);
        lv_obj_set_style_text_color(info, lv_color_hex(C_DIM()), 0);
        lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}
// ════════════════════════════════════════════════════════════════════════════
//  tab1 统计 (canvas 冷热频次柱状图)
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildStatPage()
{
    lv_obj_t* pg = _pages[1];

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
//  tab2 对奖
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_buildPickPage()
{
    lv_obj_t* pg = _pages[2];

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

    // 红球网格 11×3 (76x72, gap 8×16) → 宽 11*84-8=836, 居中
    for (int k = 0; k < 33; k++) {
        lv_obj_t* btn = lv_button_create(pg);
        const int col = k % 11, row = k / 11;
        lv_obj_set_size(btn, 76, 72);
        lv_obj_set_pos(btn, (SCREEN_W - 836) / 2 + col * 84, 112 + row * 88);
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

    // 蓝球行 16 个 (60x64, gap 12) → 宽 16*72-12=1140
    for (int k = 0; k < 16; k++) {
        lv_obj_t* btn = lv_button_create(pg);
        lv_obj_set_size(btn, 60, 64);
        lv_obj_set_pos(btn, (SCREEN_W - 1140) / 2 + k * 72, 384);
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

    // 底部操作行 y 500..572 (页面 656 高, 底缘余 84 给上拨退出带)
    _pick_summary = lv_label_create(pg);
    lv_obj_set_style_text_font(_pick_summary, zh20(), 0);
    lv_obj_set_style_text_color(_pick_summary, lv_color_hex(C_DIM()), 0);
    lv_obj_align(_pick_summary, LV_ALIGN_TOP_LEFT, 424, 526);   // 拍照识别与判定按钮之间空隙
    lv_label_set_text(_pick_summary, "红 0 + 蓝 0");

    auto mk_action = [&](const char* txt, int w, int x, uint32_t col,
                         lv_event_cb_t cb, void* ud) {
        lv_obj_t* b = lv_button_create(pg);
        lv_obj_set_size(b, w, 72);
        lv_obj_set_pos(b, x, 500);
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
                               static_cast<AppLottery*>(lv_event_get_user_data(e))->_openCamera();
                           }, this);
#ifdef PLATFORM_BUILD_WEB
    // web 版: 浏览器无相机, 隐藏拍照验票入口 (手动选号判定不受影响)
    lv_obj_add_flag(_photo_btn, LV_OBJ_FLAG_HIDDEN);
#endif
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
            _target_draws = lv_obj_create(_pages[2]);
            lv_obj_set_size(_target_draws, 700, 88);
            lv_obj_align(_target_draws, LV_ALIGN_TOP_RIGHT, -20, -2);
            lv_obj_set_style_bg_opa(_target_draws, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(_target_draws, 0, 0);
            lv_obj_set_style_pad_all(_target_draws, 0, 0);
            lv_obj_clear_flag(_target_draws, LV_OBJ_FLAG_SCROLLABLE);
            for (int i = 0; i < 6; i++) {
                lv_obj_t* b = _makeBall(_target_draws, d.reds[i], false, 64, zh30());
                lv_obj_set_pos(b, 700 - (6 * 72 + 64) - 12 + i * 72, 12);
            }
            lv_obj_t* bb = _makeBall(_target_draws, d.blue, true, 64, zh30());
            lv_obj_set_pos(bb, 700 - (6 * 72 + 64) - 12 + 6 * 72, 12);
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
    lv_obj_set_size(_result_modal, 940, 580);
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

    // 选号球行 (56px 球, 等距紧排)
    const int bx0 = (940 - (6 * 66 + 56)) / 2;
    for (int i = 0; i < 6; i++) {
        lv_obj_t* b = _makeBall(_result_modal, p.reds[i], false, 56, zh30());
        lv_obj_set_pos(b, bx0 + i * 66, 120);
    }
    {
        lv_obj_t* b = _makeBall(_result_modal, p.blues[0], true, 56, zh30());
        lv_obj_set_pos(b, bx0 + 6 * 66, 120);
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
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 260);

    if (r.level > 0) {
        snprintf(buf, sizeof(buf), r.money > 0 ? "奖金 ¥%s" : "奖金按当期公告 (浮动)",
                 fmt_thousands(r.money).c_str());
        lv_obj_t* money = lv_label_create(_result_modal);
        lv_label_set_text(money, buf);
        lv_obj_set_style_text_font(money, zh30(), 0);
        lv_obj_set_style_text_color(money, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(money, LV_ALIGN_TOP_MID, 0, 340);
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
    lv_obj_set_size(_result_modal, 1120, 640);
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
    lv_obj_set_style_text_font(title, zh30(), 0);
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
        lv_obj_set_style_text_font(note, zh30(), 0);
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
        // delete_async: 本函数高频出现在按钮 CLICKED 回调里 (删的是按钮
        // 自身所在的 modal 树) — 同步 delete 在 LVGL 事件分发中销毁目标
        // = UAF 间歇蓝屏重启 (2026-11-01 对奖 3 次崩 1 次实锤)
        lv_obj_delete_async(_result_modal);
        _result_modal = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  拍照识别 (预览 → 快门 → 上传 → 识别确认 → 判定)
// ════════════════════════════════════════════════════════════════════════════
void AppLottery::_openCamera()
{
    if (!_scr) return;
    if (GetHAL()->isCameraCapturing()) {
        _toast = "相机被其他应用占用";
        return;
    }
#ifdef PLATFORM_BUILD_DESKTOP
    _toast = "桌面模拟器无摄像头, 请在真机上使用";
    return;
#endif
    _closeResult();
    _cam_modal = lv_obj_create(_scr);
    lv_obj_set_size(_cam_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_cam_modal, 0, 0);
    lv_obj_set_style_bg_color(_cam_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_cam_modal, 0, 0);
    lv_obj_set_style_radius(_cam_modal, 0, 0);
    lv_obj_set_style_pad_all(_cam_modal, 0, 0);
    lv_obj_clear_flag(_cam_modal, LV_OBJ_FLAG_SCROLLABLE);

    _cam_canvas = lv_canvas_create(_cam_modal);
    lv_obj_set_size(_cam_canvas, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_cam_canvas, 0, 0);
    // buffer 由 hal 预览 task lv_canvas_set_buffer 挂 (RGB565 直刷)

    // 快门圆钮 (底部中央) + 取消
    lv_obj_t* shutter = lv_button_create(_cam_modal);
    lv_obj_set_size(shutter, 130, 130);
    lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shutter, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(shutter, 10, 0);
    lv_obj_set_style_border_color(shutter, lv_color_hex(0x666666), 0);
    lv_obj_add_event_cb(shutter, [](lv_event_t* e) {
        static_cast<AppLottery*>(lv_event_get_user_data(e))->_shutter();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* cancel = lv_button_create(_cam_modal);
    lv_obj_set_size(cancel, 150, 64);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 24, -32);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x2A2C30), 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, [](lv_event_t* e) {
        auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
        self->_closeCamera();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, zh30(), 0);
    lv_obj_center(cl);

    GetHAL()->startCameraCapture(_cam_canvas);
}

void AppLottery::_closeCamera()
{
    GetHAL()->stopCameraCapture();
    if (_cam_modal) {
        lv_obj_delete_async(_cam_modal);   // 同 _closeResult: 回调里删祖先树
        _cam_modal = nullptr;
        _cam_canvas = nullptr;
    }
}

void AppLottery::_shutter()
{
    if (_upload_in_flight.exchange(true)) return;
    // 两段式快门: 主线程只抓帧拷贝 (<100ms, 持 LVGL 锁可忍); JPEG 编码
    // (~1s CPU 密集) 挪 worker —— 持锁编码会把预览 task 卡在
    // lv_canvas_set_buffer 等锁, stop 等待随之超时 (2026-11-01 实测 UI 卡 3s)
    auto frame = std::make_shared<std::string>(GetHAL()->capturePhotoFrame());
    if (frame->empty()) {
        _upload_in_flight.store(false);
        _toast = "拍照失败, 请重试";
        return;
    }
    _closeCamera();
    // 上传 + 服务端 OCR ~10s, 给「识别中」反馈 (结果页弹出时清除)
    if (_scr && !_ocr_wait_lbl) {
        _ocr_wait_lbl = lv_label_create(_scr);
        lv_label_set_text(_ocr_wait_lbl, "识别中… (约 10 秒)");
        lv_obj_set_style_text_font(_ocr_wait_lbl, zh30(), 0);
        lv_obj_set_style_text_color(_ocr_wait_lbl, lv_color_hex(C_TEXT()), 0);
        lv_obj_align(_ocr_wait_lbl, LV_ALIGN_CENTER, 0, 0);
    }
    auto alive = _alive;
    bool ok = GetHAL()->tryRunDetached([this, frame, alive]() {
        auto* hal = GetHAL();
        // q65 ≈ 70KB (q85 ≈ 140KB): esp_hosted SDIO 大流量 POST 会间歇断言复位
        // (同款坑: 2026-10-26 封面 800KB 直灌下载; 传输层无节流钩子, 缩体积最直接)
        std::string jpeg = hal->jpegEncodeFrame(*frame, 65);
        if (jpeg.empty()) {
            std::lock_guard<std::mutex> lk(_bets_mu);
            _remote_err = "照片编码失败";
            _upload_in_flight.store(false);
            _bets_ready.store(true);
            return;
        }
        std::string base = "http://" + hal->getConfig("svc_host", "192.168.1.200") + ":8766";
        auto resp = hal->httpPost(base + "/api/ssq/ticket", jpeg,
                                  {{"Content-Type", "image/jpeg"}});
        {
            std::lock_guard<std::mutex> lk(_bets_mu);
            _remote_bets.clear();
            _remote_partial.clear();
            _remote_err.clear();
            if (!alive || !*alive) {
                _upload_in_flight.store(false);
                return;
            }
            try {
                auto j = nlohmann::json::parse(resp.body);
                if (j.value("ok", false) && (j.contains("bets") || j.contains("partial"))) {
                    for (const auto& b : j["bets"]) {
                        ssq::Pick p;
                        for (int r : b.value("reds", std::vector<int>{}))
                            if (r >= 1 && r <= 33) p.reds.push_back(r);
                        for (int bl : b.value("blue", std::vector<int>{}))
                            if (bl >= 1 && bl <= 16) p.blues.push_back(bl);
                        p.multiplier = std::max(1, std::min(99, b.value("x", 1)));
                        if (!p.reds.empty() && !p.blues.empty())
                            _remote_bets.push_back(std::move(p));
                    }
                    for (const auto& b : j.value("partial", nlohmann::json::array())) {
                        ssq::Pick p;
                        for (int r : b.value("reds", std::vector<int>{}))
                            if (r >= 1 && r <= 33) p.reds.push_back(r);
                        for (int bl : b.value("blue", std::vector<int>{}))
                            if (bl >= 1 && bl <= 16) p.blues.push_back(bl);
                        p.multiplier = std::max(1, std::min(99, b.value("x", 1)));
                        _remote_partial.push_back(std::move(p));
                    }
                    if (_remote_bets.empty() && _remote_partial.empty())
                        _remote_err = "识别结果为空";
                } else {
                    _remote_err = j.value("error", "HTTP " + std::to_string(resp.status));
                }
            } catch (const std::exception& e) {
                _remote_err = std::string("响应解析失败: ") + e.what();
            }
        }
        mclog::tagInfo(TAG, "upload worker done");
        _upload_in_flight.store(false);
        _bets_ready.store(true);
    }, 32 * 1024);   // jpeg 编码+JSON 深解析叠加, 16K 临界溢出(PSRAM 栈无运行时保护)
    if (!ok) {
        _upload_in_flight.store(false);
        _toast = "上传 worker 启动失败";
    }
}

// worker 完成 → 确认 modal (UI 线程)
void AppLottery::_applyRemoteBets()
{
    if (!_scr) return;
#ifndef PLATFORM_BUILD_DESKTOP
    mclog::tagInfo(TAG, "confirm: internal heap free={} largest={}",
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif
    if (_ocr_wait_lbl) {
        lv_obj_delete(_ocr_wait_lbl);
        _ocr_wait_lbl = nullptr;
    }
    std::vector<ssq::Pick> bets, partials;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(_bets_mu);
        bets = _remote_bets;
        partials = _remote_partial;
        err = _remote_err;
    }
    if (!err.empty()) {
        _toast = "识别失败: " + err;
        return;
    }
    if (bets.empty() && partials.empty()) return;

#ifndef PLATFORM_BUILD_DESKTOP
    // 堆完整性探针 (2026-11-01 崩溃读 bets 堆数据踩空 = heap corruption 延迟爆炸;
    // 检查器会在损坏块处直接 abort 并打印地址, 比随机崩点信息量大得多)
    {
        extern bool heap_caps_check_integrity_all(bool print);
        heap_caps_check_integrity_all(true);
        mclog::tagInfo(TAG, "confirm: heap integrity ok (bets={} partial={})", bets.size(), partials.size());
    }
#endif

    // 确认页创建挪 async: bets/partials move 进堆 ctx (app 实例 mooncake 常驻,
    // this 捕获安全), 避开 onRunning→mooncake update 同步深链上的时序/栈交互
    struct ConfirmCtx {
        AppLottery* app;
        std::vector<ssq::Pick> bets;
        std::vector<ssq::Pick> partials;
    };
    auto* ctx = new ConfirmCtx{this, std::move(bets), std::move(partials)};
    lv_async_call([](void* ud) {
        auto* c = static_cast<ConfirmCtx*>(ud);
        if (c && c->app) {
            c->app->_confirmModal(c->bets, c->partials);
        }
        delete c;
    }, ctx);
    mclog::tagInfo(TAG, "confirm: scheduled");
}

// 识别确认 modal: 逐注展示 + 「改」(填入手动选号) + 「对奖」;
// partial 注 (识别不完整) 显示红字标注, 只引导修正不参与判定
void AppLottery::_confirmModal(const std::vector<ssq::Pick>& bets,
                               const std::vector<ssq::Pick>& partials)
{
    LvglLockGuard lock;
    if (!_scr) return;
    _closeResult();

    const int rows = (int)bets.size() + (int)partials.size();
    const int mh = 150 + rows * 72 + 110 + (partials.empty() ? 0 : 40);
    _result_modal = lv_obj_create(_scr);
    if (!_result_modal) {
        _toast = "内存不足, 请退出重开 app 后重试";
        _toast_start = 0;
        return;
    }
    lv_obj_set_size(_result_modal, 880, std::min(mh, 640));
    lv_obj_align(_result_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_result_modal, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_radius(_result_modal, 24, 0);
    lv_obj_set_style_border_width(_result_modal, 0, 0);
    lv_obj_set_style_pad_all(_result_modal, 0, 0);
    lv_obj_clear_flag(_result_modal, LV_OBJ_FLAG_SCROLLABLE);

    char buf[96];
    if (partials.empty())
        snprintf(buf, sizeof(buf), "识别到 %d 注 (点「改」可修正后判定)", rows);
    else
        snprintf(buf, sizeof(buf), "完整 %d 注 + 不完整 %d 注 (请点「改」修正)", (int)bets.size(), (int)partials.size());
    lv_obj_t* title = lv_label_create(_result_modal);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // 保留 bets 供回调 (modal user_data 挂堆拷贝)
    auto* bets_copy = new std::vector<ssq::Pick>(bets);
    lv_obj_set_user_data(_result_modal, bets_copy);
    lv_obj_add_event_cb(_result_modal, [](lv_event_t* e) {
        delete static_cast<std::vector<ssq::Pick>*>(lv_obj_get_user_data(
            lv_event_get_current_target_obj(e)));
    }, LV_EVENT_DELETE, nullptr);

    const int y0 = 80;
    // 上限是 bets 自身行数——rows 含 partials, 用 rows 索引 bets[i] 在 partials 非空时
    // 越界读堆外 (2026-11-01 16:19 PANIC: bets=3 partials=2 → bets[3]/bets[4] 垃圾 Pick
    // → p.reds[k] 访问 NULL+0x14; 间歇性取决于堆垃圾内容, 只读不坏堆故探针测不出)
    for (int i = 0; i < (int)bets.size() && i < 5; i++) {
        const auto& p = bets[i];
        const int bx = 40;
        for (size_t k = 0; k < p.reds.size() && k < 8; k++) {
            lv_obj_t* b = _makeBall(_result_modal, p.reds[k], false, 44, zh20());
            if (b) lv_obj_set_pos(b, bx + k * 50, y0 + i * 72);
        }
        for (size_t k = 0; k < p.blues.size() && k < 2; k++) {
            lv_obj_t* b = _makeBall(_result_modal, p.blues[k], true, 44, zh20());
            if (b) lv_obj_set_pos(b, bx + 8 * 50 + k * 50, y0 + i * 72);
        }
        lv_obj_t* edit = lv_button_create(_result_modal);
        lv_obj_set_size(edit, 100, 52);
        lv_obj_align(edit, LV_ALIGN_TOP_RIGHT, -40, y0 + i * 72 - 4);
        lv_obj_set_style_bg_color(edit, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_radius(edit, 12, 0);
        lv_obj_set_user_data(edit, (void*)(intptr_t)i);
        lv_obj_add_event_cb(edit, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            auto* v = static_cast<std::vector<ssq::Pick>*>(
                lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_current_target_obj(e))));
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
            if (v && idx < (int)v->size()) self->_editBet((*v)[idx]);
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* el = lv_label_create(edit);
        lv_label_set_text(el, "改");
        lv_obj_set_style_text_font(el, zh20(), 0);
        lv_obj_center(el);
    }

    // partial 注行 (识别不完整): 球行 + 红字标注 + 「改」修正 (不参与判定)
    for (size_t pi = 0; pi < partials.size() && pi < 3; pi++) {
        const int row_y = y0 + ((int)bets.size() + (int)pi) * 72;
        const auto& p = partials[pi];
        for (size_t k = 0; k < p.reds.size() && k < 8; k++) {
            lv_obj_t* b = _makeBall(_result_modal, p.reds[k], false, 44, zh20());
            if (b) lv_obj_set_pos(b, 40 + k * 50, row_y);
        }
        for (size_t k = 0; k < p.blues.size() && k < 2; k++) {
            lv_obj_t* b = _makeBall(_result_modal, p.blues[k], true, 44, zh20());
            if (b) lv_obj_set_pos(b, 40 + 8 * 50 + k * 50, row_y);
        }
        lv_obj_t* tag = lv_label_create(_result_modal);
        lv_label_set_text(tag, "不完整");
        lv_obj_set_style_text_font(tag, zh20(), 0);
        lv_obj_set_style_text_color(tag, lv_color_hex(0xE53935), 0);
        lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, -150, row_y + 8);
        // 「改」: 每钮挂堆拷贝 (modal 删除事件统一回收到 bets_copy, 这里独立释放)
        auto* pick_copy = new ssq::Pick(p);
        lv_obj_t* edit = lv_button_create(_result_modal);
        lv_obj_set_size(edit, 100, 52);
        lv_obj_align(edit, LV_ALIGN_TOP_RIGHT, -40, row_y - 4);
        lv_obj_set_style_bg_color(edit, lv_color_hex(0x8A2E2E), 0);
        lv_obj_set_style_radius(edit, 12, 0);
        lv_obj_set_user_data(edit, pick_copy);
        lv_obj_add_event_cb(edit, [](lv_event_t* e) {
            auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
            lv_obj_t* btn = lv_event_get_current_target_obj(e);
            auto* pick = static_cast<ssq::Pick*>(lv_obj_get_user_data(btn));
            if (pick) {
                ssq::Pick p = *pick;
                delete pick;
                lv_obj_set_user_data(btn, nullptr);
                self->_editBet(p);
            }
        }, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(edit, [](lv_event_t* e) {
            auto* pick = static_cast<ssq::Pick*>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
            if (pick) {   // modal 被整删时释放 (走「改」路径已置空不会双删)
                delete pick;
                lv_obj_set_user_data(lv_event_get_current_target_obj(e), nullptr);
            }
        }, LV_EVENT_DELETE, nullptr);
        lv_obj_t* el = lv_label_create(edit);
        lv_label_set_text(el, "改");
        lv_obj_set_style_text_font(el, zh20(), 0);
        lv_obj_center(el);
    }

    mclog::tagInfo(TAG, "confirm: rows drawn");
    lv_obj_t* ok_btn = lv_button_create(_result_modal);
    lv_obj_set_size(ok_btn, 240, 68);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_RIGHT, -32, -24);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(C_ACC()), 0);
    lv_obj_set_style_radius(ok_btn, 14, 0);
    lv_obj_add_event_cb(ok_btn, [](lv_event_t* e) {
        auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
        auto* v = static_cast<std::vector<ssq::Pick>*>(
            lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_current_target_obj(e))));
        if (v) {
            if (v->empty()) {
                self->_toast = "无完整注, 请点「改」修正后手动判定";
                self->_toast_start = 0;
                return;
            }
            std::vector<ssq::Pick> bets = *v;   // 拷贝 (modal 随后删)
            self->_closeResult();
            self->_judgeBets(bets);
        }
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* ol = lv_label_create(ok_btn);
    lv_label_set_text(ol, "对  奖");
    lv_obj_set_style_text_font(ol, zh30(), 0);
    lv_obj_center(ol);

    lv_obj_t* retake = lv_button_create(_result_modal);
    lv_obj_set_size(retake, 200, 68);
    lv_obj_align(retake, LV_ALIGN_BOTTOM_LEFT, 32, -24);
    lv_obj_set_style_bg_color(retake, lv_color_hex(0x2A2C30), 0);
    lv_obj_set_style_radius(retake, 14, 0);
    lv_obj_add_event_cb(retake, [](lv_event_t* e) {
        auto* self = static_cast<AppLottery*>(lv_event_get_user_data(e));
        self->_closeResult();
        self->_openCamera();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* rl = lv_label_create(retake);
    lv_label_set_text(rl, "重  拍");
    lv_obj_set_style_text_font(rl, zh30(), 0);
    lv_obj_center(rl);
}

// 该注填入手动选号网格 (其余注丢弃), 用户在对奖页修正后手动判定
void AppLottery::_editBet(const ssq::Pick& bet)
{
    memset(_pick_red, 0, sizeof(_pick_red));
    memset(_pick_blue, 0, sizeof(_pick_blue));
    for (int r : bet.reds)
        if (r >= 1 && r <= 33) _pick_red[r - 1] = true;
    for (int b : bet.blues)
        if (b >= 1 && b <= 16) _pick_blue[b - 1] = true;
    _closeResult();
    _setTab(3);
    _updatePickUi();
    _toast = "已填入选号网格, 修正后点「判定」";
}

// 多注/复式混合判定 (拍照路径; 手动路径仍走 _showSingle/MultiResult)
void AppLottery::_judgeBets(const std::vector<ssq::Pick>& bets)
{
    std::vector<ssq::DrawIssue> issues;
    {
        std::lock_guard<std::mutex> lk(_mu);
        issues = _issues;
    }
    if (issues.empty()) {
        _toast = "暂无开奖数据";
        return;
    }
    const ssq::DrawIssue& d = issues[_target_idx < (int)issues.size() ? _target_idx : 0];

    LvglLockGuard lock;
    if (!_scr) return;
    _closeResult();

    static const char* names[7] = {"未中", "一等奖", "二等奖", "三等奖", "四等奖", "五等奖", "六等奖"};
    // 2026-11-01 用户要求: 结果卡字号/球放大; 金额含票面倍数 (六等奖 5×2=10 实测 bug)
    _result_modal = lv_obj_create(_scr);
    const int mh = 164 + (int)bets.size() * 92 + 110;
    lv_obj_set_size(_result_modal, 1180, std::min(mh, 700));
    lv_obj_align(_result_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_result_modal, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_radius(_result_modal, 24, 0);
    lv_obj_set_style_border_width(_result_modal, 0, 0);
    lv_obj_set_style_pad_all(_result_modal, 0, 0);
    lv_obj_clear_flag(_result_modal, LV_OBJ_FLAG_SCROLLABLE);

    char buf[160];
    snprintf(buf, sizeof(buf), "对奖结果 · %s 期 · 共 %d 注", d.code.c_str(), (int)bets.size());
    lv_obj_t* title = lv_label_create(_result_modal);
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    // 当期开奖号球行 (44px 居中, 供逐注对照)
    {
        const int bx0 = (1180 - (6 * 52 + 44)) / 2;
        for (int i = 0; i < 6; i++) {
            lv_obj_t* b = _makeBall(_result_modal, d.reds[i], false, 44, zh30());
            lv_obj_set_pos(b, bx0 + i * 52, 62);
        }
        lv_obj_t* b = _makeBall(_result_modal, d.blue, true, 44, zh30());
        lv_obj_set_pos(b, bx0 + 6 * 52, 62);
    }

    long total = 0;
    long bet_total = 0;
    bool has_floating = false;
    int y = 122;
    for (const auto& p : bets) {
        const int mult = std::max(1, p.multiplier);
        int extra_rows = 0;
        if (p.reds.size() == 6 && p.blues.size() == 1) {   // 单式逐注
            int r6[6];
            for (int i = 0; i < 6; i++) r6[i] = p.reds[i];
            ssq::PrizeResult r = ssq::judgeOne(d, r6, p.blues[0]);
            const long money = r.money * mult;
            total += money;
            bet_total += 2L * mult;
            if (r.level >= 1 && r.money == 0) has_floating = true;
            if (r.level) {
                snprintf(buf, sizeof(buf), "%s%s ¥%s", names[r.level],
                         mult > 1 ? ("×" + std::to_string(mult)).c_str() : "",
                         fmt_thousands(money).c_str());
            } else {
                snprintf(buf, sizeof(buf), "未中奖");
            }
            lv_obj_t* res = lv_label_create(_result_modal);
            lv_label_set_text(res, buf);
            lv_obj_set_style_text_font(res, zh30(), 0);
            lv_obj_set_style_text_color(res, lv_color_hex(r.level ? 0xFFB300 : C_DIM()), 0);
            lv_obj_align(res, LV_ALIGN_TOP_RIGHT, -32, y + 10);
        } else {   // 复式注
            ssq::MultiResult m = ssq::judgeMulti(d, p);
            total += m.total_money * mult;
            bet_total += m.bet_money * mult;
            if (m.has_floating) has_floating = true;
            snprintf(buf, sizeof(buf), "复式 %d红%d蓝 %ld 注%s",
                     (int)p.reds.size(), (int)p.blues.size(), m.bet_count * mult,
                     mult > 1 ? ("×" + std::to_string(mult)).c_str() : "");
            lv_obj_t* info = lv_label_create(_result_modal);
            lv_label_set_text(info, buf);
            lv_obj_set_style_text_font(info, zh30(), 0);
            lv_obj_set_style_text_color(info, lv_color_hex(C_DIM()), 0);
            lv_obj_align(info, LV_ALIGN_TOP_RIGHT, -32, y);
            int ly = y + 40;
            for (int lv = 6; lv >= 1; lv--) {
                if (m.level_counts[lv] == 0) continue;
                long gm = ssq::gradeMoney(lv, d) * m.level_counts[lv] * mult;
                snprintf(buf, sizeof(buf), "%s × %ld%s", names[lv], m.level_counts[lv],
                         gm > 0 ? (" ¥" + fmt_thousands(gm)).c_str() : "");
                lv_obj_t* l = lv_label_create(_result_modal);
                lv_label_set_text(l, buf);
                lv_obj_set_style_text_font(l, zh30(), 0);
                lv_obj_set_style_text_color(l, lv_color_hex(0xFFB300), 0);
                lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -32, ly);
                ly += 38;
                extra_rows++;
            }
        }
        // 该注球行: 44px 球 zh30, 蓝球紧跟红球 (不固定起点拉开距离)
        for (size_t k = 0; k < p.reds.size() && k < 12; k++) {
            lv_obj_t* b = _makeBall(_result_modal, p.reds[k], false, 44, zh30());
            lv_obj_set_pos(b, 32 + (int)k * 50, y + 5);
        }
        const int blue_x = 32 + (int)std::min(p.reds.size(), (size_t)12) * 50 + 8;
        for (size_t k = 0; k < p.blues.size() && k < 4; k++) {
            lv_obj_t* b = _makeBall(_result_modal, p.blues[k], true, 44, zh30());
            lv_obj_set_pos(b, blue_x + (int)k * 50, y + 5);
        }
        y += 92 + extra_rows * 38;
    }

    lv_obj_t* total_lbl = lv_label_create(_result_modal);
    if (total > 0) {
        snprintf(buf, sizeof(buf), "合计 ¥%s%s 投注¥%ld", fmt_thousands(total).c_str(),
                 has_floating ? "※" : "", bet_total);
        lv_label_set_text(total_lbl, buf);
        lv_obj_set_style_text_color(total_lbl, lv_color_hex(0xFFB300), 0);
    } else {
        snprintf(buf, sizeof(buf), "未中奖 (投注 ¥%ld)", bet_total);
        lv_label_set_text(total_lbl, buf);
        lv_obj_set_style_text_color(total_lbl, lv_color_hex(C_DIM()), 0);
    }
    lv_obj_set_style_text_font(total_lbl, zh30(), 0);
    lv_obj_align(total_lbl, LV_ALIGN_BOTTOM_LEFT, 32, -38);

    lv_obj_t* close = lv_button_create(_result_modal);
    lv_obj_set_size(close, 240, 72);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -32, -28);
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

// ════════════════════════════════════════════════════════════════════════════
//  手势 (上拨退出; 底缘 y>650 起手 + 前台守卫 + 双轨 500ms 去重)
// ════════════════════════════════════════════════════════════════════════════
namespace {
struct SwipeCtx {
    AppLottery* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
    int scroll0 = 0;   // PRESSED 时开奖列表滚动位 (该手势滚过列表则不退出)
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
                    ctx->scroll0 = ctx->app->_drawListScrollY();
                    return;
                }
                if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
                    if (ctx->app->_result_modal || ctx->app->_cam_modal) return;   // 弹窗期间屏蔽
                    if (lv_screen_active() != ctx->app->_scr) return;   // 前台守卫
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    // 该手势滚动了列表 (底缘起手上滑翻期数 ≠ 想退出)
                    if (std::abs(ctx->app->_drawListScrollY() - ctx->scroll0) > 10) return;
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
                if (ctx->app->_result_modal || ctx->app->_cam_modal) return;
                if (lv_screen_active() != ctx->app->_scr) return;   // 前台守卫
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    if (ctx->press_y <= 650) return;
                    if (std::abs(ctx->app->_drawListScrollY() - ctx->scroll0) > 10) return;
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
