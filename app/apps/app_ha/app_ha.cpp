#include "app_ha.h"
#include "ha_weather.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <thread>
#include <fmt/format.h>

using json = nlohmann::json;

static const std::string _tag = "app-ha";

// ─── Server address ───────────────────────────────────────────────────────────
// Home Assistant host (:8123). Everything in this app — lights, switches, fan,
// climate, TV, Sonos AND weather — now talks directly to HA. Configurable
// on-screen via the Settings app (NVS key "ha_host"). The Mac-side Hermes
// services (weather used to be there, Claude bridge still is) live on a separate
// host stored in NVS "svc_host"; this app no longer needs it.
static const char* HA_HOST_DEFAULT = "";  // Configure via Settings app

static std::string ha_host()
{
    return GetHAL()->getConfig("ha_host", HA_HOST_DEFAULT);
}
static std::string ha_url()    { return ha_weather::baseUrl(ha_host()); }

// ─── App ──────────────────────────────────────────────────────────────────────
AppHA::AppHA()
{
    setAppInfo().name = "AppHA";
}

void AppHA::onCreate()
{
    // The HA + Sonos clients are intentionally created in onOpen (and fully
    // released in onClose), NOT here. onCreate runs once at install time and the
    // object would then live forever. HaClient::_states holds ~300 entities
    // whose many small string/map allocations land mostly in internal SRAM;
    // keeping them around after the app closes starved xiaozhi of internal DMA
    // RAM and crashed it (I2S DMA alloc failure → Load access fault) when
    // entering it after HA. Create-on-open / destroy-on-close frees that RAM.
}

void AppHA::onOpen()
{
    _closing = false;

    // (Re)create the HA client each time the app opens. Reading ha_url() here
    // also means a HA address changed in Settings takes effect on next open.
    // Sonos is now controlled through this same HA client (media_player.*),
    // so there's no separate Sonos client anymore.
    if (!_ha) {
        _ha = std::make_shared<HaClient>();
        HaClient::Config cfg;
        cfg.url   = ha_url();
        cfg.token = ha_weather::token();
        _ha->init(cfg);
    }

    _view = std::make_unique<ha_view::HaView>();
    _view->init([this](const std::string& entity_id,
                       const std::string& action,
                       const std::string& value) {
        mclog::tagInfo(_tag, "action: {} {} {}", entity_id, action, value);
        if (entity_id == "app" && action == "home") {
            close();
            return;
        }
        if (action == "toggle") _ha->toggle(entity_id);
        _last_ui_update = 0;
    });
}

static ha_view::DeviceCard _make_card(const char* eid, const char* lbl,
                                       const std::string& state,
                                       const char* icon = "")
{
    ha_view::DeviceCard c;
    c.entity_id = eid;
    c.label     = lbl;
    c.icon      = icon;
    c.is_on     = (state == "on" || state == "playing");
    c.is_offline = (state == "unavailable" || state == "unknown");
    return c;
}

static int _tab5_battery_percent_from_voltage(float voltage)
{
    if (voltage <= 0.0f) return -1;

    float empty = 3.20f;
    float full  = 4.20f;
    if (voltage > 5.0f) {
        empty = 6.40f;
        full  = 8.40f;
    }

    int pct = (int)(((voltage - empty) * 100.0f / (full - empty)) + 0.5f);
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

static ha_view::BatteryInfo _tab5_battery_from_power()
{
    GetHAL()->updatePowerMonitorData();
    ha_view::BatteryInfo battery;
    battery.percentage = _tab5_battery_percent_from_voltage(GetHAL()->powerMonitorData.busVoltage);
    battery.charging   = GetHAL()->powerMonitorData.shuntCurrent > 0.05f;
    return battery;
}

void AppHA::onRunning()
{
    uint32_t now = GetHAL()->millis();
    if (now - _last_ui_update < 500) return;
    _last_ui_update = now;

    // ── Tab: 灯光 ───────────────────────────────────────────────────────────
    std::vector<ha_view::DeviceCard> living;
    living.push_back(_make_card("switch.xiaomi_cn_2143401838_w2_on_p_2_1", "吸顶灯",
        _ha->getEntityState("switch.xiaomi_cn_2143401838_w2_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_2143401838_w2_on_p_3_1", "筒灯",
        _ha->getEntityState("switch.xiaomi_cn_2143401838_w2_on_p_3_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_2102538340_w1_on_p_2_1", "走廊灯",
        _ha->getEntityState("switch.xiaomi_cn_2102538340_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_945886502_w1_on_p_2_1", "餐厅灯",
        _ha->getEntityState("switch.xiaomi_cn_945886502_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_945769368_w1_on_p_2_1", "书台灯",
        _ha->getEntityState("switch.xiaomi_cn_945769368_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_946012639_w1_on_p_2_1", "厨房灯",
        _ha->getEntityState("switch.xiaomi_cn_946012639_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_945954255_w1_on_p_2_1", "卫生间灯",
        _ha->getEntityState("switch.xiaomi_cn_945954255_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.zimi_cn_1067292111_dhkg02_on_p_2_1", "主人房大灯",
        _ha->getEntityState("switch.zimi_cn_1067292111_dhkg02_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.zimi_cn_1067292111_dhkg02_on_p_3_1", "主人房小灯",
        _ha->getEntityState("switch.zimi_cn_1067292111_dhkg02_on_p_3_1"), "lightbulb"));

    // ── Time & date ──────────────────────────────────────────────────────────
    time_t t  = time(nullptr);
    tm*    lt = localtime(&t);
    char tbuf[16], dbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M", lt);
    strftime(dbuf, sizeof(dbuf), "%m月%d日 周", lt);
    static const char* WDAYS[] = {"日","一","二","三","四","五","六"};
    std::string date_str = std::string(dbuf) + WDAYS[lt->tm_wday];

    // ── Weather: fetch every 10 minutes ─────────────────────────────────────
    uint32_t now_ms = GetHAL()->millis();
    if (_last_weather_ms == 0 || now_ms - _last_weather_ms > 600000) {
        // Only advance the timer once HA actually has weather data, so the very
        // first reads (before the initial /api/states poll lands) keep retrying.
        if (_fetch_weather_async()) _last_weather_ms = now_ms;
    }

    ha_view::WeatherInfo weather;
    {
        std::lock_guard<std::mutex> lock(_weather_mutex);
        weather = _weather;
    }
    ha_view::BatteryInfo battery = _tab5_battery_from_power();

    _view->update(living, weather, battery, _ha->isConnected(), tbuf, date_str);
}

bool AppHA::_fetch_weather_async()
{
    // Weather now comes straight from HA's weather entity, which the HaClient
    // already polls as part of /api/states — no separate HTTP request needed.
    if (!_ha) return false;
    std::string st  = _ha->getEntityState(ha_weather::ENTITY);
    if (st.empty() || st == "unavailable" || st == "unknown")
        return false;  // not fetched yet; retry next tick
    std::string tc  = _ha->getEntityAttr(ha_weather::ENTITY, "temperature", "--");
    std::string hum = _ha->getEntityAttr(ha_weather::ENTITY, "humidity", "--");
    // met.no gives temperature like "28.1"; show whole degrees to match the old UI.
    if (auto dot = tc.find('.'); dot != std::string::npos) tc = tc.substr(0, dot);

    ha_view::WeatherInfo w;
    w.condition   = st;
    w.temp        = tc + "°C";
    w.humidity    = hum + "%";
    w.description = ha_weather::condZh(st);

    std::lock_guard<std::mutex> lock(_weather_mutex);
    _weather = w;
    return true;
}

void AppHA::_start_worker(std::function<void()> fn)
{
    if (_closing.load()) return;
    _active_workers.fetch_add(1, std::memory_order_relaxed);

    auto worker_done = [this]() {
        if (_active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(_worker_mutex);
            _worker_cv.notify_all();
        }
    };

    // Spawn through the HAL so a failed thread creation degrades gracefully
    // instead of aborting the device. This matters right after leaving xiaozhi:
    // its freed audio task stacks are still being reclaimed, so internal RAM is
    // momentarily fragmented and pthread_create can fail. std::thread would
    // abort() there (C++ exceptions are disabled); tryRunDetached returns false.
    bool ok = GetHAL()->tryRunDetached([fn = std::move(fn), worker_done]() {
        fn();
        worker_done();
    });
    if (!ok) {
        // Couldn't spawn — undo the counter so onClose's _join_workers() does
        // not wait forever, and skip this fetch (it'll be retried next tick).
        mclog::tagWarn(_tag, "worker spawn failed (low internal RAM), skipping");
        worker_done();
    }
}

void AppHA::_join_workers()
{
    std::unique_lock<std::mutex> lock(_worker_mutex);
    // Wait up to 5s for in-flight HTTP fetches to finish.
    _worker_cv.wait_for(lock, std::chrono::seconds(5), [this]() {
        return _active_workers.load(std::memory_order_acquire) == 0;
    });
}

void AppHA::onClose()
{
    _closing = true;
    if (_ha)    _ha->destroy();
    _join_workers();
    // Restore launcher screen BEFORE destroying LVGL objects so there is always
    // an active screen — deleting the current active screen would leave LVGL in
    // an undefined state.
    if (_close_cb) _close_cb();
    _view.reset();
    // Release the clients so HaClient::_states (~300 entities, mostly internal
    // SRAM) is fully freed while the app is closed. Recreated in onOpen. This is
    // what lets xiaozhi allocate its I2S DMA after the user has been in HA.
    _ha.reset();
}
