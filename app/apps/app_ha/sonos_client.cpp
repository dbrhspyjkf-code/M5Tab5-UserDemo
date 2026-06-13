// SPDX-License-Identifier: MIT
#include "sonos_client.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
static const std::string _tag = "sonos";

void SonosClient::init(Config cfg)
{
    _cfg     = cfg;
    _running = true;
    _poll_thread = std::thread([this]() { _poll_loop(); });
}

void SonosClient::destroy()
{
    _running = false;
    if (_poll_thread.joinable()) _poll_thread.join();
    _join_workers();
}

SonosClient::State SonosClient::getState() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _state;
}

void SonosClient::requestRefresh()
{
    _refresh_requested = true;
}

void SonosClient::sendCommand(const std::string& action)
{
    if (!_running.load()) return;

    std::string url   = _cfg.baseUrl + "/api/tools/call";
    std::string body  = std::string("{\"name\":\"control_sonos\",")
                      + "\"arguments\":{\"action\":\"" + action + "\"}}";

    _busy = true;
    _start_worker([this, url, body, action]() {
        auto resp = GetHAL()->httpPost(url, body,
            {{"Content-Type", "application/json"}});
        if (!resp.ok) {
            mclog::tagWarn(_tag, "sendCommand({}) failed: {}", action, resp.status);
        } else {
            mclog::tagInfo(_tag, "sendCommand({}) ok", action);
        }
        _busy = false;
        // Trigger an immediate state refresh so the UI shows the new state fast
        _refresh_requested = true;
    });
}

void SonosClient::_start_worker(std::function<void()> fn)
{
    if (!_running.load()) return;
    std::lock_guard<std::mutex> lock(_worker_mutex);
    _workers.emplace_back(std::move(fn));
}

void SonosClient::_join_workers()
{
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(_worker_mutex);
        workers.swap(_workers);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

void SonosClient::_poll_loop()
{
    // Initial fetch
    _fetch_state();

    auto last = std::chrono::steady_clock::now();
    while (_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (_refresh_requested.load()) {
            _refresh_requested = false;
            _fetch_state();
            last = std::chrono::steady_clock::now();
            continue;
        }

        State s;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            s = _state;
        }
        // Adaptive interval: 5s when playing, 30s when idle
        int interval = (s.ok && s.raw_state == "PLAYING")
            ? _cfg.pollIntervalMsPlaying
            : _cfg.pollIntervalMsIdle;

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        if (elapsed >= interval) {
            _fetch_state();
            last = std::chrono::steady_clock::now();
        }
    }
}

void SonosClient::_fetch_state()
{
    if (!GetHAL()) return;

    auto resp = GetHAL()->httpGet(_cfg.baseUrl + "/api/sonos/state");
    if (!resp.ok) {
        mclog::tagWarn(_tag, "fetchState failed: {}", resp.status);
        std::lock_guard<std::mutex> lock(_mutex);
        _state.ok = false;
        return;
    }

    try {
        auto j = json::parse(resp.body);
        State ns;
        ns.ok         = j.value("ok", false);
        ns.raw_state  = j.value("state", "");
        ns.volume     = j.value("volume", 0);
        ns.muted      = j.value("muted", false);
        ns.is_tv      = j.value("is_tv", false);
        ns.artist     = j.value("artist", "");
        ns.title      = j.value("title", "");
        ns.album      = j.value("album", "");
        if (GetHAL()) ns.last_update_ms = (uint32_t)GetHAL()->millis();

        std::lock_guard<std::mutex> lock(_mutex);
        _state = std::move(ns);
    } catch (const std::exception& ex) {
        mclog::tagWarn(_tag, "parse error: {}", ex.what());
        std::lock_guard<std::mutex> lock(_mutex);
        _state.ok = false;
    }
}
