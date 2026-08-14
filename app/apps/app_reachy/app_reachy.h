// SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
// SPDX-License-Identifier: MIT
#pragma once

#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include "reachy_client.h"

/**
 * AppReachy — native dashboard and controls for the YRobot FastAPI service.
 */
class AppReachy : public mooncake::AppAbility {
public:
    AppReachy();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    // ── Layout constants ──────────────────────────────────────────────────
    static constexpr int W = 1280;
    static constexpr int H = 720;
    static constexpr int HEADER_H = 56;
    static constexpr int TAB_BAR_H = 64;
    static constexpr int POLL_MS = 5000;

    // ── Palette (matches AppHA / AppSettings) ─────────────────────────────
    static constexpr uint32_t C_BG        = 0x0E1A2B;
    static constexpr uint32_t C_HEADER    = 0x142540;
    static constexpr uint32_t C_CARD      = 0x1B3552;
    static constexpr uint32_t C_CARD_PR   = 0x244B7A;
    static constexpr uint32_t C_TAB_BG    = 0x142540;
    static constexpr uint32_t C_TAB_ACTIVE = 0x244B7A;
    static constexpr uint32_t C_ACCENT    = 0x66E0FF;
    static constexpr uint32_t C_ACCENT2   = 0xA3F7BF;
    static constexpr uint32_t C_TEXT      = 0xFFFFFF;
    static constexpr uint32_t C_TEXT2     = 0xB8C8DA;
    static constexpr uint32_t C_WARN      = 0xFFD166;
    static constexpr uint32_t C_ERR       = 0xFF6B6B;
    static constexpr uint32_t C_OK        = 0xA3F7BF;

    enum class Tab : int { Status, Motion, Audio, Control, Chat, System, Logs, Count };

    std::function<void()> _close_cb;

    // ── UI roots ──────────────────────────────────────────────────────────
    lv_obj_t* _scr = nullptr;
    lv_obj_t* _tab_bar = nullptr;
    lv_obj_t* _tab_btns[(int)Tab::Count] = {};
    lv_obj_t* _tab_pages[(int)Tab::Count] = {};
    Tab       _active = Tab::Status;
    lv_indev_t* _gesture_indev = nullptr;

    // Header status (host + last fetch)
    lv_obj_t* _hdr_host = nullptr;
    lv_obj_t* _hdr_state = nullptr;
    uint32_t  _last_poll_ms = 0;
    bool      _last_ok = false;

    // Per-tab labels (kept here so we can update them in onRunning without
    // re-finding every child label after each fetch).
    // Status tab
    lv_obj_t* _st_service_state = nullptr;
    lv_obj_t* _st_uptime        = nullptr;
    lv_obj_t* _st_backend       = nullptr;
    lv_obj_t* _st_gateway       = nullptr;
    lv_obj_t* _st_video         = nullptr;
    lv_obj_t* _st_wake          = nullptr;
    lv_obj_t* _st_ws_state      = nullptr;
    lv_obj_t* _st_cpu           = nullptr;   // 系统指标（CPU/内存/磁盘/温度合并一行）
    lv_obj_t* _st_daemon        = nullptr;
    lv_obj_t* _st_ha            = nullptr;
    // Motion tab
    lv_obj_t* _mo_mode      = nullptr;
    lv_obj_t* _mo_current   = nullptr;   // current move (or "—")
    lv_obj_t* _mo_queue     = nullptr;   // command queue depth
    lv_obj_t* _mo_loop      = nullptr;   // loop_hz
    lv_obj_t* _mo_antennas  = nullptr;
    lv_obj_t* _mo_gaze      = nullptr;   // gaze source + target rad
    lv_obj_t* _mo_failures  = nullptr;
    // Audio tab
    lv_obj_t* _au_volume_lbl    = nullptr;
    lv_obj_t* _au_volume_slider = nullptr;
    lv_obj_t* _au_mute_btn      = nullptr;
    lv_obj_t* _au_mute_lbl      = nullptr;
    lv_obj_t* _au_mic_btn       = nullptr;
    lv_obj_t* _au_mic_lbl       = nullptr;
    lv_obj_t* _au_vad_lbl       = nullptr;
    lv_obj_t* _au_vad_slider    = nullptr;
    lv_obj_t* _au_control       = nullptr;
    lv_obj_t* _au_status        = nullptr;
    int _last_nonzero_volume = 50;
    // Control tab
    lv_obj_t* _ct_xiaozhi_btn = nullptr;
    lv_obj_t* _ct_qwen_btn = nullptr;
    lv_obj_t* _ct_video_btn = nullptr;
    lv_obj_t* _ct_video_lbl = nullptr;
    lv_obj_t* _ct_voice_dropdown = nullptr;
    lv_obj_t* _ct_camera_btn = nullptr;
    lv_obj_t* _ct_camera_lbl = nullptr;
    lv_obj_t* _ct_wake_btn = nullptr;
    lv_obj_t* _ct_sleep_btn = nullptr;
    lv_obj_t* _ct_daemon_restart_btn = nullptr;
    lv_obj_t* _ct_backend_state = nullptr;
    lv_obj_t* _ct_status = nullptr;
    // System tab
    lv_obj_t* _sy_state = nullptr;
    lv_obj_t* _sy_daemon = nullptr;
    lv_obj_t* _sy_restart_btn = nullptr;
    // Logs tab
    lv_obj_t* _lg_text = nullptr;
    lv_obj_t* _lg_refresh_btn = nullptr;

    // Cached JSON for the active tab only (round-tripped per switch).
    reachy_client::Status    _status;
    reachy_client::Motion    _motion;
    reachy_client::Audio     _audio;
    reachy_client::ControlState _control;
    reachy_client::SystemState _sys;
    bool _has_status = false;
    bool _has_motion = false;
    bool _has_audio  = false;
    bool _has_control = false;
    bool _has_sys    = false;

    // ── Cached fetcher thread state ───────────────────────────────────────
    // onRunning only triggers a fetch; the fetch spawns a std::thread that
    // touches _status etc. via the mutex. The UI reads from the cache.
    std::mutex _cache_mutex;
    std::atomic<bool> _fetch_inflight{false};
    // Pending tab to fetch — set by onRunning, consumed by the spawned thread.
    std::atomic<int>  _fetch_pending{(int)Tab::Status};
    std::atomic<uint32_t> _fetched_at_ms{0};     // last successful fetch stamp
    std::atomic<bool> _fetched_ok{false};        // last fetch result
    uint32_t _rendered_at_ms = 0;                 // last response consumed by LVGL

    enum class OperationKind {
        None, SetVolume, SetMic, SetVad, SetBackend, SetVideo, SetVoice,
        SetCamera, DaemonWake, DaemonSleep, DaemonRestart, RestartYRobot
    };
    struct Operation {
        OperationKind kind = OperationKind::None;
        int int_value = 0;
        float float_value = 0.f;
        bool bool_value = false;
        std::string text_value;
    };
    Operation _pending_operation;
    std::atomic<bool> _operation_pending{false};
    reachy_client::OperationResult _operation_result;
    bool _operation_result_ready = false;
    Operation _confirmed_operation;

    // ── UI helpers ────────────────────────────────────────────────────────
    void _buildUI();
    void _buildHeader();
    void _buildTabBar();
    void _buildStatusPage();
    void _buildMotionPage();
    void _buildAudioPage();
    void _buildControlPage();
    void _buildChatPage();
    void _buildSystemPage();
    void _buildLogsPage();
    void _showOfflineBanner();
    void _switchTab(Tab t);
    void _refreshActiveTab();
    void _refreshHeader();
    void _renderStatus();
    void _renderMotion();
    void _renderAudio();
    void _renderControl();
    bool _queueOperation(Operation operation);
    reachy_client::OperationResult _executeOperation(const Operation& operation);
    reachy_client::OperationResult _switchBackend(const std::string& target);
    void _confirmOperation(Operation operation, const char* message);
    void _renderSystem();
    void _renderLogs();
    void _confirmAndRestart();
    void _confirmRestartCb(lv_event_t* e);
    void _requestClose();
    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _tabCb(lv_event_t* e);
    static void _audioEventCb(lv_event_t* e);
    static void _controlEventCb(lv_event_t* e);
    static void _restartCb(lv_event_t* e);
    static void _logsRefreshCb(lv_event_t* e);
    static void _confirmYesCb(lv_event_t* e);
    static void _confirmNoCb(lv_event_t* e);
    static void _modalBgCb(lv_event_t* e);
    static void _gestureCb(lv_event_t* e);
};
