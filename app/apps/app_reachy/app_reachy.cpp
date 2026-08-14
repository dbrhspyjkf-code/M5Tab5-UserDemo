// SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
// SPDX-License-Identifier: MIT
#include "app_reachy.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <lvgl.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <cstdio>
#include <utility>

static const std::string _tag = "reachy";

// ── Chinese fonts (same pattern as app_home / app_settings) ─────────────────
// GB2312 common ~3500 glyphs, XIP from flash on device. Desktop sim falls
// back to font_puhui_20_4 linked from xiaozhi-fonts. All visible labels in
// this app use these — never raw montserrat (Chinese glyphs box otherwise).
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
extern const uint8_t font_puhui_common_20_4_bin_start[]
    asm("_binary_font_puhui_common_20_4_bin_start");
static const lv_font_t* zh_font_lg() {
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_sm() {
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_20_4_bin_start);
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif

// ── Forward decls ─────────────────────────────────────────────────────────
struct TabCtx {
    class AppReachy* self;
    int idx;
};

// ── Helpers ────────────────────────────────────────────────────────────────
static lv_obj_t* _kv_row(lv_obj_t* page, int y, const char* label,
                          lv_obj_t** value_out, int card_h = 56,
                          int value_x = 220, int value_w = 900) {
    lv_obj_t* card = lv_obj_create(page);
    lv_obj_set_size(card, 1180, card_h);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B3552), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xB8C8DA), 0);
    lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);

    lv_obj_t* val = lv_label_create(card);
    lv_label_set_text(val, "—");
    lv_obj_align(val, LV_ALIGN_LEFT_MID, value_x, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(val, zh_font_lg(), 0);
    lv_obj_set_width(val, value_w);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);

    if (value_out) *value_out = val;
    return val;
}

static std::string _format_uptime(int s) {
    if (s <= 0) return "—";
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%dh %02dm %02ds", h, m, sec);
    return buf;
}

static std::string _bool_zh(bool v) { return v ? "是" : "否"; }

// ── Lifecycle ──────────────────────────────────────────────────────────────
AppReachy::AppReachy() {
    setAppInfo().name = "Reachy";
}

void AppReachy::onCreate() {
    // nothing — HAL/UI live on onOpen.
}

void AppReachy::onOpen() {
    if (_scr) {
        lv_screen_load(_scr);
        _installSwipeGesture();
    } else {
        _buildUI();
    }
    _last_poll_ms = 0;  // force a fetch on the first onRunning tick
}

void AppReachy::onClose() {
    _removeSwipeGesture();
    if (_close_cb) _close_cb();
    // LVGL screen is destroyed by the framework when we close the app.
    _scr = nullptr;
}

void AppReachy::onRunning() {
    // Mooncake keeps calling onRunning() for installed apps even after another
    // app has taken the screen. Never poll or touch this app's LVGL tree while
    // it is in the background.
    if (!_scr || lv_screen_active() != _scr) return;

    // 1) Drain any completed background fetch into the UI.
    bool fetched_now = _fetched_ok.load();
    uint32_t fetched_at = _fetched_at_ms.load();
    if (fetched_now && fetched_at != 0 && _rendered_at_ms != fetched_at) {
        // Re-render the active tab from the cache.
        switch (_active) {
        case Tab::Status: _renderStatus(); break;
        case Tab::Motion: _renderMotion(); break;
        case Tab::Audio:  _renderAudio();  break;
        case Tab::Control: _renderControl(); break;
        case Tab::System: _renderSystem(); break;
        case Tab::Logs:   _renderLogs();   break;
        default: break;
        }
        _refreshHeader();
        _rendered_at_ms = fetched_at;
    }

    // 2) Maybe kick a new fetch (every POLL_MS, or the first time).
    uint32_t now = GetHAL()->millis();
    if (_operation_pending.load() || _last_poll_ms == 0 || (now - _last_poll_ms) >= POLL_MS) {
        _last_poll_ms = now;
        if (_fetch_inflight.load()) return;  // skip — already running
        _refreshActiveTab();
    }
}

// ── UI build ───────────────────────────────────────────────────────────────
void AppReachy::_buildUI() {
    _scr = lv_obj_create(nullptr);
    lv_obj_set_size(_scr, W, H);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scr, 0, 0);
    lv_obj_set_style_pad_all(_scr, 0, 0);

    _buildHeader();
    _buildTabBar();

    // Build all pages up-front; _active tab is unhidden last.
    _buildStatusPage();
    _buildMotionPage();
    _buildAudioPage();
    _buildControlPage();
    _buildChatPage();
    _buildSystemPage();
    _buildLogsPage();

    _switchTab(Tab::Status);
    lv_scr_load(_scr);
    _installSwipeGesture();
}

void AppReachy::_requestClose() {
    if (_close_cb) _close_cb();
}

void AppReachy::_installSwipeGesture() {
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, _gestureCb, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppReachy::_removeSwipeGesture() {
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, _gestureCb, this);
        _gesture_indev = nullptr;
    }
}

void AppReachy::_buildHeader() {
    lv_obj_t* bar = lv_obj_create(_scr);
    lv_obj_set_size(bar, W, HEADER_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_HEADER), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    lv_obj_t* title = lv_label_create(bar);
    lv_label_set_text(title, "Reachy");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);

    _hdr_host = lv_label_create(bar);
    lv_label_set_text(_hdr_host, ("host: " + reachy_client::_host()).c_str());
    lv_obj_align(_hdr_host, LV_ALIGN_LEFT_MID, 180, 0);
    lv_obj_set_style_text_color(_hdr_host, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_style_text_font(_hdr_host, zh_font_lg(), 0);

    _hdr_state = lv_label_create(bar);
    lv_label_set_text(_hdr_state, "连接中…");
    lv_obj_align(_hdr_state, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_set_style_text_color(_hdr_state, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_text_font(_hdr_state, zh_font_lg(), 0);
}

void AppReachy::_buildTabBar() {
    _tab_bar = lv_obj_create(_scr);
    lv_obj_set_size(_tab_bar, W, TAB_BAR_H);
    lv_obj_set_pos(_tab_bar, 0, HEADER_H);
    lv_obj_set_style_bg_color(_tab_bar, lv_color_hex(C_TAB_BG), 0);
    lv_obj_set_style_border_width(_tab_bar, 0, 0);
    lv_obj_set_style_pad_all(_tab_bar, 0, 0);

    static const char* LABELS[(int)Tab::Count] = {"状态", "运动", "音频", "控制", "聊天", "系统", "日志"};
    int btn_w = W / (int)Tab::Count;
    for (int i = 0; i < (int)Tab::Count; i++) {
        lv_obj_t* b = lv_obj_create(_tab_bar);
        lv_obj_set_size(b, btn_w - 16, TAB_BAR_H - 16);
        lv_obj_set_pos(b, i * btn_w + 8, 8);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_TAB_BG), 0);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        _tab_btns[i] = b;

        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, LABELS[i]);
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT2), 0);
        lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);

        // Bundle the tab index with a self-pointer so _tabCb can route to
        // the right instance. Heap-allocated so the pointer is stable.
        auto* ctx = new TabCtx{this, i};
        lv_obj_add_event_cb(b, _tabCb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(b, [](lv_event_t* e) {
            delete static_cast<TabCtx*>(lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, ctx);
    }
}

void AppReachy::_switchTab(Tab t) {
    if (t == _active) return;
    _active = t;

    for (int i = 0; i < (int)Tab::Count; i++) {
        bool active = (i == (int)t);
        lv_obj_set_style_bg_color(_tab_btns[i],
            lv_color_hex(active ? C_TAB_ACTIVE : C_TAB_BG), 0);
        if (active) {
            lv_obj_set_style_shadow_width(_tab_btns[i], 16, 0);
            lv_obj_set_style_shadow_color(_tab_btns[i], lv_color_hex(C_ACCENT), 0);
            lv_obj_set_style_shadow_opa(_tab_btns[i], LV_OPA_30, 0);
        } else {
            lv_obj_set_style_shadow_width(_tab_btns[i], 0, 0);
        }
        lv_obj_t* lbl = lv_obj_get_child(_tab_btns[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl,
            lv_color_hex(active ? C_ACCENT : C_TEXT2), 0);
        if (_tab_pages[i]) {
            if (active) lv_obj_clear_flag(_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    _refreshActiveTab();
}

void AppReachy::_refreshActiveTab() {
    if (!_scr) return;
    // Spawn a detached worker that does the actual HTTP round-trip. We can't
    // synchronously call httpGet() on the LVGL thread — the LVGL mutex would
    // be held during a 10 s timeout, freezing the UI.
    _fetch_inflight.store(true);
    _fetch_pending.store((int)_active);
    bool ok = GetHAL()->tryRunDetached([this, tab = _active]() {
        Operation operation;
        if (_operation_pending.load()) {
            {
                std::lock_guard<std::mutex> lk(_cache_mutex);
                operation = _pending_operation;
                _pending_operation = {};
                _operation_pending.store(false);
            }
            auto result = _executeOperation(operation);
            {
                std::lock_guard<std::mutex> lk(_cache_mutex);
                _operation_result = std::move(result);
                _operation_result_ready = true;
            }
        }
        bool ping_ok = reachy_client::ping();
        if (!ping_ok) {
            _fetched_ok.store(false);
            _fetched_at_ms.store(GetHAL()->millis());
            _fetch_inflight.store(false);
            return;
        }
        switch (tab) {
        case Tab::Status: {
            auto s = reachy_client::fetchStatus();
            std::lock_guard<std::mutex> lk(_cache_mutex);
            _status = s;
            _has_status = true;
            break;
        }
        case Tab::Motion: {
            auto m = reachy_client::fetchMotion();
            auto s = reachy_client::fetchStatus();
            std::lock_guard<std::mutex> lk(_cache_mutex);
            _status = s;
            _has_status = true;
            _motion = reachy_client::mergeStatusIntoMotion(m, s);
            _has_motion = true;
            break;
        }
        case Tab::Audio:  {
            auto a = reachy_client::fetchAudio();
            std::lock_guard<std::mutex> lk(_cache_mutex);
            _audio = a;
            _has_audio = true;
            break;
        }
        case Tab::Control: {
            auto c = reachy_client::fetchControlState();
            std::lock_guard<std::mutex> lk(_cache_mutex);
            _control = std::move(c);
            _has_control = true;
            break;
        }
        case Tab::Chat: break;
        case Tab::System: {
            auto s = reachy_client::fetchSystemState();
            std::lock_guard<std::mutex> lk(_cache_mutex);
            _sys = s;
            _has_sys = true;
            // We also cached 'status' earlier for the daemon status field;
            // reuse the last good value if status hasn't been fetched yet
            // this session.
            if (!_has_status) {
                auto st = reachy_client::fetchStatus();
                std::lock_guard<std::mutex> lk2(_cache_mutex);
                _status = st;
                _has_status = true;
            }
            break;
        }
        case Tab::Logs: {
            // Logs returns raw JSON; we don't cache it, just expose a refresh
            // button and let the worker pre-parse if needed. For now do
            // nothing here — the Logs tab reads fresh on each refresh.
            break;
        }
        default: break;
        }
        _fetched_ok.store(true);
        _fetched_at_ms.store(GetHAL()->millis());
        _fetch_inflight.store(false);
    });
    if (!ok) {
        // Worker spawn failed (low internal RAM). Reset and try again next tick.
        _fetch_inflight.store(false);
        mclog::tagWarn(_tag, "worker spawn failed; skipping this poll");
    }
}

void AppReachy::_refreshHeader() {
    if (_hdr_state) {
        lv_label_set_text(_hdr_state, _last_ok ? "在线" : "离线");
        lv_obj_set_style_text_color(_hdr_state,
            lv_color_hex(_last_ok ? C_OK : C_ERR), 0);
    }
    if (_hdr_host) {
        lv_label_set_text(_hdr_host, ("host: " + reachy_client::_host()).c_str());
    }
}

void AppReachy::_showOfflineBanner() {
    if (_hdr_state) {
        lv_label_set_text(_hdr_state, "离线");
        lv_obj_set_style_text_color(_hdr_state, lv_color_hex(C_ERR), 0);
    }
    // For active tab, set a "—" marker on the first label so the user sees
    // something. Avoid touching every label — they'd flicker on every poll.
    auto dash = [](lv_obj_t* lbl) {
        if (lbl) lv_label_set_text(lbl, "—");
    };
    switch (_active) {
    case Tab::Status:
        dash(_st_service_state); dash(_st_uptime); dash(_st_backend);
        dash(_st_gateway); dash(_st_video); dash(_st_wake);
        dash(_st_ws_state); dash(_st_cpu); dash(_st_daemon); dash(_st_ha);
        break;
    case Tab::Motion:
        dash(_mo_mode); dash(_mo_current); dash(_mo_queue);
        dash(_mo_loop); dash(_mo_antennas); dash(_mo_gaze); dash(_mo_failures);
        break;
    case Tab::Audio:
        if (_au_volume_lbl) lv_label_set_text(_au_volume_lbl, "—");
        dash(_au_vad_lbl); dash(_au_control); dash(_au_status);
        break;
    case Tab::Control:
        dash(_ct_backend_state); dash(_ct_status);
        break;
    case Tab::Chat:
        break;
    case Tab::System:
        dash(_sy_state); dash(_sy_daemon);
        break;
    case Tab::Logs:
        if (_lg_text) lv_label_set_text(_lg_text, "(无法连接 Reachy)");
        break;
    default: break;
    }
}

// ── STATUS page ────────────────────────────────────────────────────────────
void AppReachy::_buildStatusPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);  // fixed grid, see row count
    _tab_pages[0] = page;

    int y = 0;
    const int RH = 52;      // row card height
    const int GAP = 8;      // vertical gap

    // Row 1: 服务状态 / 运行时
    _kv_row(page, y, "服务", &_st_service_state, RH); y += RH + GAP;
    _kv_row(page, y, "运行时", &_st_uptime, RH); y += RH + GAP;

    // Row 2: 对话后端 / 网关 / 视频
    _kv_row(page, y, "对话后端", &_st_backend, RH); y += RH + GAP;
    _kv_row(page, y, "网关 URL", &_st_gateway, RH); y += RH + GAP;
    _kv_row(page, y, "视频", &_st_video, RH); y += RH + GAP;

    // Row 3: 唤醒 / WS 状态
    _kv_row(page, y, "唤醒", &_st_wake, RH); y += RH + GAP;
    _kv_row(page, y, "WS 状态", &_st_ws_state, RH); y += RH + GAP;

    // Row 4: 系统指标（四列合一）
    _kv_row(page, y, "系统", &_st_cpu, RH); y += RH + GAP;

    // Row 5: Daemon / HA 集成
    _kv_row(page, y, "Daemon", &_st_daemon, RH); y += RH + GAP;
    _kv_row(page, y, "Home Assistant", &_st_ha, RH);
}

void AppReachy::_renderStatus() {
    // Read cache (filled by background fetch in _refreshActiveTab).
    reachy_client::Status s;
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (!_has_status) return;
        s = _status;
    }
    if (!s.ok) { _showOfflineBanner(); return; }
    _last_ok = true;

    auto setv = [](lv_obj_t* lbl, const std::string& t) {
        if (lbl) lv_label_set_text(lbl, t.c_str());
    };
    auto setv_colour = [](lv_obj_t* lbl, const std::string& t, uint32_t c) {
        if (lbl) {
            lv_label_set_text(lbl, t.c_str());
            lv_obj_set_style_text_color(lbl, lv_color_hex(c), 0);
        }
    };

    if (s.service_state == "running" || s.service_state == "active") {
        setv_colour(_st_service_state, s.service_state.empty() ? "—" : s.service_state, C_OK);
    } else {
        setv_colour(_st_service_state, s.service_state.empty() ? "—" : s.service_state, C_WARN);
    }
    char upbuf[32];
    snprintf(upbuf, sizeof(upbuf), "%s (pid %d)", _format_uptime(s.uptime_s).c_str(), s.pid);
    setv(_st_uptime, upbuf);

    setv(_st_backend, s.configured_backend.empty() ? s.runtime_backend : s.configured_backend);
    setv(_st_gateway, s.gateway_url);
    char vidbuf[64];
    if (s.video_enabled && s.video_fps > 0) {
        snprintf(vidbuf, sizeof(vidbuf), "%.1f fps / %d ms", s.video_fps, s.video_interval_ms);
    } else if (s.video_enabled) {
        snprintf(vidbuf, sizeof(vidbuf), "开启");
    } else {
        snprintf(vidbuf, sizeof(vidbuf), "关闭");
    }
    setv(_st_video, vidbuf);

    char wakebuf[64];
    snprintf(wakebuf, sizeof(wakebuf), "%s (%s) %s",
             s.wake_phrase.empty() ? "—" : s.wake_phrase.c_str(),
             _bool_zh(s.wake_enabled).c_str(),
             s.wake_active ? "🟢" : "⚪");
    setv(_st_wake, wakebuf);
    setv_colour(_st_ws_state, s.ws_state.empty() ? "—" : s.ws_state,
                s.ws_state == "connected" ? C_OK : C_WARN);

    char sysbuf[80];
    snprintf(sysbuf, sizeof(sysbuf), "CPU %.1f%%  内存 %.1f%%  磁盘 %.1f%%  温度 %.1f°C",
             s.cpu_percent, s.memory_percent, s.disk_percent, s.temperature_c);
    setv(_st_cpu, sysbuf);

    setv_colour(_st_daemon, s.daemon_state.empty() ? "—" : s.daemon_state,
                s.daemon_available ? C_OK : C_WARN);
    char habuf[64];
    if (s.ha_configured) {
        snprintf(habuf, sizeof(habuf), "%s", s.ha_url.empty() ? "已配置" : s.ha_url.c_str());
    } else if (s.ha_enabled) {
        snprintf(habuf, sizeof(habuf), "未配置 (启用)");
    } else {
        snprintf(habuf, sizeof(habuf), "未启用");
    }
    setv(_st_ha, habuf);
}

// ── MOTION page ────────────────────────────────────────────────────────────
void AppReachy::_buildMotionPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    _tab_pages[1] = page;

    int y = 0;
    const int RH = 52;
    const int GAP = 8;
    _kv_row(page, y, "模式", &_mo_mode, RH); y += RH + GAP;
    _kv_row(page, y, "当前动作", &_mo_current, RH); y += RH + GAP;
    _kv_row(page, y, "命令队列", &_mo_queue, RH); y += RH + GAP;
    _kv_row(page, y, "Loop Hz", &_mo_loop, RH); y += RH + GAP;
    _kv_row(page, y, "天线角度", &_mo_antennas, RH); y += RH + GAP;
    _kv_row(page, y, "注视方向", &_mo_gaze, RH); y += RH + GAP;
    _kv_row(page, y, "Set-target 失败", &_mo_failures, RH);
}

void AppReachy::_renderMotion() {
    reachy_client::Motion m;
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (!_has_motion) return;
        m = _motion;
    }
    if (!m.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    auto setv = [](lv_obj_t* lbl, const std::string& t) {
        if (lbl) lv_label_set_text(lbl, t.c_str());
    };
    setv(_mo_mode, m.mode);
    setv(_mo_current, m.current_move.empty() ? "(无)" : m.current_move);
    char qbuf[16];
    snprintf(qbuf, sizeof(qbuf), "%d", m.command_queue);
    setv(_mo_queue, qbuf);
    char lbuf[24];
    snprintf(lbuf, sizeof(lbuf), "%.2f Hz", m.loop_hz);
    setv(_mo_loop, lbuf);
    setv(_mo_antennas, m.antennas.empty() ? "—" : m.antennas);
    char gbuf[48];
    snprintf(gbuf, sizeof(gbuf), "%s %.2f rad%s",
             m.gaze_source.empty() ? "—" : m.gaze_source.c_str(),
             m.gaze_target_rad,
             m.face_detected ? " (人脸)" : "");
    setv(_mo_gaze, gbuf);
    char fbuf[16];
    snprintf(fbuf, sizeof(fbuf), "%d", m.set_target_consecutive_failures);
    setv(_mo_failures, fbuf);
    if (_mo_failures) {
        lv_obj_set_style_text_color(_mo_failures,
            lv_color_hex(m.set_target_consecutive_failures > 5 ? C_ERR : C_TEXT), 0);
    }
}

// ── AUDIO page ─────────────────────────────────────────────────────────────
void AppReachy::_buildAudioPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    _tab_pages[2] = page;

    auto card = [page](int y, int h) {
        lv_obj_t* obj = lv_obj_create(page);
        lv_obj_set_size(obj, 1180, h);
        lv_obj_set_pos(obj, 0, y);
        lv_obj_set_style_bg_color(obj, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(obj, 16, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    };
    auto label = [](lv_obj_t* parent, const char* text, int x, int y) {
        lv_obj_t* obj = lv_label_create(parent);
        lv_label_set_text(obj, text);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_style_text_color(obj, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(obj, zh_font_lg(), 0);
        return obj;
    };
    auto button = [&](lv_obj_t* parent, const char* text, int x, int y,
                      lv_obj_t** button_out, lv_obj_t** label_out) {
        lv_obj_t* obj = lv_obj_create(parent);
        lv_obj_set_size(obj, 190, 58);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_style_bg_color(obj, lv_color_hex(C_CARD_PR), 0);
        lv_obj_set_style_radius(obj, 12, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* lbl = lv_label_create(obj);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);
        *button_out = obj;
        *label_out = lbl;
    };

    lv_obj_t* volume_card = card(0, 126);
    label(volume_card, "音量", 18, 12);
    _au_volume_lbl = label(volume_card, "—", 150, 12);
    _au_volume_slider = lv_slider_create(volume_card);
    lv_obj_set_size(_au_volume_slider, 850, 20);
    lv_obj_set_pos(_au_volume_slider, 24, 82);
    lv_slider_set_range(_au_volume_slider, 0, 100);
    lv_obj_add_event_cb(_au_volume_slider, _audioEventCb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_au_volume_slider, _audioEventCb, LV_EVENT_RELEASED, this);
    button(volume_card, "静音", 960, 48, &_au_mute_btn, &_au_mute_lbl);
    lv_obj_add_event_cb(_au_mute_btn, _audioEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t* mic_card = card(136, 82);
    label(mic_card, "Mic 输入", 18, 24);
    button(mic_card, "启用", 960, 12, &_au_mic_btn, &_au_mic_lbl);
    lv_obj_add_event_cb(_au_mic_btn, _audioEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t* vad_card = card(228, 126);
    label(vad_card, "VAD 灵敏度", 18, 12);
    _au_vad_lbl = label(vad_card, "—", 220, 12);
    _au_vad_slider = lv_slider_create(vad_card);
    lv_obj_set_size(_au_vad_slider, 1100, 20);
    lv_obj_set_pos(_au_vad_slider, 24, 82);
    lv_slider_set_range(_au_vad_slider, 1, 500);
    lv_obj_add_event_cb(_au_vad_slider, _audioEventCb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_au_vad_slider, _audioEventCb, LV_EVENT_RELEASED, this);

    _kv_row(page, 364, "音量控制", &_au_control, 52);
    _kv_row(page, 426, "操作状态", &_au_status, 52);
}

void AppReachy::_renderAudio() {
    reachy_client::Audio a;
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (!_has_audio) return;
        a = _audio;
    }
    if (!a.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    auto setv = [](lv_obj_t* lbl, const std::string& t) {
        if (lbl) lv_label_set_text(lbl, t.c_str());
    };
    if (a.volume_percent < 0) {
        setv(_au_volume_lbl, a.volume_error.empty() ? "不可用" : a.volume_error);
        if (_au_volume_slider) lv_slider_set_value(_au_volume_slider, 0, LV_ANIM_OFF);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d %%", a.volume_percent);
        setv(_au_volume_lbl, buf);
        if (a.volume_percent > 0) _last_nonzero_volume = a.volume_percent;
        if (_au_volume_slider) lv_slider_set_value(_au_volume_slider, a.volume_percent, LV_ANIM_ON);
    }
    if (_au_mute_lbl) lv_label_set_text(_au_mute_lbl, a.volume_percent == 0 ? "恢复音量" : "静音");
    if (_au_mic_lbl) lv_label_set_text(_au_mic_lbl, a.mic_input_enabled ? "禁用 Mic" : "启用 Mic");
    char vadbuf[32];
    snprintf(vadbuf, sizeof(vadbuf), "%.3f %s",
             a.vad_rms_min,
             a.vad_unit.empty() ? "" : a.vad_unit.c_str());
    setv(_au_vad_lbl, vadbuf);
    if (_au_vad_slider)
        lv_slider_set_value(_au_vad_slider, (int)(a.vad_rms_min * 1000.f + 0.5f), LV_ANIM_ON);
    setv(_au_control, a.volume_control.empty() ? "—" : a.volume_control);
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (_operation_result_ready) {
            setv(_au_status, _operation_result.ok ? "已同步到 Reachy" : _operation_result.error);
            lv_obj_set_style_text_color(_au_status,
                lv_color_hex(_operation_result.ok ? C_OK : C_ERR), 0);
            _operation_result_ready = false;
        }
    }
}

bool AppReachy::_queueOperation(Operation operation) {
    if (_fetch_inflight.load() || _operation_pending.load()) {
        if (_au_status) lv_label_set_text(_au_status, "正在处理，请稍候");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        _pending_operation = std::move(operation);
        _operation_pending.store(true);
    }
    if (_au_status) lv_label_set_text(_au_status, "处理中…");
    _last_poll_ms = 0;
    return true;
}

reachy_client::OperationResult AppReachy::_executeOperation(const Operation& operation) {
    switch (operation.kind) {
    case OperationKind::SetVolume: return reachy_client::setVolume(operation.int_value);
    case OperationKind::SetMic:    return reachy_client::setMicEnabled(operation.bool_value);
    case OperationKind::SetVad:    return reachy_client::setVad(operation.float_value);
    case OperationKind::SetBackend: return _switchBackend(operation.text_value);
    case OperationKind::SetVideo: return reachy_client::setVideoEnabled(operation.bool_value);
    case OperationKind::SetVoice: return reachy_client::setVoice(operation.text_value);
    case OperationKind::SetCamera: return reachy_client::setCameraRunning(operation.bool_value);
    case OperationKind::DaemonWake: return reachy_client::daemonAction("wake");
    case OperationKind::DaemonSleep: return reachy_client::daemonAction("sleep");
    case OperationKind::DaemonRestart: return reachy_client::daemonAction("restart");
    case OperationKind::RestartYRobot: return reachy_client::restartYRobot();
    default: return {false, 0, "无效操作"};
    }
}

reachy_client::OperationResult AppReachy::_switchBackend(const std::string& target) {
    auto saved = reachy_client::setBackend(target);
    if (!saved.ok) return saved;
    auto restarted = reachy_client::restartYRobot();
    if (!restarted.ok) return restarted;
    for (int second = 0; second < 30; ++second) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto status = reachy_client::fetchStatus();
        if (status.ok && status.configured_backend == target && status.runtime_backend == target)
            return {true, 200, ""};
    }
    return {false, 0, "切换超时；未自动回退"};
}

// ── CONTROL page ───────────────────────────────────────────────────────────
void AppReachy::_buildControlPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    _tab_pages[(int)Tab::Control] = page;

    auto button = [this, page](const char* text, int x, int y, int w, uint32_t color) {
        lv_obj_t* b = lv_obj_create(page);
        lv_obj_set_size(b, w, 58);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        auto* lbl = lv_label_create(b);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_style_text_font(lbl, zh_font_lg(), 0);
        lv_obj_add_event_cb(b, _controlEventCb, LV_EVENT_CLICKED, this);
        return b;
    };

    _kv_row(page, 0, "对话后端", &_ct_backend_state, 58, 220, 450);
    _ct_xiaozhi_btn = button("XIAOZHI", 780, 0, 180, C_CARD_PR);
    _ct_qwen_btn = button("QWEN", 980, 0, 180, C_CARD_PR);
    _ct_video_btn = button("视频", 0, 72, 250, C_CARD_PR);
    _ct_video_lbl = lv_obj_get_child(_ct_video_btn, 0);
    _ct_camera_btn = button("摄像头", 270, 72, 250, C_CARD_PR);
    _ct_camera_lbl = lv_obj_get_child(_ct_camera_btn, 0);

    _ct_voice_dropdown = lv_dropdown_create(page);
    lv_obj_set_size(_ct_voice_dropdown, 620, 58);
    lv_obj_set_pos(_ct_voice_dropdown, 540, 72);
    lv_dropdown_set_options(_ct_voice_dropdown, "读取 QWEN 音色…");
    lv_obj_set_style_text_font(_ct_voice_dropdown, zh_font_lg(), 0);
    lv_obj_add_event_cb(_ct_voice_dropdown, _controlEventCb, LV_EVENT_VALUE_CHANGED, this);

    _ct_wake_btn = button("Daemon 唤醒", 0, 148, 280, C_CARD_PR);
    _ct_sleep_btn = button("Daemon 休眠", 300, 148, 280, C_ERR);
    _ct_daemon_restart_btn = button("Daemon 重启", 600, 148, 280, C_ERR);
    _kv_row(page, 224, "操作状态", &_ct_status, 58);
}

void AppReachy::_buildChatPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    _tab_pages[(int)Tab::Chat] = page;
}

void AppReachy::_renderControl() {
    reachy_client::ControlState state;
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (!_has_control) return;
        state = _control;
    }
    if (!state.ok) { _showOfflineBanner(); return; }
    std::string backend = "配置: " + state.configured_backend + " / 运行: " + state.running_backend;
    if (!state.backend_error.empty()) backend += " / 错误: " + state.backend_error;
    lv_label_set_text(_ct_backend_state, backend.c_str());
    lv_label_set_text(_ct_video_lbl, state.video_enabled ? "关闭视频" : "开启视频");
    lv_label_set_text(_ct_camera_lbl, state.camera_running ? "关闭摄像头" : "开启摄像头");
    if (!state.available_voices.empty()) {
        std::string options;
        int selected = 0;
        for (size_t i = 0; i < state.available_voices.size(); ++i) {
            if (!options.empty()) options += "\n";
            options += state.available_voices[i];
            if (state.available_voices[i] == state.configured_voice) selected = (int)i;
        }
        lv_dropdown_set_options(_ct_voice_dropdown, options.c_str());
        lv_dropdown_set_selected(_ct_voice_dropdown, selected);
    }
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (_operation_result_ready) {
            lv_label_set_text(_ct_status,
                _operation_result.ok ? "操作成功" : _operation_result.error.c_str());
            lv_obj_set_style_text_color(_ct_status,
                lv_color_hex(_operation_result.ok ? C_OK : C_ERR), 0);
            _operation_result_ready = false;
        }
    }
}

// ── SYSTEM page ────────────────────────────────────────────────────────────
void AppReachy::_buildSystemPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    _tab_pages[(int)Tab::System] = page;

    int y = 0;
    const int RH = 52;
    const int GAP = 8;

    _kv_row(page, y, "YRobot 状态", &_sy_state, RH); y += RH + GAP;
    _kv_row(page, y, "Daemon", &_sy_daemon, RH); y += RH + GAP;

    // Restart card
    {
        lv_obj_t* card = lv_obj_create(page);
        lv_obj_set_size(card, 1180, 120);
        lv_obj_set_pos(card, 0, y);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1B3552), 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* note = lv_label_create(card);
        lv_label_set_text(note, "重启服务");
        lv_obj_align(note, LV_ALIGN_LEFT_MID, 18, -20);
        lv_obj_set_style_text_color(note, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_text_font(note, zh_font_lg(), 0);

        lv_obj_t* sub = lv_label_create(card);
        lv_label_set_text(sub, "POST /api/system/restart — 短暂中断对话和音频。");
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 18, 20);
        lv_obj_set_style_text_color(sub, lv_color_hex(C_TEXT2), 0);
        lv_obj_set_style_text_font(sub, zh_font_sm(), 0);

        _sy_restart_btn = lv_obj_create(card);
        lv_obj_set_size(_sy_restart_btn, 280, 60);
        lv_obj_align(_sy_restart_btn, LV_ALIGN_RIGHT_MID, -18, 0);
        lv_obj_set_style_bg_color(_sy_restart_btn, lv_color_hex(C_ERR), 0);
        lv_obj_set_style_radius(_sy_restart_btn, 14, 0);
        lv_obj_set_style_border_width(_sy_restart_btn, 0, 0);
        lv_obj_set_style_pad_all(_sy_restart_btn, 0, 0);
        lv_obj_clear_flag(_sy_restart_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_sy_restart_btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* btn_lbl = lv_label_create(_sy_restart_btn);
        lv_label_set_text(btn_lbl, "重启服务");
        lv_obj_center(btn_lbl);
        lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(btn_lbl, zh_font_lg(), 0);

        lv_obj_add_event_cb(_sy_restart_btn, _restartCb, LV_EVENT_CLICKED, this);
    }
}

void AppReachy::_renderSystem() {
    reachy_client::SystemState s;
    bool has_status = false;
    reachy_client::Status   st;
    {
        std::lock_guard<std::mutex> lk(_cache_mutex);
        if (_has_sys)  s = _sys;
        if (_has_status) { st = _status; has_status = true; }
    }
    if (!s.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    auto setv = [](lv_obj_t* lbl, const std::string& t) {
        if (lbl) lv_label_set_text(lbl, t.c_str());
    };
    char sbuf[64];
    snprintf(sbuf, sizeof(sbuf), "%s (%s, pid %d, uptime %s)",
             s.service.empty() ? "—" : s.service.c_str(),
             s.running ? "运行中" : "已停止",
             s.pid,
             _format_uptime(s.uptime_s).c_str());
    setv(_sy_state, sbuf);
    setv(_sy_daemon, has_status ? (st.daemon_state.empty() ? "—" : st.daemon_state)
                                : std::string("—"));
}

void AppReachy::_confirmAndRestart() {
    Operation operation;
    operation.kind = OperationKind::RestartYRobot;
    _confirmOperation(operation, "确认重启 YRobot 服务？");
}

void AppReachy::_confirmOperation(Operation operation, const char* message) {
    _confirmed_operation = std::move(operation);
    // Modal backdrop
    lv_obj_t* bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, W, H);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, _modalBgCb, LV_EVENT_CLICKED, this);

    // Dialog
    lv_obj_t* dlg = lv_obj_create(bg);
    lv_obj_set_size(dlg, 600, 280);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(dlg, 18, 0);
    lv_obj_set_style_border_width(dlg, 0, 0);
    lv_obj_set_style_pad_all(dlg, 24, 0);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = lv_label_create(dlg);
    lv_label_set_text(t, message);
    lv_obj_set_pos(t, 0, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(t, zh_font_lg(), 0);

    lv_obj_t* note = lv_label_create(dlg);
    lv_label_set_text(note, "对话将被中断，机器人头部将复位。\nReachy 大约 5 秒后恢复在线。");
    lv_obj_set_pos(note, 0, 56);
    lv_obj_set_style_text_color(note, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_style_text_font(note, zh_font_lg(), 0);
    lv_obj_set_width(note, 540);

    lv_obj_t* yes = lv_obj_create(dlg);
    lv_obj_set_size(yes, 240, 60);
    lv_obj_set_pos(yes, 0, 200);
    lv_obj_set_style_bg_color(yes, lv_color_hex(C_ERR), 0);
    lv_obj_set_style_radius(yes, 12, 0);
    lv_obj_set_style_border_width(yes, 0, 0);
    lv_obj_set_style_pad_all(yes, 0, 0);
    lv_obj_clear_flag(yes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(yes, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* yes_lbl = lv_label_create(yes);
    lv_label_set_text(yes_lbl, "重启");
    lv_obj_center(yes_lbl);
    lv_obj_set_style_text_color(yes_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(yes_lbl, zh_font_lg(), 0);
    lv_obj_add_event_cb(yes, _confirmYesCb, LV_EVENT_CLICKED, this);

    lv_obj_t* no = lv_obj_create(dlg);
    lv_obj_set_size(no, 240, 60);
    lv_obj_set_pos(no, 280, 200);
    lv_obj_set_style_bg_color(no, lv_color_hex(0x244B7A), 0);
    lv_obj_set_style_radius(no, 12, 0);
    lv_obj_set_style_border_width(no, 0, 0);
    lv_obj_set_style_pad_all(no, 0, 0);
    lv_obj_clear_flag(no, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(no, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* no_lbl = lv_label_create(no);
    lv_label_set_text(no_lbl, "取消");
    lv_obj_center(no_lbl);
    lv_obj_set_style_text_color(no_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(no_lbl, zh_font_lg(), 0);
    lv_obj_add_event_cb(no, _confirmNoCb, LV_EVENT_CLICKED, this);
}

// ── LOGS page ──────────────────────────────────────────────────────────────
void AppReachy::_buildLogsPage() {
    lv_obj_t* page = lv_obj_create(_scr);
    lv_obj_set_size(page, W, H - HEADER_H - TAB_BAR_H);
    lv_obj_set_pos(page, 0, HEADER_H + TAB_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    _tab_pages[(int)Tab::Logs] = page;

    // Log text card
    lv_obj_t* card = lv_obj_create(page);
    lv_obj_set_size(card, 1180, 500);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B3552), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _lg_text = lv_label_create(card);
    lv_label_set_text(_lg_text, "(刷新获取日志)");
    lv_obj_set_pos(_lg_text, 0, 0);
    lv_obj_set_style_text_color(_lg_text, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_lg_text, zh_font_sm(), 0);
    lv_obj_set_width(_lg_text, 1150);
    lv_label_set_long_mode(_lg_text, LV_LABEL_LONG_WRAP);

    _lg_refresh_btn = lv_obj_create(page);
    lv_obj_set_size(_lg_refresh_btn, 180, 50);
    lv_obj_align(_lg_refresh_btn, LV_ALIGN_TOP_RIGHT, 0, 512);
    lv_obj_set_style_bg_color(_lg_refresh_btn, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(_lg_refresh_btn, 12, 0);
    lv_obj_set_style_border_width(_lg_refresh_btn, 0, 0);
    lv_obj_set_style_pad_all(_lg_refresh_btn, 0, 0);
    lv_obj_clear_flag(_lg_refresh_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_lg_refresh_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* btn_lbl = lv_label_create(_lg_refresh_btn);
    lv_label_set_text(btn_lbl, "刷新");
    lv_obj_center(btn_lbl);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0x081522), 0);
    lv_obj_set_style_text_font(btn_lbl, zh_font_lg(), 0);
    lv_obj_add_event_cb(_lg_refresh_btn, _logsRefreshCb, LV_EVENT_CLICKED, this);
}

void AppReachy::_renderLogs() {
    // Logs fetch is heavy — always go async to avoid blocking the LVGL thread.
    if (_fetch_inflight.load()) return;
    _fetch_inflight.store(true);
    if (_lg_text) lv_label_set_text(_lg_text, "(加载中…)");
    bool ok = GetHAL()->tryRunDetached([this]() {
        auto raw = reachy_client::fetchLogsRaw(200);
        if (raw.empty()) {
            _fetched_ok.store(false);
            _fetched_at_ms.store(GetHAL()->millis());
            _fetch_inflight.store(false);
            if (_lg_text) lv_label_set_text(_lg_text, "(无法连接 Reachy 或无日志)");
            return;
        }
        std::string out;
        try {
            auto j = nlohmann::json::parse(raw);
            if (j.is_array()) {
                int max = std::min((int)j.size(), 200);
                for (int i = 0; i < max; i++) {
                    auto& e = j[i];
                    std::string ts = e.value("ts", "");
                    std::string lvl = e.value("level", "");
                    std::string msg = e.value("msg", "");
                    if (!out.empty()) out += "\n";
                    out += ts;
                    out += "  [";
                    out += lvl;
                    out += "]  ";
                    out += msg;
                }
            }
        } catch (...) {
            out = "(日志解析失败)";
        }
        // Push the result back onto the LVGL thread.
        auto* ptext = new std::string(std::move(out));
        lv_async_call([](void* ud) {
            auto* pair = static_cast<std::pair<AppReachy*, std::string*>*>(ud);
            if (pair->first->_lg_text) lv_label_set_text(pair->first->_lg_text, pair->second->c_str());
            delete pair->second;
            delete pair;
        }, new std::pair<AppReachy*, std::string*>(this, ptext));
        _fetched_ok.store(true);
        _fetched_at_ms.store(GetHAL()->millis());
        _fetch_inflight.store(false);
    });
    if (!ok) {
        _fetch_inflight.store(false);
        if (_lg_text) lv_label_set_text(_lg_text, "(后台资源紧张，跳过本次刷新)");
    }
}

// ── Callbacks ──────────────────────────────────────────────────────────────
void AppReachy::_tabCb(lv_event_t* e) {
    auto* ctx = static_cast<TabCtx*>(lv_event_get_user_data(e));
    ctx->self->_switchTab((Tab)ctx->idx);
}

void AppReachy::_audioEventCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto code = lv_event_get_code(e);
    if (target == self->_au_volume_slider) {
        int value = lv_slider_get_value(target);
        if (code == LV_EVENT_VALUE_CHANGED) {
            char text[16];
            snprintf(text, sizeof(text), "%d %%", value);
            lv_label_set_text(self->_au_volume_lbl, text);
        } else if (code == LV_EVENT_RELEASED) {
            self->_queueOperation({OperationKind::SetVolume, value});
        }
    } else if (target == self->_au_vad_slider) {
        int value = lv_slider_get_value(target);
        if (code == LV_EVENT_VALUE_CHANGED) {
            char text[20];
            snprintf(text, sizeof(text), "%.3f RMS", value / 1000.f);
            lv_label_set_text(self->_au_vad_lbl, text);
        } else if (code == LV_EVENT_RELEASED) {
            Operation operation;
            operation.kind = OperationKind::SetVad;
            operation.float_value = value / 1000.f;
            self->_queueOperation(operation);
        }
    } else if (target == self->_au_mute_btn && code == LV_EVENT_CLICKED) {
        int current = lv_slider_get_value(self->_au_volume_slider);
        int target_volume = current == 0 ? self->_last_nonzero_volume : 0;
        self->_queueOperation({OperationKind::SetVolume, target_volume});
    } else if (target == self->_au_mic_btn && code == LV_EVENT_CLICKED) {
        Operation operation;
        operation.kind = OperationKind::SetMic;
        operation.bool_value = std::string(lv_label_get_text(self->_au_mic_lbl)) == "启用 Mic";
        self->_queueOperation(operation);
    }
}

void AppReachy::_controlEventCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    Operation operation;
    if (target == self->_ct_xiaozhi_btn || target == self->_ct_qwen_btn) {
        operation.kind = OperationKind::SetBackend;
        operation.text_value = target == self->_ct_qwen_btn ? "qwen" : "xiaozhi";
        self->_confirmOperation(operation,
            target == self->_ct_qwen_btn ? "切换到 QWEN 并重启 YRobot？"
                                         : "切换到 XIAOZHI 并重启 YRobot？");
    } else if (target == self->_ct_sleep_btn) {
        operation.kind = OperationKind::DaemonSleep;
        self->_confirmOperation(operation, "让 Reachy Daemon 休眠？");
    } else if (target == self->_ct_daemon_restart_btn) {
        operation.kind = OperationKind::DaemonRestart;
        self->_confirmOperation(operation, "重启 Reachy Daemon？");
    } else if (target == self->_ct_wake_btn) {
        operation.kind = OperationKind::DaemonWake;
        self->_queueOperation(operation);
    } else if (target == self->_ct_video_btn) {
        operation.kind = OperationKind::SetVideo;
        operation.bool_value = std::string(lv_label_get_text(self->_ct_video_lbl)) == "开启视频";
        self->_queueOperation(operation);
    } else if (target == self->_ct_camera_btn) {
        operation.kind = OperationKind::SetCamera;
        operation.bool_value = std::string(lv_label_get_text(self->_ct_camera_lbl)) == "开启摄像头";
        self->_queueOperation(operation);
    } else if (target == self->_ct_voice_dropdown) {
        char voice[64] = {};
        lv_dropdown_get_selected_str(self->_ct_voice_dropdown, voice, sizeof(voice));
        operation.kind = OperationKind::SetVoice;
        operation.text_value = voice;
        self->_queueOperation(operation);
    }
}

void AppReachy::_restartCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    self->_confirmAndRestart();
}

void AppReachy::_logsRefreshCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    self->_renderLogs();
}

void AppReachy::_confirmYesCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    self->_queueOperation(self->_confirmed_operation);
    self->_confirmed_operation = {};
    // Dismiss the modal — find backdrop on top layer and delete it.
    auto top = lv_layer_top();
    if (top) {
        // The modal is the only child added recently.
        lv_obj_clean(top);
    }
}

void AppReachy::_confirmNoCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    self->_confirmed_operation = {};
    auto top = lv_layer_top();
    if (top) lv_obj_clean(top);
}

void AppReachy::_modalBgCb(lv_event_t* e) {
    // Click backdrop closes the dialog.
    auto top = lv_layer_top();
    if (top) lv_obj_clean(top);
}

void AppReachy::_gestureCb(lv_event_t* e) {
    lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
    if (lv_indev_get_gesture_dir(dev) != LV_DIR_TOP) return;
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    lv_async_call([](void* ud) {
        auto* self = static_cast<AppReachy*>(ud);
        self->_requestClose();
    }, self);
}
