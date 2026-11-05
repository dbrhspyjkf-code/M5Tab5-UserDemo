#include "app_yt.h"

#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <algorithm>

#include "../utils/rgb565_fetch.h"
#include "../app_games/gmu.h"
#include "../app_voice_input/app_voice_input.h"

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <xiaozhi_ctl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

static const char* TAG = "yt";

// 单调毫秒 (games/lottery 同款)
static int64_t yt_mono_ms()
{
#ifdef PLATFORM_BUILD_DESKTOP
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#else
    return (int64_t)(esp_timer_get_time() / 1000);
#endif
}

// URL 百分号编码 (搜索词含中文/空格; encodeURIComponent 同款)
static std::string yt_urlencode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// SwipeCtx: 定义+分配+使用一体 (games/lottery 同款)
struct YtSwipeCtx {
    AppYt* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
};

// 播放页缩略图占位 (8×8 黑块; buf 到货前绝不给 lv_image 喂空 data)
static const lv_image_dsc_t* yt_placeholder()
{
    static lv_image_dsc_t dsc;
    static uint16_t px[8 * 8];
    static bool init = [] {
        memset(px, 0, sizeof(px));
        memset(&dsc, 0, sizeof(dsc));
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = 8;
        dsc.header.h = 8;
        dsc.header.stride = 16;
        dsc.data_size = sizeof(px);
        dsc.data = (const uint8_t*)px;
        return true;
    }();
    (void)init;
    return &dsc;
}

// ════════════════════════════════════════════════════════════════════════════
//  生命周期
// ════════════════════════════════════════════════════════════════════════════
AppYt::AppYt()
{
    setAppInfo().name = "油管";
}

void AppYt::onOpen()
{
    mclog::tagInfo(TAG, "opened");
    if (_scr) _destroyUi();
    // 手势 alive 旗标成对铁律 (b275e71)
    _alive = std::make_shared<bool>(true);
    _app_closing.store(false);
    _buildUi();
    lv_screen_load(_scr);
    _installSwipeGesture();
    _histLoad();
    _histRender();

    // 硬件键盘 → 搜索框 (textarea 入 lvKbGroup 铁律, SSH 教训)
#ifndef PLATFORM_BUILD_DESKTOP
    if (GetHAL()->lvKbGroup && _ta) {
        lv_group_add_obj(GetHAL()->lvKbGroup, _ta);
        // 硬键盘 Enter 触发搜索 (textarea 默认会插 '\n')
        GetHAL()->onEnterPressed = [this]() {
            std::string q = lv_textarea_get_text(_ta);
            if (!q.empty()) _searchAsync(q);
        };
    }
#endif
}

void AppYt::onRunning()
{
    // 失焦收尾 (games/lottery 同款; async 删屏防「删除活动屏」)。
    // 油管关 app = 停播 (car 同语义): 失焦也顺手停流, 不做后台播。
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
        _want.store(-1);
        _removeSwipeGesture();
        lv_obj_t* scr = _scr;
        _nullUiPtrs();
        _scr = nullptr;
        lv_async_call([](void* p) { lv_obj_delete(static_cast<lv_obj_t*>(p)); }, scr);
        return;
    }
    if (!_scr) return;

    // 搜索结果应用 (onRunning=mooncake main 线程, LVGL 变更持锁 — lottery 铁律)
    if (_search_ready.exchange(false)) _applySearch();

    // 缩略图到货 → 贴图 (worker 只交指针, LVGL 全在持锁段)
    {
        std::vector<std::pair<int, void*>> batch;
        {
            std::lock_guard<std::mutex> lk(_thumb_mu);
            batch.swap(_thumb_ready);
        }
        if (!batch.empty()) {
            LvglLockGuard lock;
            for (auto& [idx, buf] : batch) {
                if (idx < 0 || idx >= (int)_rows.size()) continue;
                if (!_rows[idx]) continue;
                _thumb_bufs[idx] = buf;
                // 行内 0 号子对象 = 缩略图 lv_image 占位
                lv_obj_t* img = lv_obj_get_child(_rows[idx], 0);
                if (img) {
                    lv_image_set_src(img, buf);
                    lv_image_set_scale(img, 128);   // 320×180 → 显示 160×90
                }
                // 播放页大图同步 (若正播这一条)
                if (_cur_item == idx && _play_thumb) {
                    lv_image_set_src(_play_thumb, buf);
                    lv_image_set_scale(_play_thumb, 256);
                }
            }
        }
    }

    // 播放页进度/状态 (500ms 节流)
    const int64_t now = yt_mono_ms();
    if (_play_view && !lv_obj_has_flag(_play_view, LV_OBJ_FLAG_HIDDEN) &&
        now - _last_ui_ms >= 500) {
        _last_ui_ms = now;
        LvglLockGuard lock;
        const int st = _state.load();
        const int64_t bytes = _pcm_bytes.load();
        const int dur = (_cur_item >= 0 && _cur_item < (int)_items.size()) ? _items[_cur_item].d : 0;
        const int el = (int)(bytes / PCM_BYTES_PER_SEC);
        if (_play_status) {
            const char* s = nullptr;
            if (st == 1) s = "连接中… 首次起播约 5-15s";
            else if (st == 3) s = "播放失败, 点「停止」重选";
            else if (st == 0 && bytes > 0) s = "播完了, 点「停止」回列表";
            if (s) lv_label_set_text(_play_status, s);
        }
        if (_play_bar && dur > 0) {
            int pct = (int)(el * 100 / dur);
            if (pct > 100) pct = 100;
            lv_bar_set_value(_play_bar, pct, LV_ANIM_OFF);
        }
        if (_play_time) {
            char b[40];
            snprintf(b, sizeof(b), "%d:%02d / %d:%02d",
                     el / 60, el % 60, dur / 60, dur % 60);
            lv_label_set_text(_play_time, b);
        }
    }
}

void AppYt::onClose()
{
    mclog::tagInfo(TAG, "closed");
    _destroyUi();
    _alive = std::make_shared<bool>(false);
}

void AppYt::_requestExitOnce()
{
    int64_t now = yt_mono_ms();
    if (_last_exit_ms != 0 && now - _last_exit_ms < 500) return;
    _last_exit_ms = now;
    _requestClose();
}

// ════════════════════════════════════════════════════════════════════════════
//  搜索 + 历史
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_searchAsync(const std::string& q)
{
    bool expected = false;
    if (!_search_in_flight.compare_exchange_strong(expected, true)) return;
    // 状态提示跑在调用线程 (LVGL 线程/持锁 onRunning 两处入口都合法)
    if (_status_lbl) lv_label_set_text(_status_lbl, "搜索中… (冷搜约 10-25s)");
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, q]() {
        auto* hal = GetHAL();
        std::string base = "http://" + hal->getConfig("svc_host", "192.168.1.200") + ":8766";
        auto resp = hal->httpGet(base + "/yt/search?q=" + yt_urlencode(q));
        std::vector<YtItem> items;
        std::string err;
        if (!alive || !*alive) {
            _search_in_flight.store(false);
            return;
        }
        if (resp.status != 200 || resp.body.empty()) {
            err = "HTTP " + std::to_string(resp.status);
        } else {
            try {
                auto j = nlohmann::json::parse(resp.body);
                if (!j.value("ok", false)) {
                    err = j.value("error", "") == "no_results"
                              ? "无结果, 换个关键词"
                              : "搜索失败 (上游不可用)";
                } else {
                    for (const auto& it : j["items"]) {
                        YtItem d;
                        d.id = it.value("id", "");
                        d.t = it.value("t", "");
                        d.c = it.value("c", "");
                        d.d = it.value("d", 0);
                        if (!d.id.empty()) items.push_back(std::move(d));
                        if ((int)items.size() >= 20) break;
                    }
                    if (items.empty()) err = "无结果, 换个关键词";
                }
            } catch (const std::exception& e) {
                err = std::string("解析失败: ") + e.what();
            }
        }
        {
            std::lock_guard<std::mutex> lk(_mu);
            _items = std::move(items);
            _search_err = err;
        }
        _search_in_flight.store(false);
        _search_ready.store(true);
    }, 16 * 1024);
}

void AppYt::_applySearch()
{
    if (!_scr) return;
    std::vector<YtItem> items;
    std::string err;
    std::string q_snapshot;
    {
        std::lock_guard<std::mutex> lk(_mu);
        items = _items;
        err = _search_err;
    }

    _thumb_gen.fetch_add(1);   // 在途缩略图 worker 作废
    _want.store(-1);           // 新搜索 = 停播 (car 同语义)

    {
        LvglLockGuard lock;
        _showView(false);
        // 断行/播放页引用后释放旧缩略图缓冲
        for (auto& r : _rows) r = nullptr;
        _rows.clear();
        if (_play_thumb) lv_image_set_src(_play_thumb, yt_placeholder());
        for (void* b : _thumb_bufs) rgb565::release(b);
        _thumb_bufs.clear();
        _cur_item = -1;

        if (_status_lbl) {
            if (!err.empty()) {
                lv_label_set_text(_status_lbl, err.c_str());
            } else {
                char b[48];
                snprintf(b, sizeof(b), "%d 个结果", (int)items.size());
                lv_label_set_text(_status_lbl, b);
            }
            lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_DIM()), 0);
        }
        _buildRows();
        if (err.empty() && _ta) {
            q_snapshot = lv_textarea_get_text(_ta);
        }
    }
    if (!err.empty() || items.empty()) return;

    // 搜到结果 → 记历史 (NVS 写在锁内 ~ms 级, 可接受)
    if (!q_snapshot.empty()) _histAdd(q_snapshot);

    // 逐行拉缩略图 (单 worker 顺序拉; gen 不符即自弃, 释放归属始终在 worker)
    auto gen = _thumb_gen.load();
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, gen, items]() {
        for (size_t i = 0; i < items.size() && i < 20; i++) {
            if (!alive || !*alive || gen != _thumb_gen.load()) break;
            void* buf = rgb565::alloc(THUMB_W, THUMB_H);
            if (!buf) break;
            std::string base = "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
            if (!rgb565::download(base + "/yt/thumb/" + items[i].id + ".rgb565", buf)) {
                rgb565::release(buf);
                continue;
            }
            std::lock_guard<std::mutex> lk(_thumb_mu);
            if (gen != _thumb_gen.load()) {
                rgb565::release(buf);   // 拉完才过期的: 自己释放, 不交给 UI
                continue;
            }
            _thumb_ready.push_back({(int)i, buf});
        }
    }, 16 * 1024);
}

void AppYt::_histLoad()
{
    std::lock_guard<std::mutex> lk(_mu);
    _hist.clear();
    auto v = GetHAL()->getConfig("yt_hist", "");
    if (v.empty()) return;
    try {
        auto j = nlohmann::json::parse(v);
        if (j.is_array())
            for (const auto& e : j)
                if (e.is_string() && !e.get<std::string>().empty())
                    _hist.push_back(e.get<std::string>());
    } catch (...) {}
    if (_hist.size() > 10) _hist.resize(10);
}

void AppYt::_histSave()
{
    // 调用方已持 _mu
    nlohmann::json j = nlohmann::json::array();
    for (const auto& s : _hist) j.push_back(s);
    GetHAL()->setConfig("yt_hist", j.dump());
}

void AppYt::_histAdd(const std::string& q)
{
    std::string out;
    for (char c : q) out += (c == '\n' || c == '\r') ? ' ' : c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return;
    if (out.size() > 60) out = out.substr(0, 60);
    {
        std::lock_guard<std::mutex> lk(_mu);
        _hist.erase(std::remove(_hist.begin(), _hist.end(), out), _hist.end());
        _hist.insert(_hist.begin(), out);
        if (_hist.size() > 10) _hist.resize(10);
        _histSave();
    }
    _histRender();
}

void AppYt::_histDel(const std::string& q)
{
    {
        std::lock_guard<std::mutex> lk(_mu);
        _hist.erase(std::remove(_hist.begin(), _hist.end(), q), _hist.end());
        _histSave();
    }
    _histRender();
}

void AppYt::_histRender()
{
    // 只在 LVGL 线程/持锁上下文调用; 数据经 _mu 取快照
    std::vector<std::string> hist;
    {
        std::lock_guard<std::mutex> lk(_mu);
        hist = _hist;
    }
    if (!_hist_row) return;
    lv_obj_clean(_hist_row);
    for (const auto& q : hist) {
        lv_obj_t* chip = lv_button_create(_hist_row);
        if (!chip) break;
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_radius(chip, 22, 0);
        lv_obj_set_style_pad_hor(chip, 24, 0);
        lv_obj_set_style_pad_ver(chip, 8, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(chip, strdup(q.c_str()));
        lv_obj_add_event_cb(chip, [](lv_event_t* e) {
            auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
            lv_obj_t* chip = lv_event_get_current_target_obj(e);
            const char* q = (const char*)lv_obj_get_user_data(chip);
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_CLICKED && self && q) {
                lv_textarea_set_text(self->_ta, q);
                self->_searchAsync(q);
            } else if (code == LV_EVENT_LONG_PRESSED && self && q) {
                self->_histDel(q);   // 长按删除 (car 同款)
            }
        }, LV_EVENT_ALL, this);
        lv_obj_t* l = lv_label_create(chip);
        lv_label_set_text(l, q.c_str());
        lv_obj_set_style_text_font(l, gmu::zh20(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8EAEC), 0);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  播放控制 (UI; 调用点都在 LVGL 线程, 不加锁)
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_play(int i)
{
    if (i < 0 || i >= (int)_items.size()) return;
    _cur_item = i;
    _pcm_bytes.store(0);
    _state.store(1);
    _want.store(i);
    _startTask();

    if (_play_title) {
        lv_label_set_text(_play_title, _items[i].t.c_str());
    }
    if (_play_sub) {
        char b[96];
        snprintf(b, sizeof(b), "%s · %d:%02d", _items[i].c.c_str(),
                 _items[i].d / 60, _items[i].d % 60);
        lv_label_set_text(_play_sub, b);
    }
    if (_play_thumb) {
        lv_image_set_src(_play_thumb,
                         _thumb_bufs[i] ? _thumb_bufs[i] : yt_placeholder());
        lv_image_set_scale(_play_thumb, 256);
    }
    if (_play_status) lv_label_set_text(_play_status, "连接中… 首次起播约 5-15s");
    if (_play_bar) lv_bar_set_value(_play_bar, 0, LV_ANIM_OFF);
    _showView(true);
}

void AppYt::_stop()
{
    _want.store(-1);
    _showView(false);
}

void AppYt::_showView(bool play)
{
    if (!_scr) return;
    if (play) {
        if (_home_view) lv_obj_add_flag(_home_view, LV_OBJ_FLAG_HIDDEN);
        if (_play_view) lv_obj_clear_flag(_play_view, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (_play_view) lv_obj_add_flag(_play_view, LV_OBJ_FLAG_HIDDEN);
        if (_home_view) lv_obj_clear_flag(_home_view, LV_OBJ_FLAG_HIDDEN);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  UI 骨架
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG()), 0);

    // ── home 视图 ──
    _home_view = lv_obj_create(_scr);
    if (!_home_view) return;
    lv_obj_set_size(_home_view, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_home_view, 0, 0);
    lv_obj_set_style_bg_opa(_home_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_home_view, 0, 0);
    lv_obj_set_style_pad_all(_home_view, 0, 0);
    lv_obj_clear_flag(_home_view, LV_OBJ_FLAG_SCROLLABLE);

    // 搜索行: textarea + 语音 + 搜索
    _ta = lv_textarea_create(_home_view);
    if (_ta) {
        lv_obj_set_size(_ta, 640, 56);
        lv_obj_set_pos(_ta, 24, 12);
        lv_obj_set_style_text_font(_ta, gmu::zh20(), 0);
        lv_obj_set_style_bg_color(_ta, lv_color_hex(C_CARD()), 0);
        lv_obj_set_style_border_color(_ta, lv_color_hex(C_TRACK()), 0);
        lv_obj_set_style_border_width(_ta, 2, 0);
        lv_obj_set_style_radius(_ta, 14, 0);
        lv_obj_set_style_pad_hor(_ta, 16, 0);
        lv_textarea_set_placeholder_text(_ta, "搜索 YouTube… (语音/硬键盘)");
        lv_textarea_set_one_line(_ta, true);
        lv_obj_clear_flag(_ta, LV_OBJ_FLAG_SCROLLABLE);
    }
    gmu::action_btn(_home_view, "语音", 676, 12, 84, 56, [](lv_event_t* e) {
        auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
        if (self && self->_ta) AppVoiceInput::requestMicInput(self->_ta);
    }, this);
    gmu::action_btn(_home_view, "搜索", 772, 12, 120, 56, [](lv_event_t* e) {
        auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
        if (self && self->_ta) {
            std::string q = lv_textarea_get_text(self->_ta);
            if (!q.empty()) self->_searchAsync(q);
        }
    }, this, gmu::zh30());

    // 历史行 (横向滚动, 点击重搜 / 长按删除)
    _hist_row = lv_obj_create(_home_view);
    if (_hist_row) {
        lv_obj_set_size(_hist_row, SCREEN_W - 48, 48);
        lv_obj_set_pos(_hist_row, 24, 78);
        lv_obj_set_style_bg_opa(_hist_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_hist_row, 0, 0);
        lv_obj_set_style_pad_all(_hist_row, 0, 0);
        lv_obj_set_flex_flow(_hist_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(_hist_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(_hist_row, 10, 0);
        lv_obj_add_flag(_hist_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_hist_row, LV_DIR_HOR);
    }

    // 状态行
    _status_lbl = gmu::soft_label(_home_view, "输入关键词开始搜索", 24, 138, C_DIM());

    // 结果列表 (纵向滚动)
    _grid = lv_obj_create(_home_view);
    if (_grid) {
        lv_obj_set_size(_grid, SCREEN_W - 24, SCREEN_H - 170);
        lv_obj_set_pos(_grid, 12, 168);
        lv_obj_set_style_bg_opa(_grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_grid, 0, 0);
        lv_obj_set_style_pad_all(_grid, 0, 0);
        lv_obj_set_style_pad_row(_grid, 6, 0);
        lv_obj_set_flex_flow(_grid, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(_grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_grid, LV_DIR_VER);
    }

    // ── play 视图 (初始隐藏) ──
    _play_view = lv_obj_create(_scr);
    if (!_play_view) return;
    lv_obj_set_size(_play_view, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_play_view, 0, 0);
    lv_obj_set_style_bg_opa(_play_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_play_view, 0, 0);
    lv_obj_set_style_pad_all(_play_view, 0, 0);
    lv_obj_clear_flag(_play_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_play_view, LV_OBJ_FLAG_HIDDEN);

    _play_thumb = lv_image_create(_play_view);
    if (_play_thumb) {
        lv_image_set_src(_play_thumb, yt_placeholder());
        lv_obj_set_pos(_play_thumb, (SCREEN_W - THUMB_W) / 2, 56);
    }
    _play_title = gmu::soft_label(_play_view, "", 24, 268, C_TEXT());
    if (_play_title) {
        lv_obj_set_width(_play_title, SCREEN_W - 48);
        lv_label_set_long_mode(_play_title, LV_LABEL_LONG_DOT);
    }
    _play_sub = gmu::soft_label(_play_view, "", 24, 316, C_DIM());
    _play_status = gmu::soft_label(_play_view, "", 24, 356, C_ACC());

    _play_bar = lv_bar_create(_play_view);
    if (_play_bar) {
        lv_obj_set_size(_play_bar, SCREEN_W - 300, 12);
        lv_obj_set_pos(_play_bar, 24, 400);
        lv_bar_set_range(_play_bar, 0, 100);
        lv_obj_set_style_bg_color(_play_bar, lv_color_hex(C_TRACK()), 0);
        lv_obj_set_style_bg_color(_play_bar, lv_color_hex(C_ACC()), LV_PART_INDICATOR);
        lv_obj_set_style_radius(_play_bar, 6, 0);
    }
    _play_time = gmu::soft_label(_play_view, "0:00 / 0:00", SCREEN_W - 250, 392,
                                 C_SOFT(), gmu::mono20());

    gmu::action_btn(_play_view, "停 止", 24, 460, 220, 72, [](lv_event_t* e) {
        static_cast<AppYt*>(lv_event_get_user_data(e))->_stop();
    }, this, gmu::zh30());
}

void AppYt::_buildRows()
{
    if (!_grid) return;
    std::vector<YtItem> items;
    {
        std::lock_guard<std::mutex> lk(_mu);
        items = _items;
    }
    _rows.clear();
    for (size_t i = 0; i < items.size(); i++) {
        lv_obj_t* row = lv_obj_create(_grid);
        if (!row) return;
        lv_obj_set_size(row, SCREEN_W - 48, 116);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD()), 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
            int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
            if (self && self->_scr) self->_play(i);
        }, LV_EVENT_CLICKED, this);

        // 0 号子对象: 缩略图占位 (track 色方块, 到货后换真图)
        lv_obj_t* img = lv_image_create(row);
        if (img) {
            lv_obj_set_pos(img, 12, 13);
            lv_obj_set_size(img, 160, 90);
            lv_obj_set_style_bg_color(img, lv_color_hex(C_TRACK()), 0);
            lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(img, 8, 0);
            lv_obj_set_style_border_width(img, 0, 0);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_image_set_scale(img, 128);
        }
        // 标题 (可换行, 超宽截断)
        lv_obj_t* t = lv_label_create(row);
        if (t) {
            lv_label_set_text(t, items[i].t.c_str());
            lv_obj_set_style_text_font(t, gmu::zh20(), 0);
            lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT()), 0);
            lv_obj_set_pos(t, 188, 14);
            lv_obj_set_width(t, SCREEN_W - 48 - 200);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        }
        // 频道 · 时长
        lv_obj_t* s = lv_label_create(row);
        if (s) {
            char b[96];
            snprintf(b, sizeof(b), "%s · %d:%02d", items[i].c.c_str(),
                     items[i].d / 60, items[i].d % 60);
            lv_label_set_text(s, b);
            lv_obj_set_style_text_font(s, gmu::zh20(), 0);
            lv_obj_set_style_text_color(s, lv_color_hex(C_DIM()), 0);
            lv_obj_set_pos(s, 188, 82);
            lv_obj_set_width(s, SCREEN_W - 48 - 200);
            lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        }
        _rows.push_back(row);
        _thumb_bufs.push_back(nullptr);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  手势 (上拨退出)
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new YtSwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                auto* ctx = static_cast<YtSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x;
                    ctx->press_y = p.y;
                    return;
                }
                if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
                    if (lv_screen_active() != ctx->app->_scr) return;
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    if (ctx->press_y > 650 && dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* c = new YtSwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* cc = static_cast<YtSwipeCtx*>(u);
                            auto a = cc->alive.lock();
                            if (a && *a) cc->app->_requestExitOnce();
                            delete cc;
                        }, c);
                    }
                    return;
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<YtSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (lv_screen_active() != ctx->app->_scr) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    if (ctx->press_y <= 650) return;
                    auto* c = new YtSwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* cc = static_cast<YtSwipeCtx*>(u);
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

void AppYt::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
        _gesture_ctx = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  销毁
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_nullUiPtrs()
{
    _home_view = _play_view = _ta = _hist_row = _grid = _status_lbl = nullptr;
    for (auto& r : _rows) r = nullptr;
    _rows.clear();
    _play_thumb = _play_title = _play_sub = _play_bar = _play_time = _play_status = nullptr;
}

void AppYt::_destroyUi()
{
    _removeSwipeGesture();
    // 停播 + 作废在途 worker (关 app = 停播, car 同语义)
    _app_closing.store(true);
    _want.store(-1);
    _thumb_gen.fetch_add(1);
#ifndef PLATFORM_BUILD_DESKTOP
    if (GetHAL()->lvKbGroup && _ta) lv_group_remove_obj(_ta);
    GetHAL()->onEnterPressed = nullptr;
#endif
    // 释放 UI 持有的缩略图缓冲 (在途那块归 worker 自己释放, 见 worker gen 检查)
    for (void* b : _thumb_bufs) rgb565::release(b);
    _thumb_bufs.clear();
    {
        std::lock_guard<std::mutex> lk(_thumb_mu);
        for (auto& p : _thumb_ready) rgb565::release(p.second);
        _thumb_ready.clear();
    }
    lv_obj_t* scr = _scr;
    _nullUiPtrs();
    _scr = nullptr;
    if (scr) lv_obj_delete(scr);
}

// ════════════════════════════════════════════════════════════════════════════
//  流任务 (设备端; 桌面无流引擎) — 收音机 8769 消费链同款
// ════════════════════════════════════════════════════════════════════════════
#ifndef PLATFORM_BUILD_DESKTOP

namespace {

constexpr int kYtRingSize = 131072;   // 128KB ≈ 4s @ 32KB/s (CPU 突发吸收, radio 同款)

uint8_t* s_yt_ring = nullptr;
int s_yt_head = 0, s_yt_tail = 0;
bool s_yt_armed = false;

inline int yt_ring_used()
{
    int u = s_yt_head - s_yt_tail;
    return u >= 0 ? u : u + kYtRingSize;
}

struct YtResampler {
    int32_t prev = 0;
    int phase = 0;
    int16_t out[3072];
    void process(const int16_t* in, int n, bool reset)
    {
        if (reset) { prev = 0; phase = 0; }
        int oc = 0;
        for (int i = 0; i < n; i++) {
            int32_t cur = in[i];
            while (phase < 3) {
                out[oc++] = (int16_t)(prev + (cur - prev) * phase / 3);
                if (oc == (int)(sizeof(out) / sizeof(out[0]))) {
                    xiaozhi_codec_write(out, oc);
                    oc = 0;
                }
                phase += 2;
            }
            phase -= 3;
            prev = cur;
        }
        if (oc > 0) xiaozhi_codec_write(out, oc);
    }
};
YtResampler s_yt_rs;

void yt_ring_drain()
{
    if (!s_yt_ring) return;
    if (!s_yt_armed) {
        if (yt_ring_used() < kYtRingSize / 4) return;   // 起播攒 1/4 环
        s_yt_armed = true;
    }
    if (yt_ring_used() == 0) return;
    static int16_t chunk[2048];
    while (yt_ring_used() >= (int)sizeof(chunk)) {
        int contig = kYtRingSize - s_yt_tail;
        if (contig > (int)sizeof(chunk)) contig = sizeof(chunk);
        memcpy(chunk, s_yt_ring + s_yt_tail, contig);
        s_yt_tail = (s_yt_tail + contig) % kYtRingSize;
        if (contig < (int)sizeof(chunk)) {
            memcpy((uint8_t*)chunk + contig, s_yt_ring, sizeof(chunk) - contig);
            s_yt_tail = sizeof(chunk) - contig;
        }
        s_yt_rs.process(chunk, sizeof(chunk) / sizeof(chunk[0]), false);
    }
}

void yt_ring_push(const uint8_t* p, int len)
{
    while (len > 0) {
        int free_b = kYtRingSize - 1 - yt_ring_used();
        if (free_b == 0) {
            yt_ring_drain();   // 输出端阻塞=背压
            free_b = kYtRingSize - 1 - yt_ring_used();
            if (free_b == 0) vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int take = len < free_b ? len : free_b;
        int contig = kYtRingSize - s_yt_head;
        if (contig > take) contig = take;
        memcpy(s_yt_ring + s_yt_head, p, contig);
        s_yt_head = (s_yt_head + contig) % kYtRingSize;
        p += contig;
        len -= contig;
    }
}

// 任意字节数喂入, 奇数字节跨次拼对 (radio 同款)
void yt_feed_pcm(const uint8_t* p, int n, uint8_t& odd_carry, bool& have_odd)
{
    int off = 0;
    if (have_odd && n >= 1) {
        uint8_t pair[2] = {odd_carry, p[0]};
        yt_ring_push(pair, 2);
        have_odd = false;
        off = 1;
        p += 1;
    }
    int bytes = n - off;
    if (bytes & 1) {
        odd_carry = p[bytes - 1];
        have_odd = true;
        bytes -= 1;
    }
    if (bytes > 0) yt_ring_push(p, bytes);
}

void yt_teardown(esp_http_client_handle_t& http)
{
    if (http) {
        esp_http_client_cleanup(http);
        http = nullptr;
    }
    s_yt_head = s_yt_tail = 0;
    s_yt_armed = false;
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

}  // namespace

void AppYt::_startTask()
{
    if (_task_started.exchange(true)) return;
    GetHAL()->tryRunDetached([this]() {
        auto* hal = GetHAL();
        std::string base = "http://" + hal->getConfig("svc_host", "192.168.1.200") + ":8766";
        esp_http_client_handle_t http = nullptr;
        int cur = -2;
        static uint8_t inbuf[4096];
        uint8_t odd_carry = 0;
        bool have_odd = false;
        int stall = 0;

        while (!_app_closing.load()) {
            const int want = _want.load();
            if (want != cur) {
                yt_teardown(http);
                cur = want;
                odd_carry = 0;
                have_odd = false;
                stall = 0;
                if (cur < 0) {
                    _state.store(0);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    continue;
                }
                // 取该 item 的 id 快照 (新搜索会换 _items; 播放中换表已被 _want=-1 挡住)
                std::string fid;
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    if (cur >= 0 && cur < (int)_items.size()) fid = _items[cur].id;
                }
                if (fid.empty()) {
                    _state.store(3);
                    cur = -1;
                    continue;
                }
                // 播放期关 WiFi 省电 (radio 实锤: 省电 → PCM 供不上卡顿)
                esp_wifi_set_ps(WIFI_PS_NONE);
                char url[192];
                snprintf(url, sizeof(url), "%s/yt/pcm/%s", base.c_str(), fid.c_str());
                esp_http_client_config_t hcfg = {};
                hcfg.url          = url;
                hcfg.buffer_size  = 4096;
                hcfg.buffer_size_tx = 1024;
                hcfg.timeout_ms   = 8000;
                http = esp_http_client_init(&hcfg);
                bool ok = false;
                if (http && esp_http_client_open(http, 0) == ESP_OK && !_app_closing.load()) {
                    esp_http_client_fetch_headers(http);
                    int sc = esp_http_client_get_status_code(http);
                    int first = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
                    if (sc == 200 && first > 0) {
                        ok = true;
                        if (!s_yt_ring)
                            s_yt_ring = (uint8_t*)heap_caps_malloc(kYtRingSize,
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        s_yt_rs.prev = 0;
                        s_yt_rs.phase = 0;
                        yt_feed_pcm(inbuf, first, odd_carry, have_odd);
                        _pcm_bytes.fetch_add(first);
                        mclog::tagInfo(TAG, "pcm up: sc={} first={}B", sc, first);
                    }
                }
                if (!ok) {
                    _state.store(3);
                    yt_teardown(http);
                    cur = -1;
                    continue;
                }
                esp_http_client_set_timeout_ms(http, 1000);   // 感知停止/断流
                _state.store(2);
            }

            if (!http) { vTaskDelay(pdMS_TO_TICKS(80)); continue; }
            int n = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
            if (n == 0) {                    // 服务端关流 = 播完
                _state.store(0);
                yt_teardown(http);
                cur = -1;
                continue;
            }
            if (n < 0) {
                if (n != -ESP_ERR_HTTP_EAGAIN) {
                    _state.store(3);
                    yt_teardown(http);
                    cur = -1;
                    continue;
                }
                yt_ring_drain();             // 断流期环内余量续播
                if (++stall >= 20) {         // 连续 ~20s 无数据 → 失败
                    _state.store(3);
                    yt_teardown(http);
                    cur = -1;
                }
                continue;
            }
            stall = 0;
            _pcm_bytes.fetch_add(n);
            yt_feed_pcm(inbuf, n, odd_carry, have_odd);
            yt_ring_drain();
        }
        yt_teardown(http);
        _task_started.store(false);
        mclog::tagInfo(TAG, "pcm task exit");
    }, 24 * 1024);
}

#else   // 桌面模拟器: 无流引擎

void AppYt::_startTask() {}

#endif
