// SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
// SPDX-License-Identifier: MIT
#include "app_reachy.h"
#include "reachy_ui.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <cstdio>
#include <utility>

using namespace reachy_ui;
static const std::string _tag = "reachy";

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include <esp_heap_caps.h>
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
static void ensure_reachy_lvgl_pool() {
    static bool initialized = false;
    if (initialized) return;
    constexpr size_t pool_size = 128 * 1024;
    void* pool = heap_caps_malloc(pool_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pool || !lv_mem_add_pool(pool, pool_size)) {
        if (pool) heap_caps_free(pool);
        mclog::tagWarn(_tag, "failed to expand LVGL heap in PSRAM");
        return;
    }
    initialized = true;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
static void ensure_reachy_lvgl_pool() {}
#endif

struct TabCtx { class AppReachy* self; int idx; };

static std::string _format_uptime(int seconds) {
    if (seconds <= 0) return "—";
    char out[32];
    snprintf(out, sizeof(out), "%dh %02dm", seconds / 3600, (seconds % 3600) / 60);
    return out;
}

static void set_label(lv_obj_t* label, const std::string& text, uint32_t color = kText) {
    if (!label) return;
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static lv_obj_t* value_label(lv_obj_t* parent, const char* text, int x, int y,
                             int width, uint32_t color = kText) {
    lv_obj_t* label = makeLabel(parent, text, x, y, zh_font_lg(), color);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

AppReachy::AppReachy() { setAppInfo().name = "YRobot 控制台"; }
void AppReachy::onCreate() {}

void AppReachy::onOpen() {
    _open_generation.fetch_add(1);
    ensure_reachy_lvgl_pool();
    if (_scr) {
        lv_screen_load(_scr);
        _installSwipeGesture();
    } else {
        _buildUI();
    }
    _last_poll_ms = 0;
}

void AppReachy::onClose() {
    _open_generation.fetch_add(1);
    _stopVideoPreview();
    _removeSwipeGesture();
    if (_close_cb) _close_cb();
    _scr = nullptr;
}

void AppReachy::onRunning() {
    // This app can remain installed while another Mooncake app has the screen.
    // Never poll, decode JPEG, or touch LVGL while backgrounded.
    if (!_scr || lv_screen_active() != _scr) return;

    if (_active == Tab::Camera) {
        if (_video_frame_ready.exchange(false)) _renderVideoFrame();
        const uint32_t now = GetHAL()->millis();
        if (_video_preview_enabled.load() && !_video_fetch_inflight.load() &&
            (_last_video_poll_ms == 0 || now - _last_video_poll_ms >= VIDEO_POLL_MS)) {
            _last_video_poll_ms = now;
            _pollVideoFrame();
        }
    }

    const uint32_t fetched_at = _fetched_at_ms.load();
    if (_fetched_ok.load() && fetched_at && _rendered_at_ms != fetched_at &&
        _completed_generation.load() == _open_generation.load()) {
        if (!_last_fetch_reachable.load()) {
            _showOfflineBanner();
        } else {
            switch (_active) {
            case Tab::Overview: _renderOverview(); break;
            case Tab::Voice: _renderVoice(); break;
            case Tab::Interaction: _renderInteraction(); break;
            case Tab::Camera: break;
            case Tab::Maintenance: _renderMaintenance(); break;
            default: break;
            }
        }
        _refreshHeader();
        _rendered_at_ms = fetched_at;
    }

    const uint32_t now = GetHAL()->millis();
    const uint32_t interval = _active == Tab::Interaction ? CHAT_POLL_MS : POLL_MS;
    if (_operation_pending.load() || _last_poll_ms == 0 || now - _last_poll_ms >= interval) {
        _last_poll_ms = now;
        if (!_fetch_inflight.load()) _refreshActiveTab();
    }
}

void AppReachy::_buildUI() {
    _scr = lv_obj_create(nullptr);
    lv_obj_set_size(_scr, W, H);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_scr, 0, 0);
    lv_obj_set_style_pad_all(_scr, 0, 0);
    _buildHeader();
    _buildBottomDock();
    _buildOverviewPage();
    _buildVoicePage();
    _buildInteractionPage();
    _buildCameraPage();
    _buildMaintenancePage();
    _selectDestination(Tab::Overview);
    lv_scr_load(_scr);
    _installSwipeGesture();
}

void AppReachy::_requestClose() {
    _open_generation.fetch_add(1);
    if (_close_cb) _close_cb();
}

void AppReachy::_installSwipeGesture() {
    _removeSwipeGesture();
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, _gestureCb, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
    }
}

void AppReachy::_removeSwipeGesture() {
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, _gestureCb, this);
        _gesture_indev = nullptr;
    }
}

void AppReachy::_buildHeader() {
    lv_obj_t* header = lv_obj_create(_scr);
    lv_obj_set_size(header, W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kBg), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(kStroke), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    makeLabel(header, "YRobot 控制台", 28, 17, zh_font_lg(), kText);
    _hdr_host = makeLabel(header, "等待连接", 290, 20, zh_font_sm(), kTextMuted);
    _hdr_state = makeStatusPill(header, "连接中…", 1020, 12, 220, zh_font_sm(), kWarn);
}

void AppReachy::_buildBottomDock() {
    _bottom_dock = makeSurface(_scr, 16, H - DOCK_H + 8, W - 32, DOCK_H - 16, kSurfaceAlt, 28);
    static const char* labels[(int)Tab::Count] = {"概览", "语音", "互动", "相机", "维护"};
    const int dock_w = (W - 64) / (int)Tab::Count;
    for (int i = 0; i < (int)Tab::Count; ++i) {
        lv_obj_t* button = makeButton(_bottom_dock, labels[i], i * dock_w + 8, 8,
                                      dock_w - 16, DOCK_H - 32, zh_font_lg(),
                                      kSurfaceAlt, kTextMuted);
        lv_obj_set_style_border_width(button, 0, 0);
        _dock_btns[i] = button;
        auto* ctx = new TabCtx{this, i};
        lv_obj_add_event_cb(button, _tabCb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(button, [](lv_event_t* e) {
            delete static_cast<TabCtx*>(lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, ctx);
    }
}

static lv_obj_t* make_page(lv_obj_t* root) {
    lv_obj_t* page = lv_obj_create(root);
    lv_obj_set_size(page, 1248, 574);
    lv_obj_set_pos(page, 16, 64);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

void AppReachy::_buildOverviewPage() {
    lv_obj_t* page = make_page(_scr);
    _tab_pages[(int)Tab::Overview] = page;
    lv_obj_t* hero = makeSurface(page, 0, 0, 700, 274);
    makeLabel(hero, "YRobot 状态", 28, 28, zh_font_sm(), kTextMuted);
    _st_service_state = value_label(hero, "正在检查服务…", 28, 66, 620, kAccent);
    lv_obj_set_style_text_font(_st_service_state, zh_font_lg(), 0);
    _st_uptime = value_label(hero, "连接后显示运行时长", 28, 124, 620, kTextMuted);
    _st_backend = value_label(hero, "对话后端 —", 28, 174, 620, kText);
    _st_wake = value_label(hero, "等待语音活动", 28, 218, 620, kTextMuted);

    const char* metric_titles[] = {"服务", "摄像头", "语音"};
    lv_obj_t** metric_values[] = {&_st_daemon, &_st_camera_metric, &_st_voice_metric};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* metric = makeSurface(page, 716, i * 94, 532, 82);
        makeLabel(metric, metric_titles[i], 20, 15, zh_font_sm(), kTextMuted);
        *metric_values[i] = value_label(metric, "检查中", 20, 42, 480, kText);
    }

    lv_obj_t* volume = makeSurface(page, 0, 290, 292, 268);
    makeLabel(volume, "音量控制", 20, 20, zh_font_lg());
    makeLabel(volume, "请到语音页调整", 20, 72, zh_font_sm(), kTextMuted);
    lv_obj_t* mic = makeSurface(page, 308, 290, 292, 268);
    makeLabel(mic, "麦克风", 20, 20, zh_font_lg());
    makeLabel(mic, "状态将在语音页显示", 20, 72, zh_font_sm(), kTextMuted);
    lv_obj_t* backend = makeSurface(page, 616, 290, 292, 268);
    makeLabel(backend, "对话后端", 20, 20, zh_font_lg());
    _st_video = value_label(backend, "读取中", 20, 72, 252, kAccent);
    lv_obj_t* chat = makeSurface(page, 924, 290, 324, 268);
    makeLabel(chat, "最近对话", 20, 20, zh_font_lg());
    makeLabel(chat, "在“互动”页查看对话记录", 20, 72, zh_font_sm(), kTextMuted);
}

void AppReachy::_buildVoicePage() {
    lv_obj_t* page = make_page(_scr);
    _tab_pages[(int)Tab::Voice] = page;

    lv_obj_t* volume = makeSurface(page, 0, 0, 430, 250);
    makeLabel(volume, "音量", 20, 20, zh_font_lg());
    _au_volume_lbl = value_label(volume, "—", 20, 66, 250, kAccent);
    _au_volume_slider = lv_slider_create(volume);
    lv_obj_set_size(_au_volume_slider, 270, 18);
    lv_obj_set_pos(_au_volume_slider, 20, 142);
    lv_slider_set_range(_au_volume_slider, 0, 100);
    lv_obj_add_event_cb(_au_volume_slider, _audioEventCb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_au_volume_slider, _audioEventCb, LV_EVENT_RELEASED, this);
    _au_mute_btn = makeButton(volume, "静音", 20, 176, 180, 52, zh_font_lg());
    _au_mute_lbl = lv_obj_get_child(_au_mute_btn, 0);
    lv_obj_add_event_cb(_au_mute_btn, _audioEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t* mic = makeSurface(page, 446, 0, 332, 250);
    makeLabel(mic, "麦克风", 20, 20, zh_font_lg());
    _au_mic_lbl = value_label(mic, "—", 20, 66, 280, kAccent);
    _au_mic_btn = makeButton(mic, "启用 Mic", 20, 176, 220, 52, zh_font_lg());
    lv_obj_add_event_cb(_au_mic_btn, _audioEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t* server = makeSurface(page, 794, 0, 454, 250);
    makeLabel(server, "后端与音色", 20, 20, zh_font_lg());
    _ct_backend_state = value_label(server, "读取中", 20, 66, 414, kText);
    _ct_xiaozhi_btn = makeButton(server, "XIAOZHI", 20, 120, 178, 52, zh_font_sm());
    _ct_qwen_btn = makeButton(server, "QWEN", 212, 120, 178, 52, zh_font_sm());
    _ct_voice_dropdown = lv_dropdown_create(server);
    lv_obj_set_size(_ct_voice_dropdown, 370, 48);
    lv_obj_set_pos(_ct_voice_dropdown, 20, 184);
    lv_dropdown_set_options(_ct_voice_dropdown, "读取音色…");
    lv_obj_set_style_text_font(_ct_voice_dropdown, zh_font_sm(), 0);
    lv_obj_add_event_cb(_ct_voice_dropdown, _controlEventCb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_ct_xiaozhi_btn, _controlEventCb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_ct_qwen_btn, _controlEventCb, LV_EVENT_CLICKED, this);

    lv_obj_t* vad = makeSurface(page, 0, 266, 606, 292);
    makeLabel(vad, "VAD 灵敏度", 20, 20, zh_font_lg());
    _au_vad_lbl = value_label(vad, "—", 20, 72, 300, kAccent);
    _au_vad_slider = lv_slider_create(vad);
    lv_obj_set_size(_au_vad_slider, 520, 18);
    lv_obj_set_pos(_au_vad_slider, 20, 142);
    lv_slider_set_range(_au_vad_slider, 1, 500);
    lv_obj_add_event_cb(_au_vad_slider, _audioEventCb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_au_vad_slider, _audioEventCb, LV_EVENT_RELEASED, this);
    _au_status = value_label(vad, "调整后将在后台同步", 20, 196, 540, kTextMuted);

    lv_obj_t* camera = makeSurface(page, 622, 266, 626, 292);
    makeLabel(camera, "相机与视频", 20, 20, zh_font_lg());
    _ct_video_btn = makeButton(camera, "视频", 20, 76, 220, 54, zh_font_lg());
    _ct_video_lbl = lv_obj_get_child(_ct_video_btn, 0);
    _ct_camera_btn = makeButton(camera, "摄像头", 258, 76, 220, 54, zh_font_lg());
    _ct_camera_lbl = lv_obj_get_child(_ct_camera_btn, 0);
    makeLabel(camera, "开启预览请前往“相机”页", 20, 170, zh_font_sm(), kTextMuted);
    lv_obj_add_event_cb(_ct_video_btn, _controlEventCb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_ct_camera_btn, _controlEventCb, LV_EVENT_CLICKED, this);
}

void AppReachy::_buildInteractionPage() {
    lv_obj_t* page = make_page(_scr);
    _tab_pages[(int)Tab::Interaction] = page;
    lv_obj_t* motion = makeSurface(page, 0, 0, 1248, 154);
    makeLabel(motion, "动作与注视", 20, 18, zh_font_lg());
    makeLabel(motion, "模式", 20, 70, zh_font_sm(), kTextMuted);
    _mo_mode = value_label(motion, "—", 20, 102, 250, kText);
    makeLabel(motion, "当前动作", 310, 70, zh_font_sm(), kTextMuted);
    _mo_current = value_label(motion, "—", 310, 102, 260, kAccent);
    makeLabel(motion, "天线", 600, 70, zh_font_sm(), kTextMuted);
    _mo_antennas = value_label(motion, "—", 600, 102, 260, kText);
    makeLabel(motion, "注视", 890, 70, zh_font_sm(), kTextMuted);
    _mo_gaze = value_label(motion, "—", 890, 102, 330, kText);
    _mo_queue = nullptr;
    _mo_loop = nullptr;

    lv_obj_t* conversation = makeSurface(page, 0, 170, 1248, 388);
    makeLabel(conversation, "最近对话", 20, 16, zh_font_lg());
    _chat_container = lv_obj_create(conversation);
    lv_obj_set_size(_chat_container, 1208, 320);
    lv_obj_set_pos(_chat_container, 20, 54);
    lv_obj_set_style_bg_opa(_chat_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_chat_container, 0, 0);
    lv_obj_set_style_pad_all(_chat_container, 4, 0);
    lv_obj_set_scroll_dir(_chat_container, LV_DIR_VER);
}

void AppReachy::_buildCameraPage() {
    lv_obj_t* page = make_page(_scr);
    _tab_pages[(int)Tab::Camera] = page;
    lv_obj_t* frame = makeSurface(page, 0, 0, 1248, 490, kSurfaceAlt);
    _vi_image = lv_image_create(frame);
    lv_obj_align(_vi_image, LV_ALIGN_CENTER, 0, -12);
    _vi_status = makeLabel(frame, "预览已停止。开始后以 2 FPS 接收 JPEG 帧。", 24, 442, zh_font_sm(), kTextMuted);
    _vi_toggle_btn = makeButton(page, "开始预览", 1004, 506, 244, 52, zh_font_lg(), kAccent, kBg);
    _vi_toggle_lbl = lv_obj_get_child(_vi_toggle_btn, 0);
    lv_obj_add_event_cb(_vi_toggle_btn, _videoToggleCb, LV_EVENT_CLICKED, this);
    makeLabel(page, "相机预览", 0, 514, zh_font_lg());
}

void AppReachy::_buildMaintenancePage() {
    lv_obj_t* page = make_page(_scr);
    _tab_pages[(int)Tab::Maintenance] = page;
    lv_obj_t* system = makeSurface(page, 0, 0, 610, 144);
    makeLabel(system, "YRobot 服务", 20, 20, zh_font_lg());
    _sy_state = value_label(system, "检查中", 20, 74, 560, kAccent);
    lv_obj_t* daemon = makeSurface(page, 626, 0, 622, 144);
    makeLabel(daemon, "Reachy Daemon", 20, 20, zh_font_lg());
    _sy_daemon = value_label(daemon, "检查中", 20, 74, 574, kAccent);

    lv_obj_t* actions = makeSurface(page, 0, 160, 1248, 118);
    makeLabel(actions, "维护操作", 20, 18, zh_font_lg());
    _ct_wake_btn = makeButton(actions, "唤醒 Daemon", 20, 58, 230, 46, zh_font_sm(), kAccentSoft);
    _ct_sleep_btn = makeButton(actions, "休眠 Daemon", 266, 58, 230, 46, zh_font_sm(), kError);
    _ct_daemon_restart_btn = makeButton(actions, "重启 Daemon", 512, 58, 230, 46, zh_font_sm(), kError);
    _sy_restart_btn = makeButton(actions, "重启 YRobot 服务", 758, 58, 300, 46, zh_font_sm(), kError);
    lv_obj_add_event_cb(_ct_wake_btn, _controlEventCb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_ct_sleep_btn, _controlEventCb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_ct_daemon_restart_btn, _controlEventCb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_sy_restart_btn, _restartCb, LV_EVENT_CLICKED, this);

    lv_obj_t* logs = makeSurface(page, 0, 294, 1248, 264);
    makeLabel(logs, "服务日志", 20, 16, zh_font_lg());
    _lg_text = value_label(logs, "日志按需加载，不会持续轮询。", 20, 62, 970, kTextMuted);
    lv_obj_set_width(_lg_text, 960);
    lv_label_set_long_mode(_lg_text, LV_LABEL_LONG_WRAP);
    _lg_refresh_btn = makeButton(logs, "查看日志", 1014, 16, 210, 48, zh_font_sm(), kAccent, kBg);
    lv_obj_add_event_cb(_lg_refresh_btn, _logsRefreshCb, LV_EVENT_CLICKED, this);
}

void AppReachy::_selectDestination(Tab t) {
    if (_active == Tab::Camera && t != Tab::Camera) _stopVideoPreview();
    _active = t;
    for (int i = 0; i < (int)Tab::Count; ++i) {
        const bool selected = i == (int)t;
        lv_obj_set_style_bg_color(_dock_btns[i], lv_color_hex(selected ? kAccentSoft : kSurfaceAlt), 0);
        lv_obj_t* label = lv_obj_get_child(_dock_btns[i], 0);
        if (label) lv_obj_set_style_text_color(label, lv_color_hex(selected ? kAccent : kTextMuted), 0);
        if (_tab_pages[i]) {
            if (selected) lv_obj_clear_flag(_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(_tab_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    _last_poll_ms = 0;
    if (!_fetch_inflight.load()) _refreshActiveTab();
}

void AppReachy::_refreshActiveTab() {
    if (!_scr || _fetch_inflight.exchange(true)) return;
    const Tab tab = _active;
    const uint32_t generation = _open_generation.load();
    const bool started = GetHAL()->tryRunDetached([this, tab, generation]() {
        Operation operation;
        if (_operation_pending.load()) {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            operation = _pending_operation;
            _pending_operation = {};
            _operation_pending.store(false);
        }
        if (operation.kind != OperationKind::None) {
            auto result = _executeOperation(operation);
            std::lock_guard<std::mutex> lock(_cache_mutex);
            _operation_result = std::move(result);
            _operation_result_ready = true;
        }

        const bool reachable = reachy_client::ping();
        _last_fetch_reachable.store(reachable);
        if (reachable) {
            switch (tab) {
            case Tab::Overview: {
                auto status = reachy_client::fetchStatus();
                std::lock_guard<std::mutex> lock(_cache_mutex);
                _status = std::move(status); _has_status = true;
                break;
            }
            case Tab::Voice: {
                auto audio = reachy_client::fetchAudio();
                auto control = reachy_client::fetchControlState();
                std::lock_guard<std::mutex> lock(_cache_mutex);
                _audio = std::move(audio); _has_audio = true;
                _control = std::move(control); _has_control = true;
                break;
            }
            case Tab::Interaction: {
                auto motion = reachy_client::fetchMotion();
                auto status = reachy_client::fetchStatus();
                auto chat = reachy_client::fetchChat(200, 6);
                std::lock_guard<std::mutex> lock(_cache_mutex);
                _status = status; _has_status = true;
                _motion = reachy_client::mergeStatusIntoMotion(motion, status); _has_motion = true;
                _chat_messages = std::move(chat); _has_chat = true;
                break;
            }
            case Tab::Camera: break;
            case Tab::Maintenance: {
                auto sys = reachy_client::fetchSystemState();
                bool want_logs = _logs_requested.exchange(false);
                std::string text;
                if (want_logs) {
                    try {
                        auto raw = reachy_client::fetchLogsRaw(200);
                        auto json = nlohmann::json::parse(raw);
                        if (json.contains("logs") && json["logs"].is_array()) {
                            for (auto& item : json["logs"]) {
                                if (!text.empty()) text += "\n";
                                text += item.value("timestamp", "") + "  [" + item.value("level", "") + "]  " + item.value("message", "");
                            }
                        }
                    } catch (...) { text = "日志解析失败"; }
                }
                std::lock_guard<std::mutex> lock(_cache_mutex);
                _sys = std::move(sys); _has_sys = true;
                if (want_logs) { _logs_text = std::move(text); _has_logs = true; }
                break;
            }
            default: break;
            }
        }
        if (generation == _open_generation.load()) {
            _completed_generation.store(generation);
            _fetched_ok.store(true);
            _fetched_at_ms.store(GetHAL()->millis());
        }
        _fetch_inflight.store(false);
    });
    if (!started) {
        _fetch_inflight.store(false);
        mclog::tagWarn(_tag, "worker spawn failed; skipping cockpit poll");
    }
}

void AppReachy::_refreshHeader() {
    if (_hdr_host) lv_label_set_text(_hdr_host, reachy_client::_host().c_str());
    if (!_hdr_state) return;
    lv_obj_t* label = lv_obj_get_child(_hdr_state, 0);
    const bool ok = _last_ok;
    if (label) {
        lv_label_set_text(label, ok ? "已连接" : "无法连接");
        lv_obj_set_style_text_color(label, lv_color_hex(ok ? kOk : kError), 0);
    }
    lv_obj_set_style_border_color(_hdr_state, lv_color_hex(ok ? kOk : kError), 0);
}

void AppReachy::_showOfflineBanner() {
    _last_ok = false;
    set_label(_st_service_state, "无法连接 YRobot", kError);
    set_label(_st_uptime, "检查主机地址和网络后重试", kTextMuted);
    set_label(_st_backend, "—", kTextMuted); set_label(_st_video, "—", kTextMuted);
    set_label(_st_wake, "—", kTextMuted); set_label(_st_daemon, "—", kTextMuted);
    set_label(_st_camera_metric, "不可用", kError); set_label(_st_voice_metric, "不可用", kError);
    set_label(_au_volume_lbl, "—", kTextMuted); set_label(_au_vad_lbl, "—", kTextMuted);
    set_label(_au_status, "无法连接 YRobot", kError); set_label(_ct_backend_state, "—", kTextMuted);
    set_label(_mo_mode, "—", kTextMuted); set_label(_mo_current, "—", kTextMuted);
    set_label(_mo_antennas, "—", kTextMuted); set_label(_mo_gaze, "—", kTextMuted);
    set_label(_sy_state, "无法连接", kError); set_label(_sy_daemon, "—", kTextMuted);
}

void AppReachy::_renderOverview() {
    reachy_client::Status status;
    { std::lock_guard<std::mutex> lock(_cache_mutex); if (!_has_status) return; status = _status; }
    if (!status.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    const bool running = status.service_state == "running" || status.service_state == "active";
    set_label(_st_service_state, running ? "服务正常" : "需要注意", running ? kOk : kWarn);
    set_label(_st_uptime, "运行 " + _format_uptime(status.uptime_s), kTextMuted);
    set_label(_st_backend, "对话后端 · " + (status.configured_backend.empty() ? status.runtime_backend : status.configured_backend));
    set_label(_st_wake, status.wake_active ? "正在响应语音" : "等待语音活动", status.wake_active ? kAccent : kTextMuted);
    set_label(_st_daemon, status.daemon_available ? "运行中" : "不可用", status.daemon_available ? kOk : kWarn);
    set_label(_st_camera_metric, status.video_enabled ? "待命" : "已关闭", status.video_enabled ? kOk : kWarn);
    set_label(_st_voice_metric, status.ws_state == "connected" ? "在线" : "待确认", status.ws_state == "connected" ? kOk : kWarn);
    set_label(_st_video, status.video_enabled ? "视频服务已开启" : "视频服务已关闭", status.video_enabled ? kAccent : kTextMuted);
}

void AppReachy::_renderVoice() {
    reachy_client::Audio audio;
    reachy_client::ControlState control;
    { std::lock_guard<std::mutex> lock(_cache_mutex); if (!_has_audio || !_has_control) return; audio = _audio; control = _control; }
    if (!audio.ok || !control.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    if (audio.volume_percent < 0) {
        set_label(_au_volume_lbl, "不可用", kWarn);
    } else {
        set_label(_au_volume_lbl, std::to_string(audio.volume_percent) + "%", kAccent);
        if (audio.volume_percent > 0) _last_nonzero_volume = audio.volume_percent;
        lv_slider_set_value(_au_volume_slider, audio.volume_percent, LV_ANIM_ON);
    }
    if (_au_mute_lbl) lv_label_set_text(_au_mute_lbl, audio.volume_percent == 0 ? "恢复音量" : "静音");
    if (_au_mic_lbl) lv_label_set_text(_au_mic_lbl, audio.mic_input_enabled ? "禁用 Mic" : "启用 Mic");
    char vad[32]; snprintf(vad, sizeof(vad), "%.3f %s", audio.vad_rms_min, audio.vad_unit.c_str());
    set_label(_au_vad_lbl, vad, kAccent);
    lv_slider_set_value(_au_vad_slider, (int)(audio.vad_rms_min * 1000.f + .5f), LV_ANIM_ON);
    std::string backend = "配置: " + control.configured_backend + "  ·  运行: " + control.running_backend;
    if (!control.backend_error.empty()) backend += "  ·  " + control.backend_error;
    set_label(_ct_backend_state, backend, control.backend_error.empty() ? kText : kWarn);
    if (_ct_video_lbl) lv_label_set_text(_ct_video_lbl, control.video_enabled ? "关闭视频" : "开启视频");
    if (_ct_camera_lbl) lv_label_set_text(_ct_camera_lbl, control.camera_running ? "关闭摄像头" : "开启摄像头");
    if (!control.available_voices.empty()) {
        std::string options;
        int selected = 0;
        for (size_t i = 0; i < control.available_voices.size(); ++i) {
            if (!options.empty()) options += "\n";
            options += control.available_voices[i];
            if (control.available_voices[i] == control.configured_voice) selected = (int)i;
        }
        lv_dropdown_set_options(_ct_voice_dropdown, options.c_str());
        lv_dropdown_set_selected(_ct_voice_dropdown, selected);
    }
    std::lock_guard<std::mutex> lock(_cache_mutex);
    if (_operation_result_ready) {
        set_label(_au_status, _operation_result.ok ? "已同步到 YRobot" : _operation_result.error,
                  _operation_result.ok ? kOk : kError);
        _operation_result_ready = false;
    }
}

void AppReachy::_renderInteraction() {
    reachy_client::Motion motion;
    { std::lock_guard<std::mutex> lock(_cache_mutex); if (!_has_motion) return; motion = _motion; }
    if (!motion.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    set_label(_mo_mode, motion.mode.empty() ? "—" : motion.mode);
    set_label(_mo_current, motion.current_move.empty() ? "当前无动作" : motion.current_move, kAccent);
    set_label(_mo_antennas, motion.antennas.empty() ? "—" : motion.antennas);
    char gaze[64]; snprintf(gaze, sizeof(gaze), "%s %.2f rad%s", motion.gaze_source.c_str(), motion.gaze_target_rad, motion.face_detected ? "（人脸）" : "");
    set_label(_mo_gaze, gaze);
    _renderChat();
}

void AppReachy::_renderChat() {
    std::vector<reachy_client::ChatMessage> messages;
    { std::lock_guard<std::mutex> lock(_cache_mutex); if (!_has_chat) return; messages = _chat_messages; }
    std::string signature;
    for (const auto& item : messages) signature += (item.role == reachy_client::ChatMessage::Role::User ? "U:" : "A:") + item.text + "\n";
    if (signature == _chat_signature) return;
    _chat_signature = std::move(signature);
    lv_obj_clean(_chat_container);
    int y = 0;
    for (const auto& item : messages) {
        const bool user = item.role == reachy_client::ChatMessage::Role::User;
        lv_obj_t* bubble = makeSurface(_chat_container, 0, 0, 520, LV_SIZE_CONTENT,
                                       user ? kAccentSoft : kSurface, 18);
        lv_obj_set_style_min_height(bubble, 52, 0);
        lv_obj_set_style_shadow_width(bubble, 0, 0);
        lv_obj_align(bubble, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_t* text = makeLabel(bubble, item.text.c_str(), 14, 12, zh_font_lg());
        lv_obj_set_width(text, 492);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_update_layout(bubble);
        y += lv_obj_get_height(bubble) + 10;
    }
    lv_obj_scroll_to_y(_chat_container, y, LV_ANIM_OFF);
}

void AppReachy::_renderMaintenance() {
    reachy_client::SystemState sys;
    reachy_client::Status status;
    bool has_status = false;
    { std::lock_guard<std::mutex> lock(_cache_mutex); if (!_has_sys) return; sys = _sys; if (_has_status) { status = _status; has_status = true; } }
    if (!sys.ok) { _showOfflineBanner(); return; }
    _last_ok = true;
    std::string service = sys.service + (sys.running ? " · 运行中" : " · 已停止") + " · " + _format_uptime(sys.uptime_s);
    set_label(_sy_state, service, sys.running ? kOk : kWarn);
    set_label(_sy_daemon, has_status && status.daemon_available ? status.daemon_state : "状态待确认",
              has_status && status.daemon_available ? kOk : kWarn);
    _renderLogs();
    std::lock_guard<std::mutex> lock(_cache_mutex);
    if (_operation_result_ready) {
        set_label(_sy_state, _operation_result.ok ? "维护请求已发送" : _operation_result.error,
                  _operation_result.ok ? kOk : kError);
        _operation_result_ready = false;
    }
}

void AppReachy::_renderLogs() {
    std::string logs;
    { std::lock_guard<std::mutex> lock(_cache_mutex); if (!_has_logs) return; logs = _logs_text; }
    set_label(_lg_text, logs.empty() ? "无日志" : logs, logs.empty() ? kTextMuted : kText);
}

bool AppReachy::_queueOperation(Operation operation) {
    lv_obj_t* status = _active == Tab::Maintenance ? _sy_state : _au_status;
    if (_fetch_inflight.load() || _operation_pending.load()) {
        set_label(status, "正在处理，请稍候", kWarn);
        return false;
    }
    { std::lock_guard<std::mutex> lock(_cache_mutex); _pending_operation = std::move(operation); _operation_pending.store(true); }
    set_label(status, "处理中…", kWarn);
    _last_poll_ms = 0;
    return true;
}

reachy_client::OperationResult AppReachy::_executeOperation(const Operation& operation) {
    switch (operation.kind) {
    case OperationKind::SetVolume: return reachy_client::setVolume(operation.int_value);
    case OperationKind::SetMic: return reachy_client::setMicEnabled(operation.bool_value);
    case OperationKind::SetVad: return reachy_client::setVad(operation.float_value);
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
        if (status.ok && status.configured_backend == target && status.runtime_backend == target) return {true, 200, ""};
    }
    return {false, 0, "切换超时；未自动回退"};
}

void AppReachy::_pollVideoFrame() {
    _video_fetch_inflight.store(true);
    const uint32_t generation = _open_generation.load();
    const bool started = GetHAL()->tryRunDetached([this, generation]() {
        std::string frame = reachy_client::fetchCameraFrame();
        if (generation == _open_generation.load() && _video_preview_enabled.load()) {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            _video_pending_frame = std::move(frame);
            _video_frame_ready.store(true);
        }
        _video_fetch_inflight.store(false);
    });
    if (!started) {
        _video_fetch_inflight.store(false);
        set_label(_vi_status, "无法启动视频任务", kError);
    }
}

void AppReachy::_renderVideoFrame() {
    { std::lock_guard<std::mutex> lock(_cache_mutex); _video_frame = std::move(_video_pending_frame); }
    if (_video_frame.empty()) { set_label(_vi_status, "视频连接中断", kError); return; }
    _video_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    _video_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    _video_dsc.header.w = 640; _video_dsc.header.h = 360; _video_dsc.header.stride = 0;
    _video_dsc.data_size = _video_frame.size();
    _video_dsc.data = reinterpret_cast<const uint8_t*>(_video_frame.data());
    lv_image_set_src(_vi_image, &_video_dsc);
    lv_obj_align(_vi_image, LV_ALIGN_CENTER, 0, -12);
    set_label(_vi_status, "实时预览  ·  640 × 360  ·  2 FPS", kOk);
}

void AppReachy::_stopVideoPreview() {
    _video_preview_enabled.store(false);
    _last_video_poll_ms = 0;
    if (_vi_toggle_lbl) lv_label_set_text(_vi_toggle_lbl, "开始预览");
    set_label(_vi_status, "预览已停止。开始后以 2 FPS 接收 JPEG 帧。", kTextMuted);
}

void AppReachy::_confirmAndRestart() {
    Operation operation; operation.kind = OperationKind::RestartYRobot;
    _confirmOperation(operation, "确认重启 YRobot 服务？");
}

void AppReachy::_confirmOperation(Operation operation, const char* message) {
    _confirmed_operation = std::move(operation);
    lv_obj_t* bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bg, W, H); lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0), 0); lv_obj_set_style_bg_opa(bg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bg, 0, 0); lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, _modalBgCb, LV_EVENT_CLICKED, this);
    lv_obj_t* dlg = makeSurface(bg, 0, 0, 600, 320, kSurface, 22);
    lv_obj_align(dlg, LV_ALIGN_CENTER, 0, 0);
    makeLabel(dlg, message, 28, 28, zh_font_lg(), kAccent);
    lv_obj_t* note = makeLabel(dlg, "此操作可能短暂中断对话或运动。\n确认后将立即发送到 YRobot。", 28, 92, zh_font_sm(), kTextMuted);
    lv_obj_set_width(note, 540);
    lv_obj_t* yes = makeButton(dlg, "确认", 28, 224, 246, 60, zh_font_lg(), kError);
    lv_obj_t* no = makeButton(dlg, "取消", 326, 224, 246, 60, zh_font_lg(), kAccentSoft);
    lv_obj_add_event_cb(yes, _confirmYesCb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(no, _confirmNoCb, LV_EVENT_CLICKED, this);
}

void AppReachy::_tabCb(lv_event_t* e) {
    auto* ctx = static_cast<TabCtx*>(lv_event_get_user_data(e));
    ctx->self->_selectDestination((Tab)ctx->idx);
}

void AppReachy::_audioEventCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const auto code = lv_event_get_code(e);
    if (target == self->_au_volume_slider) {
        const int value = lv_slider_get_value(target);
        if (code == LV_EVENT_VALUE_CHANGED) set_label(self->_au_volume_lbl, std::to_string(value) + "%", kAccent);
        else if (code == LV_EVENT_RELEASED) self->_queueOperation({OperationKind::SetVolume, value});
    } else if (target == self->_au_vad_slider) {
        const int value = lv_slider_get_value(target);
        if (code == LV_EVENT_VALUE_CHANGED) {
            char text[20]; snprintf(text, sizeof(text), "%.3f RMS", value / 1000.f);
            set_label(self->_au_vad_lbl, text, kAccent);
        } else if (code == LV_EVENT_RELEASED) {
            Operation op; op.kind = OperationKind::SetVad; op.float_value = value / 1000.f; self->_queueOperation(op);
        }
    } else if (target == self->_au_mute_btn && code == LV_EVENT_CLICKED) {
        const int value = lv_slider_get_value(self->_au_volume_slider);
        self->_queueOperation({OperationKind::SetVolume, value == 0 ? self->_last_nonzero_volume : 0});
    } else if (target == self->_au_mic_btn && code == LV_EVENT_CLICKED) {
        Operation op; op.kind = OperationKind::SetMic;
        op.bool_value = std::string(lv_label_get_text(self->_au_mic_lbl)) == "启用 Mic";
        self->_queueOperation(op);
    }
}

void AppReachy::_controlEventCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    Operation op;
    if (target == self->_ct_xiaozhi_btn || target == self->_ct_qwen_btn) {
        op.kind = OperationKind::SetBackend;
        op.text_value = target == self->_ct_qwen_btn ? "qwen" : "xiaozhi";
        self->_confirmOperation(op, target == self->_ct_qwen_btn ? "切换到 QWEN 并重启 YRobot？" : "切换到 XIAOZHI 并重启 YRobot？");
    } else if (target == self->_ct_sleep_btn) {
        op.kind = OperationKind::DaemonSleep; self->_confirmOperation(op, "让 Reachy Daemon 休眠？");
    } else if (target == self->_ct_daemon_restart_btn) {
        op.kind = OperationKind::DaemonRestart; self->_confirmOperation(op, "重启 Reachy Daemon？");
    } else if (target == self->_ct_wake_btn) {
        op.kind = OperationKind::DaemonWake; self->_queueOperation(op);
    } else if (target == self->_ct_video_btn) {
        op.kind = OperationKind::SetVideo;
        op.bool_value = std::string(lv_label_get_text(self->_ct_video_lbl)) == "开启视频"; self->_queueOperation(op);
    } else if (target == self->_ct_camera_btn) {
        op.kind = OperationKind::SetCamera;
        op.bool_value = std::string(lv_label_get_text(self->_ct_camera_lbl)) == "开启摄像头"; self->_queueOperation(op);
    } else if (target == self->_ct_voice_dropdown) {
        char voice[64] = {}; lv_dropdown_get_selected_str(self->_ct_voice_dropdown, voice, sizeof(voice));
        op.kind = OperationKind::SetVoice; op.text_value = voice; self->_queueOperation(op);
    }
}

void AppReachy::_videoToggleCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    if (self->_video_preview_enabled.load()) { self->_stopVideoPreview(); return; }
    self->_video_preview_enabled.store(true); self->_last_video_poll_ms = 0;
    lv_label_set_text(self->_vi_toggle_lbl, "停止预览");
    set_label(self->_vi_status, "正在连接视频…", kWarn);
}

void AppReachy::_restartCb(lv_event_t* e) { static_cast<AppReachy*>(lv_event_get_user_data(e))->_confirmAndRestart(); }
void AppReachy::_logsRefreshCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    self->_logs_requested.store(true); self->_last_poll_ms = 0;
    if (!self->_fetch_inflight.load()) self->_refreshActiveTab();
}
void AppReachy::_confirmYesCb(lv_event_t* e) {
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    self->_queueOperation(self->_confirmed_operation); self->_confirmed_operation = {};
    if (auto* top = lv_layer_top()) lv_obj_clean(top);
}
void AppReachy::_confirmNoCb(lv_event_t* e) {
    static_cast<AppReachy*>(lv_event_get_user_data(e))->_confirmed_operation = {};
    if (auto* top = lv_layer_top()) lv_obj_clean(top);
}
void AppReachy::_modalBgCb(lv_event_t* e) { if (auto* top = lv_layer_top()) lv_obj_clean(top); }
void AppReachy::_gestureCb(lv_event_t* e) {
    auto* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
    if (lv_indev_get_gesture_dir(dev) != LV_DIR_TOP) return;
    auto* self = static_cast<AppReachy*>(lv_event_get_user_data(e));
    lv_async_call([](void* data) { static_cast<AppReachy*>(data)->_requestClose(); }, self);
}
