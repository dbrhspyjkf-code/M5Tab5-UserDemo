#pragma once
#include <mooncake.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>
#include "ha_client.h"
#include "sonos_client.h"
#include "view/view.h"

class AppHA : public mooncake::AppAbility {
public:
    AppHA();
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called by AppHome so it can restore its screen before this app destroys its LVGL objects.
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

private:
    std::shared_ptr<HaClient> _ha;
    std::unique_ptr<SonosClient> _sonos;
    std::unique_ptr<ha_view::HaView> _view;
    uint32_t _last_ui_update   = 0;
    uint32_t _last_weather_ms  = 0;
    uint32_t _last_sonos_ms    = 0;

    ha_view::WeatherInfo _weather;
    std::mutex           _weather_mutex;

    void _fetch_weather_async();

    // Cached lock history
    std::vector<HaClient::LockRecord> _lock_records;
    std::mutex _lock_records_mutex;
    uint32_t _last_lock_history_ms = 0;
    void _fetch_lock_history_async();

    // Force a sonos refresh (button callback)
    void _refresh_sonos_async();

    std::atomic<bool> _closing{false};
    std::function<void()> _close_cb;

    // ─── Worker pool (detached + counter) ────────────────────────────────────
    // Previously a std::vector<std::thread> was appended to and only joined in
    // onClose — every switch/fan-adjust leaked one 16 KB pthread stack in
    // internal SRAM until the app closed. After ~50 toggles the device OOM'd.
    // Workers now run detached and bump a counter; onClose waits on a CV.
    std::atomic<int>         _active_workers{0};
    std::mutex               _worker_mutex;
    std::condition_variable  _worker_cv;
    void _start_worker(std::function<void()> fn);
    void _join_workers();
};
