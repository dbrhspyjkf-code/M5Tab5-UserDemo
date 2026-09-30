// SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
// SPDX-License-Identifier: MIT
//
// reachy_client.h — HTTP client for the YRobot FastAPI dashboard running on
// the Reachy Mini (see /Users/leenzhou/Projects/YRobot-reachy-current).
//
// Field names mirror the real YRobot /api/* responses (verified against
// the live robot on 192.168.1.14:8042). Parsing is best-effort: malformed
// JSON or missing fields leave the struct in its default state and the UI
// shows "—".

#pragma once
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace reachy_client {

inline std::string _host() {
    auto h = GetHAL()->getConfig("reachy_host", std::string("192.168.1.14"));
    if (h.empty()) h = "192.168.1.14";
    auto strip = [](std::string s) {
        if (s.rfind("http://", 0) == 0) s.erase(0, 7);
        else if (s.rfind("https://", 0) == 0) s.erase(0, 8);
        auto slash = s.find('/');
        if (slash != std::string::npos) s.erase(slash);
        return s;
    };
    return strip(h);
}

inline std::string _base() { return "http://" + _host() + ":8042"; }

// ── Status (GET /api/status) ─────────────────────────────────────────────────
// Real schema (verified 2026-08-13, see also app_config.py:build_status):
//   {
//     "ok": true,
//     "status": {
//       "service":    {name, state, pid, uptime_s},
//       "system":     {cpu_percent, memory_percent, disk_percent, temperature_c,
//                      power:{...}},
//       "daemon":     {available, base_url, firmware_version, hardware_id,
//                      robot_name, daemon_state, motor_mode, awake,
//                      app_lock_state, active_app, ...},
//       "motion":     {ready, thread_alive, mode, current_move,
//                      command_queue, loop_hz, last_tick_ms,
//                      set_target_consecutive_failures, antennas[2],
//                      gaze_target_rad, gaze_source, gaze_age_s, tracking},
//       "runtime":    {wake_active, wake_phrase, wake_enabled, ws_state,
//                      backend, reconnect_count, ...},
//       "conversation": {gateway_url, realtime_mode, tls_verify,
//                        video_enabled, video_interval_ms, video_fps,
//                        proactive_enabled, configured_backend, backend},
//       "audio":      {volume_percent, control, range[2], volume_error,
//                      mic:{voiced, level_db, level_percent, rms, available},
//                      input_enabled},
//       "integrations": {home_assistant:{enabled, configured, url},
//                         local_info:{enabled}},
//       "privacy":    {audio_uploaded_to_gateway, video_uploaded_to_gateway},
//     }
//   }
struct Status {
    bool ok = false;
    // service
    std::string service_name;
    std::string service_state;     // "active"/"idle"/...
    int         pid = 0;
    int         uptime_s = 0;
    // system
    float cpu_percent = 0.f;
    float memory_percent = 0.f;
    float disk_percent = 0.f;
    float temperature_c = 0.f;
    // daemon
    std::string daemon_state;      // "running"/"stopped"/...
    bool        daemon_available = false;
    std::string daemon_base_url;
    std::string daemon_firmware;
    // motion (subset; the rest lives in Motion struct pulled from /api/motion)
    std::string motion_mode;
    bool        motion_ready = false;
    bool        motion_thread_alive = false;
    int         motion_queue = 0;
    float       motion_loop_hz = 0.f;
    int         motion_deadline_misses = 0;
    int         set_target_consecutive_failures = 0;
    std::string motion_current_move;
    // antennas is a 2-element array; stored as a "L,R" string
    std::string motion_antennas;
    // gaze
    std::string motion_gaze_source;
    float       motion_gaze_target_rad = 0.f;
    bool        motion_face_detected = false;
    // runtime
    std::string wake_phrase;
    bool        wake_enabled = false;
    bool        wake_active = false;
    std::string ws_state;          // "connected"/"paused"/"disconnected"
    std::string runtime_backend;
    int         reconnects = 0;
    bool        tts_active = false;
    // conversation
    std::string gateway_url;
    std::string realtime_mode;
    bool        tls_verify = false;
    bool        video_enabled = false;
    float       video_fps = 0.f;
    int         video_interval_ms = 0;
    bool        proactive_enabled = false;
    std::string configured_backend;
    // audio
    int         volume_percent = -1;
    int         volume_min = 0;
    int         volume_max = 100;
    std::string volume_control;   // "PCM"/"alsa"/...
    std::string volume_error;
    bool        mic_input_enabled = true;
    bool        mic_voiced = false;
    float       mic_level_db = 0.f;
    float       mic_rms = 0.f;
    // integrations
    bool        ha_enabled = false;
    bool        ha_configured = false;
    std::string ha_url;
    bool        local_info_enabled = false;
    // privacy
    bool        audio_uploaded = false;
    bool        video_uploaded = false;
};

inline Status fetchStatus() {
    Status s;
    auto resp = GetHAL()->httpGet(_base() + "/api/status");
    if (!resp.ok) return s;
    try {
        auto j = nlohmann::json::parse(resp.body);
        s.ok = j.value("ok", false);
        // The dashboard returns the entire payload under both "ok" and
        // "status"; the metrics nests inside j["status"].
        auto& st = j["status"];
        if (st.contains("service")) {
            auto& svc = st["service"];
            s.service_name  = svc.value("name", "");
            s.service_state = svc.value("state", "");
            s.pid           = svc.value("pid", 0);
            s.uptime_s      = svc.value("uptime_s", 0);
        }
        if (st.contains("system")) {
            auto& sys = st["system"];
            s.cpu_percent     = sys.value("cpu_percent", 0.f);
            s.memory_percent  = sys.value("memory_percent", 0.f);
            s.disk_percent    = sys.value("disk_percent", 0.f);
            s.temperature_c   = sys.value("temperature_c", 0.f);
        }
        if (st.contains("daemon")) {
            auto& d = st["daemon"];
            s.daemon_state      = d.value("daemon_state", "");
            s.daemon_available  = d.value("available", false);
            s.daemon_base_url   = d.value("base_url", "");
            s.daemon_firmware   = d.value("firmware_version", "");
        }
        if (st.contains("motion")) {
            auto& m = st["motion"];
            s.motion_mode        = m.value("mode", "");
            s.motion_ready       = m.value("ready", false);
            s.motion_thread_alive = m.value("thread_alive", false);
            s.motion_queue       = m.value("command_queue", 0);
            s.motion_loop_hz     = m.value("loop_hz", 0.f);
            s.motion_deadline_misses = m.value("deadline_misses", 0);
            s.set_target_consecutive_failures =
                m.value("set_target_consecutive_failures", 0);
            if (m.contains("current_move") && m["current_move"].is_string())
                s.motion_current_move = m["current_move"].get<std::string>();
            if (m.contains("antennas") && m["antennas"].is_array() && m["antennas"].size() == 2) {
                char buf[40];
                snprintf(buf, sizeof(buf), "%.2f, %.2f",
                         m["antennas"][0].get<float>(),
                         m["antennas"][1].get<float>());
                s.motion_antennas = buf;
            }
            s.motion_gaze_target_rad = m.value("gaze_target_rad", 0.f);
            s.motion_gaze_source     = m.value("gaze_source", "");
            if (m.contains("tracking") && m["tracking"].is_object()) {
                s.motion_face_detected = m["tracking"].value("face_detected", false);
            }
        }
        if (st.contains("runtime")) {
            auto& r = st["runtime"];
            s.wake_phrase    = r.value("wake_phrase", "");
            s.wake_enabled   = r.value("wake_enabled", false);
            s.wake_active    = r.value("wake_active", false);
            s.ws_state       = r.value("ws_state", "");
            s.runtime_backend = r.value("backend", "");
            s.reconnects     = r.value("reconnects", 0);
            s.tts_active     = r.value("tts_active", false);
        }
        if (st.contains("conversation")) {
            auto& c = st["conversation"];
            s.gateway_url        = c.value("gateway_url", "");
            s.realtime_mode      = c.value("realtime_mode", "");
            s.tls_verify         = c.value("tls_verify", false);
            s.video_enabled      = c.value("video_enabled", false);
            s.video_fps          = c.value("video_fps", 0.f);
            s.video_interval_ms  = c.value("video_interval_ms", 0);
            s.proactive_enabled  = c.value("proactive_enabled", false);
            s.configured_backend = c.value("configured_backend", "");
        }
        if (st.contains("audio")) {
            auto& a = st["audio"];
            if (a.contains("volume_percent") && !a["volume_percent"].is_null())
                s.volume_percent = a["volume_percent"].get<int>();
            if (a.contains("range") && a["range"].is_array() && a["range"].size() == 2) {
                s.volume_min = a["range"][0].get<int>();
                s.volume_max = a["range"][1].get<int>();
            }
            s.volume_control = a.value("control", "");
            s.volume_error   = a.value("volume_error", "");
            // volume_error surfaces as JSON null when no error; json::parse
            // returns null island; default to "" handled above.
            if (a.contains("volume_error") && a["volume_error"].is_null())
                s.volume_error.clear();
            s.mic_input_enabled = a.value("input_enabled", true);
            if (a.contains("mic") && a["mic"].is_object()) {
                s.mic_voiced   = a["mic"].value("voiced", false);
                s.mic_level_db = a["mic"].value("level_db", 0.f);
                s.mic_rms      = a["mic"].value("rms", 0.f);
            }
        }
        if (st.contains("integrations")) {
            auto& i = st["integrations"];
            if (i.contains("home_assistant")) {
                auto& h = i["home_assistant"];
                s.ha_enabled    = h.value("enabled", false);
                s.ha_configured = h.value("configured", false);
                s.ha_url        = h.value("url", "");
            }
            if (i.contains("local_info")) {
                s.local_info_enabled = i["local_info"].value("enabled", false);
            }
        }
        if (st.contains("privacy")) {
            auto& p = st["privacy"];
            s.audio_uploaded = p.value("audio_uploaded_to_gateway", false);
            s.video_uploaded = p.value("video_uploaded_to_gateway", false);
        }
    } catch (...) {
        // parse failed
    }
    return s;
}

// ── Motion (GET /api/motion) ─────────────────────────────────────────────────
// Real schema: { "ok": true, "moves": [<list of available moves>], "current": <name|null> }
// We extend this with the richer `status.motion` subtree via the status cache
// to fill `mode`, `loop_hz`, etc., since /api/motion alone has none of those.
struct Motion {
    bool ok = false;
    std::string current_move;       // from /api/motion, may be empty
    std::vector<std::string> moves; // from /api/motion
    // Mirrored from status.motion (filled by fetchMotion combined with cached Status)
    std::string mode;
    int         command_queue = 0;
    float       loop_hz = 0.f;
    int         deadline_misses = 0;
    int         set_target_consecutive_failures = 0;
    std::string antennas;
    std::string gaze_source;
    float       gaze_target_rad = 0.f;
    bool        face_detected = false;
};

inline Motion fetchMotion() {
    Motion m;
    auto resp = GetHAL()->httpGet(_base() + "/api/motion");
    if (!resp.ok) return m;
    try {
        auto j = nlohmann::json::parse(resp.body);
        m.ok = j.value("ok", false);
        if (j.contains("moves") && j["moves"].is_array()) {
            for (auto& el : j["moves"]) {
                if (el.is_string()) m.moves.push_back(el.get<std::string>());
            }
        }
        if (j.contains("current") && j["current"].is_string()) {
            m.current_move = j["current"].get<std::string>();
        }
    } catch (...) {}
    return m;
}

// Merge an authoritative Status into a Motion so the MOTION tab can show
// runtime metrics (loop_hz, queue, etc.) without a second HTTP round-trip.
inline Motion mergeStatusIntoMotion(Motion m, const Status& s) {
    m.mode = s.motion_mode;
    m.command_queue = s.motion_queue;
    m.loop_hz = s.motion_loop_hz;
    m.deadline_misses = s.motion_deadline_misses;
    m.set_target_consecutive_failures = s.set_target_consecutive_failures;
    m.antennas = s.motion_antennas;
    m.gaze_source = s.motion_gaze_source;
    m.gaze_target_rad = s.motion_gaze_target_rad;
    m.face_detected = s.motion_face_detected;
    return m;
}

// ── Audio (GET /api/volume + /api/audio/vad + /api/audio/input) ──────────────
// Real /api/volume: { "volume": { "percent": 88, "control": "PCM", "range": [0,60] } }
// Real /api/audio/vad: { "vad": { "rms_min": 0.05, "min": ..., "max": ..., "step": ..., "unit": "RMS" } }
// Real /api/audio/input: { "audio_input": { "enabled": false } }
struct Audio {
    bool ok = false;
    int  volume_percent = -1;
    int  volume_min = 0;
    int  volume_max = 100;
    std::string volume_control;
    std::string volume_error;
    bool mic_input_enabled = true;
    float vad_rms_min = 0.f;
    std::string vad_unit;       // "RMS"
};

inline Audio fetchAudio() {
    Audio a;
    auto r1 = GetHAL()->httpGet(_base() + "/api/volume");
    if (r1.ok) {
        try {
            auto j = nlohmann::json::parse(r1.body);
            if (j.contains("volume") && j["volume"].is_object()) {
                auto& v = j["volume"];
                if (v.contains("percent") && !v["percent"].is_null())
                    a.volume_percent = v["percent"].get<int>();
                if (v.contains("range") && v["range"].is_array() && v["range"].size() == 2) {
                    a.volume_min = v["range"][0].get<int>();
                    a.volume_max = v["range"][1].get<int>();
                }
                a.volume_control = v.value("control", "");
            }
        } catch (...) {}
    }
    auto r2 = GetHAL()->httpGet(_base() + "/api/audio/input");
    if (r2.ok) {
        try {
            auto j = nlohmann::json::parse(r2.body);
            if (j.contains("audio_input") && j["audio_input"].is_object())
                a.mic_input_enabled = j["audio_input"].value("enabled", true);
        } catch (...) {}
    }
    auto r3 = GetHAL()->httpGet(_base() + "/api/audio/vad");
    if (r3.ok) {
        try {
            auto j = nlohmann::json::parse(r3.body);
            if (j.contains("vad") && j["vad"].is_object()) {
                auto& v = j["vad"];
                a.vad_rms_min = v.value("rms_min", 0.f);
                a.vad_unit    = v.value("unit", "");
            }
        } catch (...) {}
    }
    a.ok = r1.ok || r2.ok || r3.ok;
    return a;
}

// ── Dashboard controls ───────────────────────────────────────────────────────
struct ControlState {
    bool ok = false;
    std::string configured_backend;
    std::string running_backend;
    std::string connection_state;
    std::string backend_error;
    bool video_enabled = false;
    std::string configured_voice;
    std::vector<std::string> available_voices;
    bool camera_running = false;
};

struct ChatMessage {
    enum class Role { User, Assistant };
    Role role = Role::Assistant;
    std::string text;
};

struct OperationResult {
    bool ok = false;
    int status = 0;
    std::string error;
};

inline const std::vector<std::pair<std::string, std::string>>& _jsonHeaders() {
    static const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}
    };
    return headers;
}

inline OperationResult _operationResult(const hal::HalBase::HttpResponse_t& resp) {
    OperationResult result{resp.ok, resp.status, ""};
    if (!resp.ok) {
        result.error = resp.body.empty() ? "HTTP " + std::to_string(resp.status) : resp.body;
    }
    return result;
}

inline OperationResult setVolume(int percent) {
    auto body = nlohmann::json{{"percent", percent}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/volume", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult setMicEnabled(bool enabled) {
    auto body = nlohmann::json{{"enabled", enabled}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/audio/input", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult setVad(float rms_min) {
    auto body = nlohmann::json{{"rms_min", rms_min}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/audio/vad", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult setBackend(const std::string& backend) {
    if (backend != "xiaozhi" && backend != "qwen")
        return {false, 0, "invalid backend"};
    auto body = nlohmann::json{{"backend", backend}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/conversation/backend", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult setVideoEnabled(bool enabled) {
    auto body = nlohmann::json{{"enabled", enabled}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/conversation/video", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult setVoice(const std::string& voice) {
    auto body = nlohmann::json{{"voice", voice}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/conversation/voice", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult setCameraRunning(bool running) {
    auto body = nlohmann::json{{"running", running}}.dump();
    return _operationResult(GetHAL()->httpPut(_base() + "/api/camera/state", body,
                                             {{"Content-Type", "application/json"}}));
}

inline OperationResult daemonAction(const std::string& action) {
    if (action != "wake" && action != "sleep" && action != "restart")
        return {false, 0, "invalid daemon action"};
    auto body = nlohmann::json{{"action", action}}.dump();
    return _operationResult(GetHAL()->httpPost(_base() + "/api/reachy-daemon/action", body,
                                              {{"Content-Type", "application/json"}}));
}

inline OperationResult restartYRobot() {
    return _operationResult(GetHAL()->httpPost(_base() + "/api/system/restart", "",
                                              {{"Content-Type", "application/json"}}));
}

inline ControlState fetchControlState() {
    ControlState state;
    auto backend = GetHAL()->httpGet(_base() + "/api/conversation/backend");
    auto video = GetHAL()->httpGet(_base() + "/api/conversation/video");
    auto voice = GetHAL()->httpGet(_base() + "/api/conversation/voice");
    auto camera = GetHAL()->httpGet(_base() + "/api/camera/state");
    try {
        if (backend.ok) {
            auto j = nlohmann::json::parse(backend.body);
            state.configured_backend = j.value("configured_backend", "");
            state.running_backend = j.value("running_backend", "");
            state.connection_state = j.value("connection_state", "");
            if (j.contains("error") && j["error"].is_string())
                state.backend_error = j["error"].get<std::string>();
        }
        if (video.ok) {
            auto j = nlohmann::json::parse(video.body);
            state.video_enabled = j.value("video_enabled", false);
        }
        if (voice.ok) {
            auto j = nlohmann::json::parse(voice.body);
            state.configured_voice = j.value("configured_voice", "");
            if (j.contains("available_voices") && j["available_voices"].is_array())
                for (auto& item : j["available_voices"])
                    if (item.is_string()) state.available_voices.push_back(item.get<std::string>());
        }
        if (camera.ok) {
            auto j = nlohmann::json::parse(camera.body);
            if (j.contains("state") && j["state"].is_object())
                state.camera_running = j["state"].value("running", false);
        }
    } catch (...) {}
    state.ok = backend.ok || video.ok || voice.ok || camera.ok;
    return state;
}

inline std::vector<ChatMessage> fetchChat(int limit = 200, size_t max_user_turns = 6) {
    std::vector<ChatMessage> messages;
    std::vector<size_t> user_starts;
    auto resp = GetHAL()->httpGet(
        _base() + "/api/logs?filter=chat&limit=" + std::to_string(limit));
    if (!resp.ok) return messages;
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (!j.contains("logs") || !j["logs"].is_array()) return messages;
        for (auto& item : j["logs"]) {
            if (!item.is_object()) continue;
            std::string line = item.value("message", "");
            struct Marker { const char* token; ChatMessage::Role role; };
            static const Marker markers[] = {
                {"xz stt:", ChatMessage::Role::User},
                {"xz tts text:", ChatMessage::Role::Assistant},
                {"qwen stt:", ChatMessage::Role::User},
                {"qwen response:", ChatMessage::Role::Assistant},
            };
            for (auto& marker : markers) {
                auto pos = line.find(marker.token);
                if (pos == std::string::npos) continue;
                std::string text = line.substr(pos + std::char_traits<char>::length(marker.token));
                auto first = text.find_first_not_of(" \t\r\n");
                if (first == std::string::npos) break;
                text.erase(0, first);
                auto last = text.find_last_not_of(" \t\r\n");
                if (last != std::string::npos) text.erase(last + 1);
                if (marker.role == ChatMessage::Role::Assistant && text.rfind("% tool", 0) == 0)
                    break;
                if (marker.role == ChatMessage::Role::Assistant && text.rfind("% ", 0) == 0)
                    break;
                if (marker.role == ChatMessage::Role::User) user_starts.push_back(messages.size());
                messages.push_back({marker.role, std::move(text)});
                break;
            }
        }
    } catch (...) {
        return {};
    }
    if (max_user_turns == 0) return {};
    if (user_starts.size() > max_user_turns) {
        size_t keep_from = user_starts[user_starts.size() - max_user_turns];
        messages.erase(messages.begin(), messages.begin() + keep_from);
    } else if (!user_starts.empty() && user_starts.front() > 0) {
        messages.erase(messages.begin(), messages.begin() + user_starts.front());
    }
    return messages;
}

inline std::string fetchCameraFrame() {
    auto resp = GetHAL()->httpGet(_base() + "/api/camera/frame");
    if (!resp.ok || resp.body.size() < 4 ||
        static_cast<uint8_t>(resp.body[0]) != 0xFF ||
        static_cast<uint8_t>(resp.body[1]) != 0xD8) return {};
    return std::move(resp.body);
}

// ── System (GET /api/system/state) ──────────────────────────────────────────
// Real schema: { "state": { "service": "yrobot.service", "running": true,
//                         "pid": 9998, "uptime_s": 2544 } }
struct SystemState {
    bool ok = false;
    std::string service;       // systemd service name
    bool running = false;
    int  pid = 0;
    int  uptime_s = 0;
};

inline SystemState fetchSystemState() {
    SystemState s;
    auto resp = GetHAL()->httpGet(_base() + "/api/system/state");
    if (!resp.ok) return s;
    try {
        auto j = nlohmann::json::parse(resp.body);
        if (j.contains("state") && j["state"].is_object()) {
            auto& st = j["state"];
            s.ok       = true;
            s.service  = st.value("service", "");
            s.running  = st.value("running", false);
            s.pid      = st.value("pid", 0);
            s.uptime_s = st.value("uptime_s", 0);
        }
    } catch (...) {}
    return s;
}

inline bool postRestart() {
    auto resp = GetHAL()->httpPost(_base() + "/api/system/restart", "", {});
    return resp.ok;
}

// ── Logs (GET /api/logs?filter=chat&limit=200) ───────────────────────────────
inline std::string fetchLogsRaw(int limit = 200) {
    auto resp = GetHAL()->httpGet(
        _base() + "/api/logs?filter=chat&limit=" + std::to_string(limit));
    if (!resp.ok) return "";
    return resp.body;
}

// Cheap connectivity ping.
inline bool ping() {
    auto resp = GetHAL()->httpGet(_base() + "/api/system/state");
    return resp.ok;
}

// ── Mobile base remote control (parity with the iOS Base Control) ──────────
// Every call goes through the audited YRobot :8042 proxy, which alone holds
// the HMAC gateway credentials. The Tab5 never talks to the mobile-base
// gateway directly and never signs anything. Semantics mirror the desktop
// baseMoveClient.ts: fail-closed interlocks, deadman-gated motion, 20 Hz
// frames, and zero+release on every exit path. The server re-validates and
// owns the final caps (lease TTL 250 ms, velocity clamps, watchdog).
struct BaseMoveStatus {
    bool ok = false;             // remote/status reachable
    bool lease_active = false;   // some remote lease exists (any owner)
    int  remaining_ms = 0;
    bool goto_known = false;     // goto-mode query succeeded
    bool goto_on = false;
    bool gamepad_known = false;  // gamepad-mode query succeeded
    bool gamepad_on = false;
    std::string error;
};

struct BaseMoveLease {
    bool ok = false;
    std::string session_id;      // opaque server token, [A-Za-z0-9_-]{1,128}
    int  remaining_ms = 0;
    std::string error;
};

struct BaseMoveResult {
    bool ok = false;
    int  status = 0;
    std::string error;
};

inline BaseMoveResult _baseResult(const hal::HalBase::HttpResponse_t& resp) {
    BaseMoveResult result{resp.ok, resp.status, ""};
    if (!resp.ok) {
        std::string detail;
        try {
            auto j = nlohmann::json::parse(resp.body);
            detail = j.value("detail", "");
        } catch (...) {}
        result.error = detail.empty()
            ? (resp.body.empty() ? "HTTP " + std::to_string(resp.status) : resp.body)
            : detail;
    }
    return result;
}

// Client-side velocity caps mirror the iOS virtual joystick mapping so both
// remotes steer identically; the gateway still re-clamps everything.
constexpr float BASE_MAX_LINEAR_X  = 0.12f;   // m/s
constexpr float BASE_MAX_ANGULAR_Z = 0.45f;   // rad/s

inline BaseMoveStatus fetchBaseStatus() {
    BaseMoveStatus s;
    auto resp = GetHAL()->httpGet(_base() + "/api/mobile-base/remote/status");
    if (!resp.ok) { s.error = "无法读取底盘状态"; return s; }
    try {
        auto j = nlohmann::json::parse(resp.body);
        s.ok            = j.value("ok", false);
        s.lease_active  = j.value("lease_active", j.value("active", false));
        s.remaining_ms  = j.value("remaining_ms",
            static_cast<int>(j.value("lease_expires_in_s", 0.0) * 1000.0));
    } catch (...) { s.error = "底盘状态解析失败"; return s; }
    auto gotoResp = GetHAL()->httpGet(_base() + "/api/mobile-base/goto-mode");
    if (gotoResp.ok) {
        try {
            auto j = nlohmann::json::parse(gotoResp.body);
            s.goto_known = true;
            s.goto_on    = j.value("on", false);
        } catch (...) {}
    }
    auto padResp = GetHAL()->httpGet(_base() + "/api/mobile-base/gamepad-mode");
    if (padResp.ok) {
        try {
            auto j = nlohmann::json::parse(padResp.body);
            s.gamepad_known = true;
            s.gamepad_on    = j.value("on", false);
        } catch (...) {}
    }
    return s;
}

inline BaseMoveLease baseAcquire() {
    BaseMoveLease lease;
    // The fixed proxy API requires an explicit empty JSON object here.
    auto resp = GetHAL()->httpPost(_base() + "/api/mobile-base/remote/acquire", "{}",
                                   _jsonHeaders());
    auto result = _baseResult(resp);
    lease.ok = result.ok;
    lease.error = result.error;
    if (!result.ok) return lease;
    try {
        auto j = nlohmann::json::parse(resp.body);
        lease.session_id = j.value("session_id", "");
        if (lease.session_id.empty() || lease.session_id.size() > 128 ||
            lease.session_id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
                != std::string::npos) {
            return {false, "", 0, "底盘返回了无效的会话标识"};
        }
        lease.remaining_ms = j.value("remaining_ms",
            static_cast<int>(j.value("lease_expires_in_s", 0.0) * 1000.0));
    } catch (...) {
        return {false, "", 0, "底盘响应解析失败"};
    }
    return lease;
}

inline BaseMoveResult baseFrame(const std::string& session_id, uint32_t sequence,
                                float linear_x, float angular_z, bool deadman) {
    // Mirror the server's 422 contract client-side: exact schema, positive
    // sequence, and deadman must gate any non-zero velocity.
    if (session_id.empty() || session_id.size() > 128) return {false, 0, "无效会话"};
    if (sequence == 0) return {false, 0, "sequence 必须为正整数"};
    if (!deadman && (linear_x != 0.f || angular_z != 0.f))
        return {false, 0, "非零速度需要按住 Deadman"};
    auto body = nlohmann::json{
        {"session_id", session_id},
        {"sequence", sequence},
        {"linear_x", linear_x},
        {"angular_z", angular_z},
        {"deadman", deadman},
    }.dump();
    return _baseResult(GetHAL()->httpPost(_base() + "/api/mobile-base/remote/frame",
                                          body, _jsonHeaders()));
}

inline BaseMoveResult baseRelease(const std::string& session_id) {
    if (session_id.empty() || session_id.size() > 128) return {false, 0, "无效会话"};
    auto body = nlohmann::json{{"session_id", session_id}}.dump();
    return _baseResult(GetHAL()->httpPost(_base() + "/api/mobile-base/remote/release",
                                          body, _jsonHeaders()));
}

inline BaseMoveResult baseStop() {
    // Remote R2 semantics: STOP only — this can never re-arm autonomy or the
    // physical gamepad; it parks the base through the audited proxy path.
    return _baseResult(GetHAL()->httpPost(_base() + "/api/mobile-base/remote/stop",
                                          "{}", _jsonHeaders()));
}

}  // namespace reachy_client
