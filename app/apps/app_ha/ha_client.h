#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

struct HaEntity {
    std::string entity_id;
    std::string state;
    std::string friendly_name;
    std::string unit;
    std::map<std::string, std::string> attributes;

    std::string domain() const
    {
        auto pos = entity_id.find('.');
        return pos != std::string::npos ? entity_id.substr(0, pos) : entity_id;
    }
    bool isOn() const
    {
        return state == "on" || state == "playing" || state == "heat" || state == "cool"
               || state == "auto";
    }
    std::string attr(const std::string& key, const std::string& def = "") const
    {
        auto it = attributes.find(key);
        return it != attributes.end() ? it->second : def;
    }
};

class HaClient {
public:
    struct Config {
        std::string url            = "http://localhost:8123";
        std::string token;
        int pollIntervalMs         = 12000;
    };

    void init(Config cfg);
    void destroy();

    // Thread-safe read
    std::vector<HaEntity> getStates() const;
    std::string getEntityState(const std::string& entity_id) const;
    std::string getEntityAttr(const std::string& entity_id, const std::string& attr,
                               const std::string& def = "") const;

    // Async service calls (fire-and-forget)
    void toggle(const std::string& entity_id);
    void callService(const std::string& domain, const std::string& service,
                     const std::string& entity_id, const std::string& extra_json = "");
    void setFanPercentage(const std::string& entity_id, int percentage);
    void setFanOscillation(const std::string& entity_id, bool oscillating);
    void setFanPresetMode(const std::string& entity_id, const std::string& mode);

    // Force a refresh now
    void requestRefresh();

    // Sync fetch of lock events from multiple entities, merged, newest first
    struct LockRecord {
        std::string time_iso;      // for sorting
        std::string display_time;  // local "HH:MM"
        std::string description;   // "人脸开锁" / "自动上锁" etc.
        uint32_t    op_id = 0;     // raw 操作ID for name lookup
    };
    std::vector<LockRecord> fetchLockHistory(
        const std::vector<std::string>& entity_ids,
        int hours = 24, int max = 4) const;

    bool isConnected() const { return _connected.load(); }
    bool isFetching() const { return _fetching.load(); }
    uint32_t lastUpdateMs() const { return _last_update_ms.load(); }

private:
    Config _cfg;
    std::vector<HaEntity> _states;
    mutable std::mutex _mutex;
    std::thread _poll_thread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _fetching{false};
    std::atomic<bool> _refresh_requested{false};
    std::atomic<uint32_t> _last_update_ms{0};

    void _start_worker(std::function<void()> fn);
    void _poll_loop();
    void _fetch_states();
};
