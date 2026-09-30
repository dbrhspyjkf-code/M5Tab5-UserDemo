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

/** Native 1280x720 cockpit for the YRobot FastAPI service. */
class AppReachy : public mooncake::AppAbility {
public:
    AppReachy();
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static constexpr int W = 1280;
    static constexpr int H = 720;
    static constexpr int HEADER_H = 64;
    static constexpr int DOCK_H = 82;
    static constexpr int CONTENT_H = H - HEADER_H - DOCK_H;
    static constexpr int POLL_MS = 5000;
    static constexpr int CHAT_POLL_MS = 3000;
    static constexpr int VIDEO_POLL_MS = 500;

    enum class Tab : int { Overview, Voice, Interaction, Camera, Maintenance, Count };

    std::function<void()> _close_cb;
    lv_obj_t* _scr = nullptr;
    lv_obj_t* _bottom_dock = nullptr;
    lv_obj_t* _dock_btns[(int)Tab::Count] = {};
    lv_obj_t* _tab_pages[(int)Tab::Count] = {};
    Tab _active = Tab::Overview;
    lv_indev_t* _gesture_indev = nullptr;

    // Header and overview.
    lv_obj_t* _hdr_host = nullptr;
    lv_obj_t* _hdr_state = nullptr;
    lv_obj_t* _st_service_state = nullptr;
    lv_obj_t* _st_uptime = nullptr;
    lv_obj_t* _st_backend = nullptr;
    lv_obj_t* _st_video = nullptr;
    lv_obj_t* _st_wake = nullptr;
    lv_obj_t* _st_daemon = nullptr;
    lv_obj_t* _st_camera_metric = nullptr;
    lv_obj_t* _st_voice_metric = nullptr;
    uint32_t _last_poll_ms = 0;
    bool _last_ok = false;

    // Voice controls.
    lv_obj_t* _au_volume_lbl = nullptr;
    lv_obj_t* _au_volume_slider = nullptr;
    lv_obj_t* _au_mute_btn = nullptr;
    lv_obj_t* _au_mute_lbl = nullptr;
    lv_obj_t* _au_mic_btn = nullptr;
    lv_obj_t* _au_mic_lbl = nullptr;
    lv_obj_t* _au_vad_lbl = nullptr;
    lv_obj_t* _au_vad_slider = nullptr;
    lv_obj_t* _au_status = nullptr;
    int _last_nonzero_volume = 50;
    lv_obj_t* _ct_xiaozhi_btn = nullptr;
    lv_obj_t* _ct_qwen_btn = nullptr;
    lv_obj_t* _ct_video_btn = nullptr;
    lv_obj_t* _ct_video_lbl = nullptr;
    lv_obj_t* _ct_voice_dropdown = nullptr;
    lv_obj_t* _ct_camera_btn = nullptr;
    lv_obj_t* _ct_camera_lbl = nullptr;
    lv_obj_t* _ct_backend_state = nullptr;

    // Interaction and recent conversation.
    lv_obj_t* _mo_mode = nullptr;
    lv_obj_t* _mo_current = nullptr;
    lv_obj_t* _mo_queue = nullptr;
    lv_obj_t* _mo_loop = nullptr;
    lv_obj_t* _mo_antennas = nullptr;
    lv_obj_t* _mo_gaze = nullptr;
    lv_obj_t* _chat_container = nullptr;
    std::string _chat_signature;

    // Camera.
    lv_obj_t* _vi_image = nullptr;
    lv_obj_t* _vi_status = nullptr;
    lv_obj_t* _vi_toggle_btn = nullptr;
    lv_obj_t* _vi_toggle_lbl = nullptr;
    lv_image_dsc_t _video_dsc = {};
    std::string _video_frame;
    std::string _video_pending_frame;
    std::atomic<bool> _video_preview_enabled{false};
    std::atomic<bool> _video_fetch_inflight{false};
    std::atomic<bool> _video_frame_ready{false};
    uint32_t _last_video_poll_ms = 0;

    // Maintenance.
    lv_obj_t* _sy_state = nullptr;
    lv_obj_t* _sy_daemon = nullptr;
    lv_obj_t* _sy_restart_btn = nullptr;
    lv_obj_t* _ct_wake_btn = nullptr;
    lv_obj_t* _ct_sleep_btn = nullptr;
    lv_obj_t* _ct_daemon_restart_btn = nullptr;
    lv_obj_t* _lg_text = nullptr;
    lv_obj_t* _lg_refresh_btn = nullptr;
    std::atomic<bool> _logs_requested{false};

    // Cached results are only written by detached workers and rendered by the UI thread.
    reachy_client::Status _status;
    reachy_client::Motion _motion;
    reachy_client::Audio _audio;
    reachy_client::ControlState _control;
    std::vector<reachy_client::ChatMessage> _chat_messages;
    reachy_client::SystemState _sys;
    bool _has_status = false;
    bool _has_motion = false;
    bool _has_audio = false;
    bool _has_control = false;
    bool _has_chat = false;
    bool _has_sys = false;
    std::string _logs_text;
    bool _has_logs = false;

    std::mutex _cache_mutex;
    std::atomic<bool> _fetch_inflight{false};
    std::atomic<uint32_t> _fetched_at_ms{0};
    std::atomic<bool> _fetched_ok{false};
    std::atomic<bool> _last_fetch_reachable{false};
    uint32_t _rendered_at_ms = 0;
    std::atomic<uint32_t> _open_generation{0};
    std::atomic<uint32_t> _completed_generation{0};

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

    void _buildUI();
    void _buildHeader();
    void _buildBottomDock();
    void _buildOverviewPage();
    void _buildVoicePage();
    void _buildInteractionPage();
    void _buildCameraPage();
    void _buildMaintenancePage();
    void _selectDestination(Tab t);
    void _refreshActiveTab();
    void _refreshHeader();
    void _showOfflineBanner();
    void _renderOverview();
    void _renderVoice();
    void _renderInteraction();
    void _renderChat();
    void _renderMaintenance();
    void _renderLogs();
    void _pollVideoFrame();
    void _renderVideoFrame();
    void _stopVideoPreview();
    bool _queueOperation(Operation operation);
    reachy_client::OperationResult _executeOperation(const Operation& operation);
    reachy_client::OperationResult _switchBackend(const std::string& target);
    void _confirmOperation(Operation operation, const char* message);
    void _confirmAndRestart();
    void _requestClose();
    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _tabCb(lv_event_t* e);
    static void _audioEventCb(lv_event_t* e);
    static void _controlEventCb(lv_event_t* e);
    static void _videoToggleCb(lv_event_t* e);
    static void _restartCb(lv_event_t* e);
    static void _logsRefreshCb(lv_event_t* e);
    static void _confirmYesCb(lv_event_t* e);
    static void _confirmNoCb(lv_event_t* e);
    static void _modalBgCb(lv_event_t* e);
    static void _gestureCb(lv_event_t* e);
};
