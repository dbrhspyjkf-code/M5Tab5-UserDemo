// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Sonos controller (talks to local tool server, NOT Home Assistant).
// Mirrors the HomeControl-iOS Sonos API surface:
//   GET  /api/sonos/state
//   POST /api/tools/call  {name:"control_sonos", arguments:{action:...}}
class SonosClient {
public:
    struct Config {
        std::string baseUrl;           // e.g. "http://<host>:<port>"
        int pollIntervalMsPlaying  = 5000;
        int pollIntervalMsIdle     = 30000;
    };

    struct State {
        bool   ok            = false;
        std::string raw_state;        // "PLAYING" / "PAUSED_PLAYBACK" / "STOPPED" / "TRANSITIONING"
        int    volume        = 0;      // 0..100
        bool   muted         = false;
        bool   is_tv         = false;  // content is TV input
        std::string artist;
        std::string title;
        std::string album;
        uint32_t last_update_ms = 0;
    };

    void init(Config cfg);
    void destroy();

    // Thread-safe snapshot of the latest state
    State getState() const;

    // Whether a /api/tools/call is currently in flight
    bool isBusy() const { return _busy.load(); }

    // Send a control command: play | pause | prev | next | volume_up |
    //                        volume_down | mute | tv
    void sendCommand(const std::string& action);

    // Force a state fetch on the next poll tick (used by the card refresh btn)
    void requestRefresh();

private:
    Config _cfg;
    mutable std::mutex _mutex;
    State  _state;

    std::thread        _poll_thread;
    std::atomic<bool>  _running{false};
    std::atomic<bool>  _busy{false};
    std::atomic<bool>  _refresh_requested{false};

    std::vector<std::thread> _workers;
    std::mutex               _worker_mutex;

    void _poll_loop();
    void _fetch_state();
    void _start_worker(std::function<void()> fn);
    void _join_workers();
};
