// SPDX-License-Identifier: MIT
#include "sonos_client.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include "../utils/psram_json.h"

using json = nlohmann::json;
static const std::string _tag = "sonos";

void SonosClient::init(Config cfg)
{
    _cfg     = cfg;
    _running = true;
    _ensure_poll_thread();
}

void SonosClient::destroy()
{
    _running = false;
    if (_poll_exit.valid()) _poll_exit.wait();
    _poll_thread_up = false;
    _join_workers();
}

// 与 HaClient::_ensure_poll_thread 同款：运行期线程禁止裸 std::thread
// （低内部 RAM 时 pthread_create 失败 → 未捕获异常 → abort，2026-11-07 实锤）。
bool SonosClient::_ensure_poll_thread()
{
    if (_poll_thread_up.exchange(true)) return true;

    // std::function 要求可拷贝，move-only 的 promise 用 shared_ptr 装载
    auto exit_promise = std::make_shared<std::promise<void>>();
    _poll_exit = exit_promise->get_future();
    bool ok = GetHAL()->tryRunDetached([this, p = exit_promise]() {
        _poll_loop();
        p->set_value();
    }, 16 * 1024);
    if (!ok) {
        _poll_thread_up = false;
        mclog::tagWarn(_tag, "poll thread spawn failed (low internal RAM)");
        return false;
    }
    return true;
}

SonosClient::State SonosClient::getState() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _state;
}

void SonosClient::requestRefresh()
{
    _refresh_requested = true;
    _ensure_poll_thread();  // 自愈：init 时低内存 spawn 失败则借机重试
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

    // Spawn through the HAL so a failed thread creation degrades gracefully
    // instead of aborting the device (low internal RAM, 2026-11-07).
    _active_workers.fetch_add(1, std::memory_order_relaxed);
    bool ok = GetHAL()->tryRunDetached([this, fn = std::move(fn)]() {
        fn();
        if (_active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(_worker_mutex);
            _worker_cv.notify_all();
        }
    });
    if (!ok) {
        mclog::tagWarn(_tag, "worker spawn failed (low internal RAM), skipping");
        _active_workers.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void SonosClient::_join_workers()
{
    std::unique_lock<std::mutex> lock(_worker_mutex);
    _worker_cv.wait_for(lock, std::chrono::seconds(5), [this]() {
        return _active_workers.load(std::memory_order_acquire) == 0;
    });
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
        auto j = PsramJson::parse(resp.body);
        State ns;
        ns.ok         = j.value("ok", false);
        ns.raw_state  = j.value("state", std::string());
        ns.volume     = j.value("volume", 0);
        ns.muted      = j.value("muted", false);
        ns.is_tv      = j.value("is_tv", false);
        ns.artist     = j.value("artist", std::string());
        ns.title      = j.value("title", std::string());
        ns.album      = j.value("album", std::string());
        if (GetHAL()) ns.last_update_ms = (uint32_t)GetHAL()->millis();

        std::lock_guard<std::mutex> lock(_mutex);
        _state = std::move(ns);
    } catch (const std::exception& ex) {
        mclog::tagWarn(_tag, "parse error: {}", ex.what());
        std::lock_guard<std::mutex> lock(_mutex);
        _state.ok = false;
    }
}
