#include "ha_client.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;
static const std::string _tag = "ha-client";

void HaClient::init(Config cfg)
{
    _cfg     = cfg;
    _running = true;
    _poll_thread = std::thread([this]() { _poll_loop(); });
}

void HaClient::destroy()
{
    _running = false;
    if (_poll_thread.joinable()) _poll_thread.join();
}

std::vector<HaEntity> HaClient::getStates() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _states;
}

std::string HaClient::getEntityState(const std::string& entity_id) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& e : _states) {
        if (e.entity_id == entity_id) return e.state;
    }
    return "";
}

std::string HaClient::getEntityAttr(const std::string& entity_id,
                                     const std::string& attr,
                                     const std::string& def) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& e : _states) {
        if (e.entity_id == entity_id) return e.attr(attr, def);
    }
    return def;
}

void HaClient::setFanPercentage(const std::string& entity_id, int percentage)
{
    // Clamp to valid range
    if (percentage < 0)   percentage = 0;
    if (percentage > 100) percentage = 100;

    // Optimistic update
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& e : _states) {
            if (e.entity_id == entity_id) {
                e.attributes["percentage"] = std::to_string(percentage);
                e.state = percentage > 0 ? "on" : "off";
                break;
            }
        }
    }

    std::string url   = _cfg.url + "/api/services/fan/set_percentage";
    std::string body  = "{\"entity_id\":\"" + entity_id + "\","
                        "\"percentage\":" + std::to_string(percentage) + "}";
    std::string token = _cfg.token;
    _start_worker([url, body, token]() {
        auto resp = GetHAL()->httpPost(url, body,
            {{"Authorization", "Bearer " + token},
             {"Content-Type",  "application/json"}});
        if (!resp.ok)
            mclog::tagWarn(_tag, "setFanPercentage failed: {}", resp.status);
    });
    _refresh_requested = true;
}

void HaClient::setFanPresetMode(const std::string& entity_id, const std::string& mode)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& e : _states) {
            if (e.entity_id == entity_id) {
                e.attributes["preset_mode"] = mode;
                break;
            }
        }
    }
    std::string url   = _cfg.url + "/api/services/fan/set_preset_mode";
    std::string body  = "{\"entity_id\":\"" + entity_id + "\","
                        "\"preset_mode\":\"" + mode + "\"}";
    std::string token = _cfg.token;
    _start_worker([url, body, token]() {
        auto resp = GetHAL()->httpPost(url, body,
            {{"Authorization", "Bearer " + token},
             {"Content-Type",  "application/json"}});
        if (!resp.ok)
            mclog::tagWarn(_tag, "setFanPresetMode failed: {}", resp.status);
    });
    _refresh_requested = true;
}

void HaClient::setFanOscillation(const std::string& entity_id, bool oscillating)
{
    // Optimistic update
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& e : _states) {
            if (e.entity_id == entity_id) {
                e.attributes["oscillating"] = oscillating ? "true" : "false";
                break;
            }
        }
    }

    std::string url   = _cfg.url + "/api/services/fan/oscillate";
    std::string body  = "{\"entity_id\":\"" + entity_id + "\","
                        "\"oscillating\":" + (oscillating ? "true" : "false") + "}";
    std::string token = _cfg.token;
    _start_worker([url, body, token]() {
        auto resp = GetHAL()->httpPost(url, body,
            {{"Authorization", "Bearer " + token},
             {"Content-Type",  "application/json"}});
        if (!resp.ok)
            mclog::tagWarn(_tag, "setFanOscillation failed: {}", resp.status);
    });
    _refresh_requested = true;
}

void HaClient::requestRefresh()
{
    _refresh_requested = true;
}

void HaClient::toggle(const std::string& entity_id)
{
    std::string current = getEntityState(entity_id);
    std::string domain  = entity_id.substr(0, entity_id.find('.'));
    std::string service = (current == "on" || current == "playing") ? "turn_off" : "turn_on";
    callService(domain, service, entity_id);
}

void HaClient::callService(const std::string& domain, const std::string& service,
                            const std::string& entity_id, const std::string& extra_json)
{
    // Optimistic update
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& e : _states) {
            if (e.entity_id == entity_id) {
                if (service == "turn_on")  e.state = "on";
                if (service == "turn_off") e.state = "off";
                break;
            }
        }
    }

    // Fire async HTTP
    std::string url  = _cfg.url + "/api/services/" + domain + "/" + service;
    std::string body = "{\"entity_id\":\"" + entity_id + "\"";
    if (!extra_json.empty()) body += "," + extra_json;
    body += "}";

    std::string token = _cfg.token;
    _start_worker([url, body, token]() {
        auto resp = GetHAL()->httpPost(url, body,
            {{"Authorization", "Bearer " + token},
             {"Content-Type", "application/json"}});
        if (!resp.ok) {
            mclog::tagWarn(_tag, "service call failed: {} {}", url, resp.status);
        }
    });

    // Schedule a refresh after 1s to confirm new state
    _refresh_requested = true;
}

void HaClient::_start_worker(std::function<void()> fn)
{
    // Workers capture url/body/token by value (no shared state), and only
    // call GetHAL() which is a singleton that outlives this client. Detach is
    // safe and avoids accumulating joinable threads.
    if (!_running.load()) return;
    std::thread(std::move(fn)).detach();
}

void HaClient::_poll_loop()
{
    // Initial fetch
    _fetch_states();

    auto last = std::chrono::steady_clock::now();
    while (_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (_refresh_requested.load()) {
            _refresh_requested = false;
            _fetch_states();
            last = std::chrono::steady_clock::now();
            continue;
        }

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        if (elapsed >= _cfg.pollIntervalMs) {
            _fetch_states();
            last = std::chrono::steady_clock::now();
        }
    }
}

void HaClient::_fetch_states()
{
    if (!GetHAL()) return;
    _fetching = true;

    auto resp = GetHAL()->httpGet(
        _cfg.url + "/api/states",
        {{"Authorization", "Bearer " + _cfg.token}});

    _fetching = false;

    if (!resp.ok) {
        mclog::tagWarn(_tag, "fetchStates failed: {}", resp.status);
        _connected = false;
        return;
    }

    try {
        auto arr = json::parse(resp.body);
        std::vector<HaEntity> entities;
        for (auto& item : arr) {
            HaEntity e;
            e.entity_id     = item.value("entity_id", "");
            e.state         = item.value("state", "");
            auto& attrs     = item["attributes"];
            e.friendly_name = attrs.value("friendly_name", "");
            e.unit          = attrs.value("unit_of_measurement", "");
            // Store all scalar attributes as strings
            for (auto& [k, v] : attrs.items()) {
                if (v.is_string())  e.attributes[k] = v.get<std::string>();
                else if (v.is_number_integer()) e.attributes[k] = std::to_string(v.get<int>());
                else if (v.is_number_float())   e.attributes[k] = std::to_string(v.get<double>());
                else if (v.is_boolean())        e.attributes[k] = v.get<bool>() ? "true" : "false";
            }
            // Store last_changed as pseudo-attribute for lock cards
            if (item.contains("last_changed") && item["last_changed"].is_string())
                e.attributes["last_changed"] = item["last_changed"].get<std::string>();
            entities.push_back(std::move(e));
        }
        size_t n;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _states = std::move(entities);
            n = _states.size();
        }
        _connected    = true;
        _last_update_ms = (uint32_t)GetHAL()->millis();
        mclog::tagInfo(_tag, "fetched {} entities", n);
    } catch (const std::exception& ex) {
        mclog::tagWarn(_tag, "parse error: {}", ex.what());
        _connected = false;
    }
}

// Convert ISO UTC timestamp to local HH:MM string (UTC+8, China)
static std::string _utc_to_local_hhmm(const std::string& iso)
{
    if (iso.size() < 19) return "--:--";
    int h = 0, m = 0;
    if (sscanf(iso.c_str() + 11, "%2d:%2d", &h, &m) != 2) return "--:--";
    h = (h + 8) % 24;  // UTC → UTC+8
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return buf;
}

static std::string _decode_op(const json& attrs)
{
    if (!attrs.contains("操作方式")) return "";
    int code = 0;
    auto& v = attrs["操作方式"];
    if (v.is_number_integer()) code = v.get<int>();
    else if (v.is_string()) code = std::stoi(v.get<std::string>());
    switch (code) {
        case 1: return "钥匙";
        case 2: return "指纹";
        case 3: return "人脸";
        case 4: return "密码";
        case 5: return "App";
        default: return "";
    }
}

std::vector<HaClient::LockRecord> HaClient::fetchLockHistory(
    const std::vector<std::string>& entity_ids, int hours, int max) const
{
    if (!GetHAL() || entity_ids.empty()) return {};

    time_t now   = time(nullptr);
    time_t start = now - (time_t)hours * 3600;
    tm*    gmt   = gmtime(&start);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", gmt);

    // Build filter param
    std::string filter;
    for (size_t i = 0; i < entity_ids.size(); i++) {
        if (i) filter += ",";
        filter += entity_ids[i];
    }

    std::string url = _cfg.url + "/api/history/period/" + ts
                    + "?filter_entity_id=" + filter;
    auto resp = GetHAL()->httpGet(url, {{"Authorization", "Bearer " + _cfg.token}});
    if (!resp.ok) return {};

    std::vector<LockRecord> all;
    try {
        auto root = json::parse(resp.body);
        if (!root.is_array()) return {};

        for (auto& entity_arr : root) {
            if (!entity_arr.is_array()) continue;
            for (auto& e : entity_arr) {
                std::string st = e.value("state", "");
                if (st == "unknown" || st == "unavailable" || st.empty()) continue;
                auto& attrs = e["attributes"];
                std::string ev_type = "";
                if (attrs.contains("event_type") && attrs["event_type"].is_string())
                    ev_type = attrs["event_type"].get<std::string>();

                // Build description
                std::string op  = _decode_op(attrs);
                std::string desc;
                if (ev_type == "开锁" || !op.empty()) {
                    desc = op.empty() ? "开锁" : op + "开锁";
                } else {
                    // lock/back_lock events with null event_type → auto-lock
                    std::string fn = attrs.value("friendly_name", "");
                    if (fn.find("上锁") != std::string::npos)
                        desc = "自动上锁";
                    else if (fn.find("反锁") != std::string::npos)
                        desc = "反锁";
                    else
                        desc = "上锁";
                }

                uint32_t op_id = 0;
                if (attrs.contains("操作ID") && attrs["操作ID"].is_number_integer())
                    op_id = (uint32_t)attrs["操作ID"].get<int64_t>();

                LockRecord rec;
                rec.time_iso     = e.value("last_changed", st);
                rec.display_time = _utc_to_local_hhmm(rec.time_iso);
                rec.description  = desc;
                rec.op_id        = op_id;
                all.push_back(rec);
            }
        }
    } catch (...) { return {}; }

    // Sort newest first
    std::sort(all.begin(), all.end(),
              [](const LockRecord& a, const LockRecord& b){
                  return a.time_iso > b.time_iso;
              });
    if ((int)all.size() > max) all.resize(max);
    return all;
}
