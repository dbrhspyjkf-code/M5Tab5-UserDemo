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

// 播放输出 (收音机 kOutputs 同一对 MA 设备; pid=nullptr 即本机 I2S)
struct YtOutput { const char* name; const char* full; const char* pid; };
static const YtOutput kOutputs[] = {
    {"本机",   "本机播放",        nullptr},
    {"客厅",   "客厅 Sonos",      "RINCON_804AF273361501400"},
    {"起居室", "起居室 HomePod",  "ap464e4403a588"},
};
static const int kOutputCount = (int)(sizeof(kOutputs) / sizeof(kOutputs[0]));
static const char* kRelayBase = "http://192.168.1.200:8769/radio/";

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
    int scroll0 = 0;   // PRESSED 时列表滚动位 (该手势滚过列表 ≠ 想退出, lottery 同款)
};

// 图片占位 (8×8 黑块; buf 到货前绝不给 lv_image 喂空 data)
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
    _output_sel = _nvsOutput();
    _buildUi();
    lv_screen_load(_scr);
    _installSwipeGesture();
    _histLoad();
    _histRender();
    _refreshOutputUi();

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
    // 油管关 app = 停播 (car 同语义): 失焦也全停 (本机 + 外放), 不做后台播。
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
        _want.store(-1);
        _removeSwipeGesture();
        _detachInput();   // 防悬垂: lvKbGroup/语音回调指向即将删除的 textarea
        _closeOutputModal();                       // 弹窗挂在 lv_layer_top, 必须先摘
        std::vector<void*> bufs = _takeImageBufs();
        lv_obj_t* scr = _scr;
        _nullUiPtrs();
        _scr = nullptr;
        lv_async_call([](void* p) { lv_obj_delete(static_cast<lv_obj_t*>(p)); }, scr);
        for (void* b : bufs) rgb565::release(b);   // 删屏已发 async, 引用已断
        return;
    }
    if (!_scr) return;

    // 搜索结果应用 (onRunning=mooncake main 线程, LVGL 变更持锁 — lottery 铁律)
    if (_search_ready.exchange(false)) _applySearch();

    // 列表缩略图到货 → 贴图 (worker 只交指针, LVGL 全在持锁段)
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
                lv_obj_t* img = lv_obj_get_child(_rows[idx], 0);   // 0 号子对象 = 缩略图
                if (img) {
                    lv_image_set_src(img, buf);
                    lv_image_set_scale(img, 128);   // 320×180 → 显示 160×90
                    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
                }
            }
        }
    }

    // 大封面到货 → 播放页 (占位 8×8 STRETCH → 大图 STRETCH, 框尺寸恒定不跳版)
    {
        void* buf = nullptr;
        uint32_t gen = 0;
        {
            std::lock_guard<std::mutex> lk(_cover_mu);
            if (_cover_ready) {
                buf = _cover_ready;
                _cover_ready = nullptr;
                gen = _cover_ready_gen;
            }
        }
        if (buf) {
            LvglLockGuard lock;
            if (gen == _cover_gen.load() && _play_thumb && _cur_item >= 0) {
                if (_cover_buf) lv_image_cache_drop(_cover_buf);   // 旧图出缓存 (P4 有 cache)
                _cover_buf = buf;
                lv_image_set_src(_play_thumb, buf);
            } else {
                rgb565::release(buf);   // 已切走/换条目: 无人引用, 直接释放
            }
        }
    }

    // 播放页刷新 (500ms 节流)
    const int64_t now = yt_mono_ms();
    if (_play_view && !lv_obj_has_flag(_play_view, LV_OBJ_FLAG_HIDDEN) &&
        now - _last_ui_ms >= 500) {
        _last_ui_ms = now;
        LvglLockGuard lock;
        _updatePlayPage();
    }

    // MA 音量回读 → 滑条 (外放切换后 _ext_vol 送达)
    if (int v = _ext_vol.exchange(-1); v >= 0 && _vol_slider) {
        LvglLockGuard lock;
        lv_slider_set_value(_vol_slider, v, LV_ANIM_OFF);
        if (_vol_lbl) {
            char b[16];
            snprintf(b, sizeof(b), "%d", v);
            lv_label_set_text(_vol_lbl, b);
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
    _stopPlayback();           // 新搜索 = 停播 (本机 + 外放, car 同语义)

    {
        LvglLockGuard lock;
        _showView(false);
        for (auto& r : _rows) r = nullptr;
        _rows.clear();
        if (_play_thumb) lv_image_set_src(_play_thumb, yt_placeholder());
        if (_cover_buf) { rgb565::release(_cover_buf); _cover_buf = nullptr; }
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

    // 搜到结果 → 记历史 (NVS 写 ~ms 级)
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
//  播放控制
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_play(int i)
{
    if (i < 0 || i >= (int)_items.size()) return;
    _cur_item = i;
    _pcm_bytes.store(0);
    _err_code.store(0);
    _paused.store(false);
    _ext_pause_acc_ms.store(0);
    _ext_pause_start_ms.store(0);
    {
        LvglLockGuard lock;
        _fillPlayPage(i);
        _showView(true);
    }
    if (_output_sel > 0) _extStart(i);   // 外放: prepare → relay play_on (worker)
    else {
        _state.store(1);
        _want.store(i);
        _startTask();                    // 本机: 流任务
    }
}

void AppYt::_stopPlayback()
{
    _want.store(-1);                     // 本机流任务收尾
    if (_ext_on.exchange(false)) {       // 外放停 (worker 发 cmd stop)
        _ext_gen.fetch_add(1);           // 作废在途 prepare/play_on
        _ext_t0_ms.store(0);
        int dev = _ext_dev.exchange(-1);
        if (dev >= 0 && dev < kOutputCount)
            GetHAL()->tryRunDetached([dev]() { AppYt::_relayCmd(kOutputs[dev].pid, "stop", -1); });
    }
    if (_state.load() != 3) _state.store(0);
}

void AppYt::_togglePause()
{
    const int st = _state.load();
    if (st != 2) return;                 // 只有播放中可暂停
    const bool now_paused = !_paused.load();
    _paused.store(now_paused);
    if (_output_sel > 0 && _ext_on.load()) {
        // 外放: MA pause/resume; 暂停墙钟停表
        const int64_t now = yt_mono_ms();
        if (now_paused) {
            _ext_pause_start_ms.store(now);
        } else {
            _ext_pause_acc_ms.fetch_add(now - _ext_pause_start_ms.exchange(0));
        }
        int dev = _ext_dev.load();
        if (dev >= 0 && dev < kOutputCount)
            GetHAL()->tryRunDetached([dev, now_paused]() {
                AppYt::_relayCmd(kOutputs[dev].pid, now_paused ? "pause" : "play", -1);
            });
    }
    // 本机: 流任务见 _paused 停读, TCP 背压 → 服务端 ffmpeg stdin 写阻塞 → 真暂停
    _refreshPlayBtn();
}

void AppYt::_refreshPlayBtn()
{
    if (!_pp_lbl) return;
    lv_label_set_text(_pp_lbl, _paused.load() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

// ── NVS ──
int AppYt::_nvsOutput()
{
    int v = atoi(GetHAL()->getConfig("yt_out", "0").c_str());
    return (v >= 0 && v < kOutputCount) ? v : 0;
}

void AppYt::_setOutput(int i)
{
    if (i < 0 || i >= kOutputCount || i == _output_sel) return;
    _output_sel = i;
    GetHAL()->setConfig("yt_out", std::to_string(i));
    _closeOutputModal();
    _refreshOutputUi();
    const bool was_playing = _state.load() == 2 || _state.load() == 1;
    const int cur = _cur_item;
    if (!was_playing || cur < 0) {
        if (i > 0) _extVolSync();        // 待机切换: 同步音箱音量到滑条
        return;
    }
    // 播放中切换: 立即换到新输出重新开播 (收音机同款语义)
    _stopPlayback();
    _paused.store(false);
    _pcm_bytes.store(0);
    _ext_pause_acc_ms.store(0);
    {
        LvglLockGuard lock;
        _refreshPlayBtn();
    }
    if (i > 0) _extStart(cur);
    else {
        _state.store(1);
        _want.store(cur);
        _startTask();
    }
}

// ── 外放 (relay → MA) ──
bool AppYt::_relayCmd(const char* pid, const char* cmd, int v)
{
    char url[160];
    if (v >= 0) snprintf(url, sizeof(url), "%scmd?p=%s&c=%s&v=%d", kRelayBase, pid, cmd, v);
    else        snprintf(url, sizeof(url), "%scmd?p=%s&c=%s", kRelayBase, pid, cmd);
    auto r = GetHAL()->httpGet(url);
    // relay JSON 成功判定: {"ok":true} / {"ok": true} 均认 (radio ext_ok 同款双形态)
    return r.status == 200 &&
           (r.body.find("\"ok\":true") != std::string::npos ||
            r.body.find("\"ok\": true") != std::string::npos);
}

void AppYt::_extStart(int i)
{
    if (i < 0 || i >= (int)_items.size()) return;
    const int sel = _output_sel;
    if (sel <= 0 || sel >= kOutputCount) return;
    const std::string fid = _items[i].id;
    _ext_on.store(true);
    _state.store(1);
    _err_code.store(0);
    const auto gen = _ext_gen.fetch_add(1) + 1;   // 本代令牌
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, gen, sel, fid]() {
        auto* hal = GetHAL();
        std::string base = "http://" + hal->getConfig("svc_host", "192.168.1.200") + ":8766";
        // ① 预热管线 (冷 15-60s; HAL 给 /yt/prepare 100s 超时)
        auto pr = hal->httpGet(base + "/yt/prepare/" + fid);
        if (!alive || !*alive || gen != _ext_gen.load()) return;
        if (pr.status != 200) {
            _extFail(gen, pr.status == 429 ? 429 : 1502);
            return;
        }
        // ② MA 转投 (先 stop 再 play; relay 已踢旧 /ext 链)
        char url[192];
        snprintf(url, sizeof(url), "%syt/%s/play_on?p=%s",
                 kRelayBase, fid.c_str(), kOutputs[sel].pid);
        auto r = hal->httpGet(url);
        if (!alive || !*alive || gen != _ext_gen.load()) return;
        bool ok = r.status == 200 &&
                  (r.body.find("\"ok\":true") != std::string::npos ||
                   r.body.find("\"ok\": true") != std::string::npos);
        if (!ok) {
            _extFail(gen, 1502);
            return;
        }
        _ext_dev.store(sel);
        _ext_t0_ms.store(yt_mono_ms());
        _ext_pause_acc_ms.store(0);
        _state.store(2);
        mclog::tagInfo(TAG, "ext play: {} -> {}", fid, kOutputs[sel].full);
        _extVolSync();                   // 音箱音量 → 滑条
    }, 16 * 1024);
}

void AppYt::_extFail(uint32_t gen, int code)
{
    if (gen != _ext_gen.load()) return;  // 已被新会话取代
    _ext_on.store(false);
    _err_code.store(code);
    _state.store(3);
    int exp = _cur_item;
    _want.compare_exchange_strong(exp, -1);
    mclog::tagWarn(TAG, "ext fail: code={}", code);
}

void AppYt::_extStop()
{
    _ext_on.store(false);
    _ext_gen.fetch_add(1);
    int dev = _ext_dev.exchange(-1);
    if (dev >= 0 && dev < kOutputCount)
        GetHAL()->tryRunDetached([dev]() { AppYt::_relayCmd(kOutputs[dev].pid, "stop", -1); });
}

void AppYt::_extVolSync()
{
    int sel = _output_sel;
    if (sel <= 0 || sel >= kOutputCount) return;
    if (_ext_vol_wait.exchange(true)) return;
    GetHAL()->tryRunDetached([this, sel]() {
        char url[160];
        snprintf(url, sizeof(url), "%scmd?p=%s&c=volume_get", kRelayBase, kOutputs[sel].pid);
        auto r = GetHAL()->httpGet(url);
        if (r.status == 200) {
            auto pos = r.body.find("\"volume\":");
            if (pos != std::string::npos) {
                int v = atoi(r.body.c_str() + pos + 9);
                if (v >= 0 && v <= 100) _ext_vol.store(v);
            }
        }
        _ext_vol_wait.store(false);
    });
}

// ════════════════════════════════════════════════════════════════════════════
//  UI: home 视图
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    if (!_scr) return;
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG()), 0);
    _buildHomeView();
    _buildPlayView();
}

void AppYt::_buildHomeView()
{
    _home_view = lv_obj_create(_scr);
    if (!_home_view) return;
    lv_obj_set_size(_home_view, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_home_view, 0, 0);
    lv_obj_set_style_bg_opa(_home_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_home_view, 0, 0);
    lv_obj_set_style_pad_all(_home_view, 0, 0);
    lv_obj_clear_flag(_home_view, LV_OBJ_FLAG_SCROLLABLE);

    // 搜索行: textarea (占位 596) + 语音 (170, 与搜索同尺寸) + 搜索 (170) + 输出 chip
    _ta = lv_textarea_create(_home_view);
    if (_ta) {
        lv_obj_set_size(_ta, 596, 56);
        lv_obj_set_pos(_ta, 24, 12);
        lv_obj_set_style_text_font(_ta, gmu::zh20(), 0);
        lv_obj_set_style_text_color(_ta, lv_color_hex(C_TEXT()), 0);
        lv_obj_set_style_text_color(_ta, lv_color_hex(C_DIM()),
                                    LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_bg_color(_ta, lv_color_hex(C_CARD()), 0);
        lv_obj_set_style_border_color(_ta, lv_color_hex(C_TRACK()), 0);
        lv_obj_set_style_border_width(_ta, 2, 0);
        lv_obj_set_style_radius(_ta, 14, 0);
        lv_obj_set_style_pad_hor(_ta, 16, 0);
        lv_obj_set_style_bg_color(_ta, lv_color_hex(C_TRACK()), LV_STATE_FOCUSED);
        lv_textarea_set_placeholder_text(_ta, "搜索 YouTube… (点按弹键盘/语音)");
        lv_textarea_set_one_line(_ta, true);
        lv_obj_clear_flag(_ta, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(_ta, [](lv_event_t* e) {
            static_cast<AppYt*>(lv_event_get_user_data(e))->_showKeyboard();
        }, LV_EVENT_CLICKED, this);
    }
    gmu::action_btn(_home_view, "语音", 632, 12, 170, 56, [](lv_event_t* e) {
        auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
        if (!self || !self->_scr) return;
        self->_voice_retry = 0;
        if (self->_status_lbl) lv_label_set_text(self->_status_lbl, "请说话… (说完停 2 秒)");
        AppVoiceInput::requestVoiceText([self](const std::string& text) {
            if (!self || !self->_scr) return;
            if (text.empty()) {
                if (self->_voice_retry++ < 1 && self->_status_lbl) {
                    lv_label_set_text(self->_status_lbl, "没听清, 自动再试一次…");
                    AppVoiceInput::requestVoiceText([self](const std::string& t2) {
                        if (!self || !self->_scr) return;
                        if (!t2.empty() && self->_ta) {
                            lv_textarea_set_text(self->_ta, t2.c_str());
                            self->_hideKeyboard();
                            self->_searchAsync(t2);
                        } else if (self->_status_lbl) {
                            lv_label_set_text(self->_status_lbl, "没听清, 点麦克风再试");
                        }
                    });
                    return;
                }
                if (self->_status_lbl) lv_label_set_text(self->_status_lbl, "没听清, 点麦克风再试");
                return;
            }
            if (self->_ta) {
                lv_textarea_set_text(self->_ta, text.c_str());
                self->_hideKeyboard();
                self->_searchAsync(text);   // 识别成功直接搜
            }
        });
    }, this);
    gmu::action_btn(_home_view, "搜索", 814, 12, 170, 56, [](lv_event_t* e) {
        auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
        if (self && self->_ta) {
            std::string q = lv_textarea_get_text(self->_ta);
            if (!q.empty()) self->_searchAsync(q);
        }
    }, this, gmu::zh30());

    // 输出选择 chip (播放页同款控件, 点击弹三选弹窗)
    _out_chip_home = lv_button_create(_home_view);
    if (_out_chip_home) {
        lv_obj_set_size(_out_chip_home, 214, 56);
        lv_obj_set_pos(_out_chip_home, 1004, 12);
        lv_obj_set_style_bg_color(_out_chip_home, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_radius(_out_chip_home, 14, 0);
        lv_obj_clear_flag(_out_chip_home, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(_out_chip_home, [](lv_event_t* e) {
            static_cast<AppYt*>(lv_event_get_user_data(e))->_openOutputModal();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* ic = lv_label_create(_out_chip_home);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(C_ACC()), 0);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t* tl = lv_label_create(_out_chip_home);
        lv_obj_set_style_text_font(tl, gmu::zh20(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT()), 0);
        lv_label_set_text(tl, kOutputs[_output_sel].name);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 44, 0);
        lv_obj_set_user_data(_out_chip_home, tl);
    }

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

    // 结果列表 (两列卡片网格, 纵向滚动)
    _grid = lv_obj_create(_home_view);
    if (_grid) {
        lv_obj_set_size(_grid, SCREEN_W - 24, SCREEN_H - 170);
        lv_obj_set_pos(_grid, 12, 168);
        lv_obj_set_style_bg_opa(_grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_grid, 0, 0);
        lv_obj_set_style_pad_all(_grid, 0, 0);
        lv_obj_set_style_pad_row(_grid, 10, 0);
        lv_obj_set_style_pad_column(_grid, 12, 0);
        lv_obj_set_flex_flow(_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_add_flag(_grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(_grid, LV_DIR_VER);
    }

    // ── 屏幕虚拟键盘 (idea 同款: 点 textarea 弹出, READY=回车收起+搜索) ──
    _keyboard = lv_keyboard_create(_scr);
    if (_keyboard) {
        lv_obj_set_size(_keyboard, SCREEN_W, 340);
        lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(_keyboard, _ta);
        lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
            auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
            self->_hideKeyboard();
            if (self && self->_ta) {
                std::string q = lv_textarea_get_text(self->_ta);
                if (!q.empty()) self->_searchAsync(q);
            }
        }, LV_EVENT_READY, this);
        lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
            static_cast<AppYt*>(lv_event_get_user_data(e))->_hideKeyboard();
        }, LV_EVENT_CANCEL, this);
    }
}

// 结果卡片: 两列 (616×200): 左缩略图 284×160 (320×180 源 scale 142), 右标题/频道
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
        lv_obj_t* card = lv_obj_create(_grid);
        if (!card) return;
        lv_obj_set_size(card, 616, 200);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD()), 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)i);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
            int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
            if (self && self->_scr) self->_play(i);
        }, LV_EVENT_CLICKED, this);

        // 0 号子对象: 缩略图占位 (284×160 黑框, 到货换真图 contain 居中)
        lv_obj_t* img = lv_image_create(card);
        if (img) {
            lv_obj_set_pos(img, 10, 20);
            lv_obj_set_size(img, 284, 160);
            lv_obj_set_style_bg_color(img, lv_color_hex(C_TRACK()), 0);
            lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(img, 8, 0);
            lv_obj_set_style_border_width(img, 0, 0);
            lv_obj_set_style_clip_corner(img, true, 0);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_image_set_scale(img, 142);                    // 320×180 → 284×160
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
        }
        // 标题 (zh24, 3 行)
        lv_obj_t* t = lv_label_create(card);
        if (t) {
            lv_label_set_text(t, items[i].t.c_str());
            lv_obj_set_style_text_font(t, gmu::zh24(), 0);
            lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT()), 0);
            lv_obj_set_pos(t, 306, 14);
            lv_obj_set_width(t, 616 - 306 - 12);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        }
        // 频道 · 时长 (zh20, 频道打点截断)
        lv_obj_t* s = lv_label_create(card);
        if (s) {
            char b[96];
            snprintf(b, sizeof(b), "%s · %d:%02d", items[i].c.c_str(),
                     items[i].d / 60, items[i].d % 60);
            lv_label_set_text(s, b);
            lv_obj_set_style_text_font(s, gmu::zh20(), 0);
            lv_obj_set_style_text_color(s, lv_color_hex(C_DIM()), 0);
            lv_obj_set_pos(s, 306, 166);
            lv_obj_set_width(s, 616 - 306 - 12);
            lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        }
        _rows.push_back(card);
        _thumb_bufs.push_back(nullptr);
    }
}

void AppYt::_showKeyboard()
{
    if (!_keyboard || !_ta) return;
    if (GetHAL()->usbKeyboardDetect()) return;   // 实体键盘直打, 不弹屏 (idea/lora 同款)
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_keyboard);
}

void AppYt::_hideKeyboard()
{
    if (!_keyboard) return;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void AppYt::_detachInput()
{
    // 硬键盘 group / 语音回调都指向本屏 textarea, 拆屏前必须摘干净
#ifndef PLATFORM_BUILD_DESKTOP
    if (GetHAL()->lvKbGroup && _ta) lv_group_remove_obj(_ta);
    GetHAL()->onEnterPressed = nullptr;
#endif
    AppVoiceInput::cancelVoiceText();
}

// ════════════════════════════════════════════════════════════════════════════
//  UI: play 视图
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_buildPlayView()
{
    _play_view = lv_obj_create(_scr);
    if (!_play_view) return;
    lv_obj_set_size(_play_view, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_play_view, 0, 0);
    lv_obj_set_style_bg_opa(_play_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_play_view, 0, 0);
    lv_obj_set_style_pad_all(_play_view, 0, 0);
    lv_obj_clear_flag(_play_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_play_view, LV_OBJ_FLAG_HIDDEN);

    // 大封面 640×360 (左上; 固定框 + STRETCH: 占位 8×8/大图都不改变框尺寸)
    _play_thumb = lv_image_create(_play_view);
    if (_play_thumb) {
        lv_image_set_src(_play_thumb, yt_placeholder());
        lv_image_set_inner_align(_play_thumb, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_set_pos(_play_thumb, 24, 16);
        lv_obj_set_size(_play_thumb, COVER_W, COVER_H);
        lv_obj_set_style_radius(_play_thumb, 12, 0);
        lv_obj_set_style_clip_corner(_play_thumb, true, 0);
    }

    // 右栏: 标题 (zh30, 3 行) / 频道·时长 / 输出 chip
    _play_title = lv_label_create(_play_view);
    if (_play_title) {
        lv_obj_set_style_text_font(_play_title, gmu::zh30(), 0);
        lv_obj_set_style_text_color(_play_title, lv_color_hex(C_TEXT()), 0);
        lv_label_set_long_mode(_play_title, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_play_title, SCREEN_W - COVER_W - 96);
        lv_obj_set_pos(_play_title, COVER_W + 48, 16);
    }
    _play_sub = lv_label_create(_play_view);
    if (_play_sub) {
        lv_obj_set_style_text_font(_play_sub, gmu::zh20(), 0);
        lv_obj_set_style_text_color(_play_sub, lv_color_hex(C_DIM()), 0);
        lv_label_set_long_mode(_play_sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(_play_sub, SCREEN_W - COVER_W - 96);
        lv_obj_set_pos(_play_sub, COVER_W + 48, 156);
    }
    _out_chip_play = lv_button_create(_play_view);
    if (_out_chip_play) {
        lv_obj_set_size(_out_chip_play, 460, 60);
        lv_obj_set_pos(_out_chip_play, COVER_W + 48, 196);
        lv_obj_set_style_bg_color(_out_chip_play, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_radius(_out_chip_play, 14, 0);
        lv_obj_clear_flag(_out_chip_play, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(_out_chip_play, [](lv_event_t* e) {
            static_cast<AppYt*>(lv_event_get_user_data(e))->_openOutputModal();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* ic = lv_label_create(_out_chip_play);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(C_ACC()), 0);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t* tl = lv_label_create(_out_chip_play);
        lv_obj_set_style_text_font(tl, gmu::zh24(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT()), 0);
        lv_label_set_text(tl, kOutputs[_output_sel].name);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 52, 0);
        lv_obj_set_user_data(_out_chip_play, tl);
        lv_obj_t* ch = lv_label_create(_out_chip_play);
        lv_obj_set_style_text_font(ch, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ch, lv_color_hex(C_DIM()), 0);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    // 状态行 (全宽, 进度条上方)
    _play_status = lv_label_create(_play_view);
    if (_play_status) {
        lv_obj_set_style_text_font(_play_status, gmu::zh20(), 0);
        lv_obj_set_style_text_color(_play_status, lv_color_hex(C_ACC()), 0);
        lv_label_set_long_mode(_play_status, LV_LABEL_LONG_DOT);
        lv_obj_set_width(_play_status, SCREEN_W - 200);
        lv_obj_set_pos(_play_status, 24, 392);
        lv_label_set_text(_play_status, "");
    }

    // 进度条 + 时间 (zh24)
    _play_bar = lv_slider_create(_play_view);
    if (_play_bar) {
        lv_obj_set_size(_play_bar, SCREEN_W - 400, 14);
        lv_obj_set_pos(_play_bar, 24, 428);
        lv_slider_set_range(_play_bar, 0, 1000);
        lv_obj_set_style_bg_color(_play_bar, lv_color_hex(C_TRACK()), 0);
        lv_obj_set_style_bg_color(_play_bar, lv_color_hex(C_ACC()), LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(_play_bar, 6, LV_PART_KNOB);
        lv_obj_clear_flag(_play_bar, LV_OBJ_FLAG_CLICKABLE);   // 纯显示 (v1)
    }
    _play_el = lv_label_create(_play_view);
    if (_play_el) {
        lv_obj_set_style_text_font(_play_el, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(_play_el, lv_color_hex(C_TEXT()), 0);
        lv_label_set_text(_play_el, "0:00");
        lv_obj_set_pos(_play_el, _play_bar ? (int)lv_obj_get_x(_play_bar) : 24, 452);
    }
    _play_dur = lv_label_create(_play_view);
    if (_play_dur) {
        lv_obj_set_style_text_font(_play_dur, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(_play_dur, lv_color_hex(C_DIM()), 0);
        lv_label_set_text(_play_dur, "0:00");
        lv_obj_align(_play_dur, LV_ALIGN_TOP_RIGHT, -36, 452);
    }

    // 控制行: 播放/暂停 (200×100 大键) + 音量 (图标 + 滑条 640 + 数字)
    _pp_btn = lv_button_create(_play_view);
    if (_pp_btn) {
        lv_obj_set_size(_pp_btn, 200, 100);
        lv_obj_set_pos(_pp_btn, 24, 500);
        lv_obj_set_style_bg_color(_pp_btn, lv_color_hex(C_ACC()), 0);
        lv_obj_set_style_radius(_pp_btn, 20, 0);
        lv_obj_add_event_cb(_pp_btn, [](lv_event_t* e) {
            static_cast<AppYt*>(lv_event_get_user_data(e))->_togglePause();
        }, LV_EVENT_CLICKED, this);
        _pp_lbl = lv_label_create(_pp_btn);
        if (_pp_lbl) {
            lv_obj_set_style_text_font(_pp_lbl, &lv_font_montserrat_40, 0);
            lv_obj_set_style_text_color(_pp_lbl, lv_color_hex(0x000000), 0);
            lv_label_set_text(_pp_lbl, LV_SYMBOL_PAUSE);
            lv_obj_center(_pp_lbl);
        }
    }
    lv_obj_t* vic = lv_label_create(_play_view);
    if (vic) {
        lv_obj_set_style_text_font(vic, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(vic, lv_color_hex(C_TEXT()), 0);
        lv_label_set_text(vic, LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_pos(vic, 300, 536);
    }
    _vol_slider = lv_slider_create(_play_view);
    if (_vol_slider) {
        lv_obj_set_size(_vol_slider, 640, 14);
        lv_obj_set_pos(_vol_slider, 356, 548);
        lv_slider_set_range(_vol_slider, 0, 100);
        lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_TRACK()), 0);
        lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_ACC()), LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(_vol_slider, 8, LV_PART_KNOB);
        lv_obj_add_event_cb(_vol_slider, [](lv_event_t* e) {
            auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
            if (!self) return;
            int v = lv_slider_get_value(self->_vol_slider);
            if (self->_vol_lbl) {
                char b[16];
                snprintf(b, sizeof(b), "%d", v);
                lv_label_set_text(self->_vol_lbl, b);
            }
            if (self->_output_sel > 0 && self->_ext_dev.load() >= 0) {
                int dev = self->_ext_dev.load();
                GetHAL()->tryRunDetached([dev, v]() {
                    AppYt::_relayCmd(kOutputs[dev].pid, "volume_set", v);
                });
            } else {
#ifndef PLATFORM_BUILD_DESKTOP
                xiaozhi_set_speaker_volume(v);   // 本机: 立即生效 (无缓冲)
#endif
            }
        }, LV_EVENT_VALUE_CHANGED, this);
    }
    _vol_lbl = lv_label_create(_play_view);
    if (_vol_lbl) {
        lv_obj_set_style_text_font(_vol_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(_vol_lbl, lv_color_hex(C_TEXT()), 0);
        lv_label_set_text(_vol_lbl, "80");
        lv_obj_set_pos(_vol_lbl, _vol_slider ? (int)lv_obj_get_x(_vol_slider) + 656 : 1012, 540);
    }

    // 停止/返回列表 (次按钮, 右侧)
    gmu::action_btn(_play_view, "列表", 1040, 500, 216, 100, [](lv_event_t* e) {
        static_cast<AppYt*>(lv_event_get_user_data(e))->_stopPlayback();
        auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
        if (self && self->_scr) {
            LvglLockGuard lock;
            self->_showView(false);
        }
    }, this, gmu::zh30());
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

void AppYt::_fillPlayPage(int i)
{
    if (i < 0 || i >= (int)_items.size()) return;
    if (_play_title) {
        lv_label_set_text(_play_title, _items[i].t.c_str());
    }
    if (_play_sub) {
        char b[96];
        snprintf(b, sizeof(b), "%s · %d:%02d", _items[i].c.c_str(),
                 _items[i].d / 60, _items[i].d % 60);
        lv_label_set_text(_play_sub, b);
    }
    // 封面: 先落列表小图 (scale 284/320=228), 大图到货替换
    if (_play_thumb) {
        lv_image_set_src(_play_thumb,
                         _thumb_bufs[i] ? _thumb_bufs[i] : yt_placeholder());
        lv_image_set_inner_align(_play_thumb, LV_IMAGE_ALIGN_STRETCH);
    }
    if (_play_status) lv_label_set_text(_play_status, "连接中… 首次约 15-40s, 请稍候");
    if (_play_bar) lv_slider_set_value(_play_bar, 0, LV_ANIM_OFF);
    if (_play_el) lv_label_set_text(_play_el, "0:00");
    if (_play_dur) {
        char b[16];
        snprintf(b, sizeof(b), "%d:%02d", _items[i].d / 60, _items[i].d % 60);
        lv_label_set_text(_play_dur, b);
    }
    if (_vol_slider) {
        int v = _output_sel > 0 ? 50
                                : atoi(GetHAL()->getConfig("speaker_volume", "80").c_str());
        if (v <= 0) v = 80;
        lv_slider_set_value(_vol_slider, v, LV_ANIM_OFF);
        if (_vol_lbl) {
            char b[16];
            snprintf(b, sizeof(b), "%d", v);
            lv_label_set_text(_vol_lbl, b);
        }
    }
    _refreshPlayBtn();
    _fetchCover(i);
}

void AppYt::_fetchCover(int i)
{
    if (i < 0 || i >= (int)_items.size()) return;
    const auto gen = _cover_gen.fetch_add(1) + 1;
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, gen, i]() {
        auto* hal = GetHAL();
        std::string base = "http://" + hal->getConfig("svc_host", "192.168.1.200") + ":8766";
        void* buf = rgb565::alloc(COVER_W, COVER_H);
        if (!buf) return;
        char url[160];
        snprintf(url, sizeof(url), "%s/yt/thumb/%s.rgb565?w=%d&h=%d",
                 base.c_str(), _items[i].id.c_str(), COVER_W, COVER_H);
        if (!rgb565::download(url, buf)) {
            rgb565::release(buf);
            return;
        }
        std::lock_guard<std::mutex> lk(_cover_mu);
        if (gen != _cover_gen.load()) {
            rgb565::release(buf);
            return;
        }
        _cover_ready = buf;
        _cover_ready_gen = gen;
    }, 16 * 1024);
}

void AppYt::_updatePlayPage()
{
    const int st = _state.load();
    const bool ext = _output_sel > 0 && _ext_on.load();
    // 已播秒数: 本机 = 已喂字节 / 32000; 外放 = 墙钟 − 暂停累计
    int64_t el_s;
    if (ext) {
        int64_t t = _ext_t0_ms.load();
        el_s = t ? (yt_mono_ms() - t - _ext_pause_acc_ms.load()
                    - (_paused.load() ? yt_mono_ms() - _ext_pause_start_ms.load() : 0))
                   / 1000 : 0;
        if (el_s < 0) el_s = 0;
    } else {
        el_s = _pcm_bytes.load() / PCM_BYTES_PER_SEC;
    }
    const int dur = (_cur_item >= 0 && _cur_item < (int)_items.size()) ? _items[_cur_item].d : 0;

    if (_play_status) {
        char sbuf[128];
        const char* s = nullptr;
        if (st == 1) s = _paused.load() ? "" : "连接中… 首次约 15-40s, 请稍候";
        else if (st == 2) {
            if (_paused.load()) s = "已暂停";
            else {
                snprintf(sbuf, sizeof(sbuf), "播放中 · %s", kOutputs[_output_sel].name);
                s = sbuf;
            }
        }
        else if (st == 4) s = "播完了";
        else if (st == 3) {
            const int code = _err_code.load();
            if (code == 429) s = "服务器忙 (上一首还在准备/点太快), 稍等几秒再点";
            else if (code == 502 || code == 503) s = "服务器取不到这个视频, 换一个试试";
            else if (code == 1502) s = "音箱启动失败, 检查后稍候再试 (仍选此输出)";
            else if (code == -1) s = "连不上服务器, 稍后再试";
            else {
                snprintf(sbuf, sizeof(sbuf), "播放失败 (%d), 点「列表」返回重选", code);
                s = sbuf;
            }
        }
        if (s && strcmp(lv_label_get_text(_play_status), s) != 0)
            lv_label_set_text(_play_status, s);
    }
    if (_play_bar && dur > 0) {
        int pm = (int)(el_s * 1000 / dur);
        if (pm > 1000) pm = 1000;
        lv_slider_set_value(_play_bar, pm, LV_ANIM_OFF);
    }
    if (_play_el) {
        char b[24];
        snprintf(b, sizeof(b), "%lld:%02lld", (long long)(el_s / 60), (long long)(el_s % 60));
        if (strcmp(lv_label_get_text(_play_el), b) != 0) lv_label_set_text(_play_el, b);
    }

    // 本机暂停时 TCP 已背压, 字节不走 — 无需其它处理; 外放暂停墙钟已扣
    (void)ext;
}

// ════════════════════════════════════════════════════════════════════════════
//  输出选择弹窗 (radio 同款: 遮罩 + 三行卡)
// ════════════════════════════════════════════════════════════════════════════
void AppYt::_refreshOutputUi()
{
    for (lv_obj_t* chip : { _out_chip_home, _out_chip_play }) {
        if (!chip) continue;
        auto* tl = static_cast<lv_obj_t*>(lv_obj_get_user_data(chip));
        if (tl && _output_sel >= 0 && _output_sel < kOutputCount)
            lv_label_set_text(tl, kOutputs[_output_sel].name);
    }
}

void AppYt::_openOutputModal()
{
    if (_out_modal || !_scr) return;
    LvglLockGuard lock;
    lv_obj_t* bg = lv_obj_create(lv_layer_top());
    if (!bg) return;
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_size(bg, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bg, [](lv_event_t* e) {
        static_cast<AppYt*>(lv_event_get_user_data(e))->_closeOutputModal();
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(bg, [](lv_event_t* e) {
        // 任何路径删除 (含失焦 teardown) 都清指针, 防悬垂
        auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
        if (self && self->_out_modal == lv_event_get_target(e)) self->_out_modal = nullptr;
    }, LV_EVENT_DELETE, this);

    lv_obj_t* card = lv_obj_create(bg);
    if (!card) return;
    lv_obj_set_size(card, 560, 500);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_BORDER()), 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // 吞点击 (不透到遮罩)
    lv_obj_add_event_cb(card, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_style_text_font(title, gmu::zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT()), 0);
    lv_label_set_text(title, "播放输出");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    for (int i = 0; i < kOutputCount; i++) {
        lv_obj_t* row = lv_obj_create(card);
        if (!row) break;
        lv_obj_set_pos(row, 40, 84 + i * 128);
        lv_obj_set_size(row, 480, 108);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == _output_sel ? C_ACC() : C_TRACK()), 0);
        lv_obj_set_style_bg_opa(row, i == _output_sel ? LV_OPA_40 : LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, i == _output_sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(i == _output_sel ? C_ACC() : C_BORDER()), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = static_cast<AppYt*>(lv_event_get_user_data(e));
            int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
            if (self) self->_setOutput(i);
        }, LV_EVENT_CLICKED, this);

        lv_obj_t* ic = lv_label_create(row);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(i == _output_sel ? C_ACC() : C_DIM()), 0);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t* nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, gmu::zh30(), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT()), 0);
        lv_label_set_text(nm, kOutputs[i].full);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 76, 0);

        if (i == _output_sel) {
            lv_obj_t* ck = lv_label_create(row);
            lv_obj_set_style_text_font(ck, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(ck, lv_color_hex(C_ACC()), 0);
            lv_label_set_text(ck, LV_SYMBOL_OK);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -24, 0);
        }
    }
    _out_modal = bg;
}

void AppYt::_closeOutputModal()
{
    if (_out_modal) {
        lv_obj_delete(_out_modal);   // LV_EVENT_DELETE 回调已清 _out_modal
        _out_modal = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  手势 (上拨退出回工具页; 列表滚动共存 = lottery 同款滚动位判定)
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
                    ctx->scroll0 = ctx->app->_gridScrollY();
                    return;
                }
                if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
                    if (ctx->app->_out_modal) return;               // 弹窗期间屏蔽
                    if (lv_screen_active() != ctx->app->_scr) return;
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    if (std::abs(ctx->app->_gridScrollY() - ctx->scroll0) > 10) return;
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
                if (ctx->app->_out_modal) return;
                if (lv_screen_active() != ctx->app->_scr) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    if (ctx->press_y <= 650) return;
                    if (std::abs(ctx->app->_gridScrollY() - ctx->scroll0) > 10) return;
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
std::vector<void*> AppYt::_takeImageBufs()
{
    // 在 nullUiPtrs 之前调用: 摘下全部 rgb565 缓冲 (缩略图+大封面+worker 交付),
    // 调用方在屏对象删除 (async) 之后 release。lv_image 持 raw 指针, 必须先断引用。
    std::vector<void*> out;
    for (void* b : _thumb_bufs)
        if (b) out.push_back(b);
    _thumb_bufs.clear();
    {
        std::lock_guard<std::mutex> lk(_thumb_mu);
        for (auto& p : _thumb_ready)
            if (p.second) out.push_back(p.second);
        _thumb_ready.clear();
    }
    if (_cover_buf) {
        out.push_back(_cover_buf);
        _cover_buf = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(_cover_mu);
        if (_cover_ready) {
            out.push_back(_cover_ready);
            _cover_ready = nullptr;
        }
    }
    return out;
}

void AppYt::_nullUiPtrs()
{
    _home_view = _play_view = _ta = _keyboard = _hist_row = _grid = _status_lbl = nullptr;
    for (auto& r : _rows) r = nullptr;
    _rows.clear();
    _out_chip_home = _out_chip_play = nullptr;
    _play_thumb = _play_title = _play_sub = _play_status = nullptr;
    _play_bar = _play_el = _play_dur = nullptr;
    _pp_btn = _pp_lbl = nullptr;
    _vol_slider = _vol_lbl = nullptr;
}

void AppYt::_destroyUi()
{
    _removeSwipeGesture();
    _detachInput();
    _closeOutputModal();      // lv_layer_top 弹窗不在 _scr 子树, 先删
    // 停播 + 作废在途 worker (关 app = 停播, car 同语义)
    _app_closing.store(true);
    _stopPlayback();
    _thumb_gen.fetch_add(1);
    _cover_gen.fetch_add(1);
    std::vector<void*> bufs = _takeImageBufs();
    lv_obj_t* scr = _scr;
    _nullUiPtrs();
    _scr = nullptr;
    if (scr) lv_obj_delete(scr);
    for (void* b : bufs) rgb565::release(b);   // 屏已删, 引用已断
}

// ════════════════════════════════════════════════════════════════════════════
//  流任务 (设备端本机播放; 桌面无流引擎) — 失败/播完绝不自动重连
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
        int64_t last_attempt_ms = 0;

        // 失败/播完统一收尾: **绝不自动重连** (11-06 两次 Pi 卡死教训)。
        // _want 从「这一条」CAS 回 -1 (期间用户已点别条则尊重新选择)。
        auto finish = [&](int st, int code, const char* why) {
            yt_teardown(http);
            _err_code.store(code);
            _state.store(st);
            int exp = cur;
            _want.compare_exchange_strong(exp, -1);
            if (st == 3) mclog::tagWarn(TAG, "pcm fail: {} (code={})", why, code);
            else mclog::tagInfo(TAG, "pcm end: {}", why);
            cur = -1;
        };

        while (!_app_closing.load()) {
            const int want = _want.load();
            if (want != cur) {
                yt_teardown(http);
                cur = want;
                odd_carry = 0;
                have_odd = false;
                stall = 0;
                if (cur < 0) {                   // 用户停止
                    _state.store(0);
                    vTaskDelay(pdMS_TO_TICKS(80));
                    continue;
                }
                std::string fid;
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    if (cur >= 0 && cur < (int)_items.size()) fid = _items[cur].id;
                }
                if (fid.empty()) {
                    finish(3, -3, "item gone");
                    continue;
                }
                // 两次建连最少间隔 3s (等待中仍可被停止/换选打断)
                while (!_app_closing.load() && _want.load() == cur &&
                       yt_mono_ms() - last_attempt_ms < 3000) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (_app_closing.load() || _want.load() != cur) {
                    cur = -2;
                    continue;
                }
                last_attempt_ms = yt_mono_ms();

                // 播放期关 WiFi 省电 (radio 实锤: 省电 → PCM 供不上卡顿)
                esp_wifi_set_ps(WIFI_PS_NONE);
                char url[192];
                snprintf(url, sizeof(url), "%s/yt/pcm/%s", base.c_str(), fid.c_str());
                esp_http_client_config_t hcfg = {};
                hcfg.url          = url;
                hcfg.buffer_size  = 4096;
                hcfg.buffer_size_tx = 1024;
                // 服务端建链 = resolve + 下载头 + 首段 (冷 15-60s): 给 90s
                hcfg.timeout_ms   = 90000;
                http = esp_http_client_init(&hcfg);
                int sc = -1, first = 0;
                if (http && esp_http_client_open(http, 0) == ESP_OK && !_app_closing.load()) {
                    esp_http_client_fetch_headers(http);
                    sc = esp_http_client_get_status_code(http);
                    if (sc == 200) first = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
                }
                if (_want.load() != cur) {       // 建链期间用户已停止/换选: 丢弃本次
                    yt_teardown(http);
                    cur = -2;
                    continue;
                }
                if (sc != 200 || first <= 0) {
                    finish(3, sc != 200 ? sc : -2, sc != 200 ? "http status" : "empty stream");
                    continue;
                }
                if (!s_yt_ring)
                    s_yt_ring = (uint8_t*)heap_caps_malloc(kYtRingSize,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!s_yt_ring) {
                    finish(3, -4, "ring alloc");
                    continue;
                }
                s_yt_rs.prev = 0;
                s_yt_rs.phase = 0;
                yt_feed_pcm(inbuf, first, odd_carry, have_odd);
                _pcm_bytes.fetch_add(first);
                mclog::tagInfo(TAG, "pcm up: sc={} first={}B", sc, first);
                esp_http_client_set_timeout_ms(http, 1000);   // 感知停止/断流
                _err_code.store(0);
                _state.store(2);
            }

            if (!http) { vTaskDelay(pdMS_TO_TICKS(80)); continue; }

            // 本机暂停: 停读不拆流 — TCP 背压 → 服务端 ffmpeg stdin 写阻塞 → 真暂停,
            // 续播零断点; 服务端 _touch_loop 保证暂停 >90s 管线不被看门狗杀
            if (_paused.load() && _want.load() == cur) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }

            int n = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
            if (n == 0) {                        // 服务端关流 = 播完 (或被新会话踢)
                s_yt_armed = true;
                yt_ring_drain();                 // 环内余量 (≤4s) 播完再收尾
                finish(4, 0, "server closed");
                continue;
            }
            if (n < 0) {
                if (n != -ESP_ERR_HTTP_EAGAIN) {
                    finish(3, -5, "read error");
                    continue;
                }
                yt_ring_drain();                 // 断流期环内余量续播
                if (++stall >= 20) finish(3, -6, "stall 20s");   // 连续 ~20s 无数据
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
