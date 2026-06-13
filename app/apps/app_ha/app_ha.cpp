#include "app_ha.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <thread>
#include <fmt/format.h>

using json = nlohmann::json;

static const std::string _tag = "app-ha";

// ─── Entity config ────────────────────────────────────────────────────────────
static const char* HA_URL   = "http://192.168.1.142:8123";
static const char* HA_TOKEN =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    ".eyJpc3MiOiJhZGY3Y2Q0Y2YxNDg0OTQxYjhhNWE1NjRjM2RmZGZiYSIsImlhdCI6MTc4MTIzND"
    "I2MSwiZXhwIjoyMDk2NTk0MjYxfQ"
    ".wI-2PpOXyGUUDYSeCZleq_AUNJ0uPyyIOHxnkcFbJYk";

// Sonos controller endpoint (matches HomeControl-iOS /api/sonos/state)
static const char* SONOS_URL = "http://192.168.1.142:8900";
static const char* TV_EID = "media_player.xiaomi_cn_629973618_esprh1";

// Lights: entity_id → display label
static const std::pair<const char*, const char*> LIGHTS[] = {
    {"switch.xiaomi_cn_2102538340_w1_on_p_2_1",  "走廊灯"},
    {"switch.xiaomi_cn_2143401838_w2_on_p_2_1",  "吸顶灯"},
    {"switch.xiaomi_cn_2143401838_w2_on_p_3_1",  "筒灯"},
    {"switch.xiaomi_cn_945769368_w1_on_p_2_1",   "书台灯"},
    {"switch.xiaomi_cn_945886502_w1_on_p_2_1",   "餐厅灯"},
    {"switch.xiaomi_cn_946012639_w1_on_p_2_1",   "厨房灯"},
    {"switch.xiaomi_cn_945954255_w1_on_p_2_1",   "卫生间灯"},
    {"light.xiaomi_cn_931286672_m200_s_3_light",  "鱼缸灯"},
};

// Devices: entity_id → display label
static const std::pair<const char*, const char*> DEVICES[] = {
    {"switch.zimi_cn_62879818_v2_on_p_2_1",          "插排"},
    {"switch.xiaomi_cn_931286672_m200_on_p_2_1",      "鱼缸"},
    {"fan.dmaker_cn_740412216_p5c_s_2_fan",            "落地扇"},
    {"media_player.xiaomi_cn_629973618_esprh1",        "电视"},
};

// Sensor pairs: {temp_entity, hum_entity, label}
struct SensorPair { const char* temp; const char* hum; const char* label; };
static const SensorPair SENSORS[] = {
    {"sensor.cleargras_cn_blt_3_qs2k0qto4000_dk1_temperature_p_2_1",
     "sensor.cleargras_cn_blt_3_qs2k0qto4000_dk1_relative_humidity_p_2_2",
     "客厅"},
    {"sensor.cgllc_cn_blt_3_1g2f6mj4k4k01_dove_temperature_p_2_1",
     "sensor.cgllc_cn_blt_3_1g2f6mj4k4k01_dove_relative_humidity_p_2_2",
     "儿子房"},
    {"sensor.cgllc_cn_blt_3_1j5psn66skk00_dove_temperature_p_2_1",
     "sensor.cgllc_cn_blt_3_1j5psn66skk00_dove_relative_humidity_p_2_2",
     "女儿房"},
    {"sensor.xiaomi_cn_931286672_m200_temperature_p_2_6",
     "",
     "鱼缸水温"},
};

// ─── App ──────────────────────────────────────────────────────────────────────
AppHA::AppHA()
{
    setAppInfo().name = "AppHA";
}

void AppHA::onCreate()
{
    _closing = false;
    _ha = std::make_shared<HaClient>();

    HaClient::Config cfg;
    cfg.url   = HA_URL;
    cfg.token = HA_TOKEN;
    _ha->init(cfg);

    _sonos = std::make_unique<SonosClient>();
    SonosClient::Config scfg;
    scfg.baseUrl = SONOS_URL;
    _sonos->init(scfg);
}

void AppHA::onOpen()
{
    _view = std::make_unique<ha_view::HaView>();
    _view->init([this](const std::string& entity_id,
                       const std::string& action,
                       const std::string& value) {
        mclog::tagInfo(_tag, "action: {} {} {}", entity_id, action, value);
        if (entity_id == "app" && action == "home") {
            close();
            return;
        } else if (action == "toggle") {
            _ha->toggle(entity_id);
        } else if (action == "set_percentage") {
            _ha->setFanPercentage(entity_id, std::stoi(value));
        } else if (action == "oscillate") {
            _ha->setFanOscillation(entity_id, value == "true");
        } else if (action == "set_preset_mode") {
            _ha->setFanPresetMode(entity_id, value);
        } else if (entity_id == TV_EID) {
            if (action == "tv_power") {
                std::string st = _ha->getEntityState(entity_id);
                _ha->callService("media_player",
                    (st == "on" || st == "playing" || st == "idle") ? "turn_off" : "turn_on",
                    entity_id);
            } else if (action == "tv_vol_down") {
                _ha->callService("media_player", "volume_down", entity_id);
            } else if (action == "tv_vol_up") {
                _ha->callService("media_player", "volume_up", entity_id);
            } else if (action == "tv_mute") {
                _ha->callService("media_player", "volume_mute", entity_id,
                    "\"is_volume_muted\":" + value);
            } else if (action == "tv_source") {
                _ha->callService("media_player", "select_source", entity_id,
                    "\"source\":\"" + value + "\"");
            }
        } else if (entity_id == "sonos" && _sonos) {
            // Sonos transport / volume / mode controls
            //   sonos_play | sonos_pause | sonos_prev | sonos_next |
            //   sonos_vol_up | sonos_vol_down | sonos_mute | sonos_tv |
            //   sonos_refresh
            static const std::pair<const char*, const char*> SONOS_ACTIONS[] = {
                {"sonos_play",      "play"},
                {"sonos_pause",     "pause"},
                {"sonos_prev",      "prev"},
                {"sonos_next",      "next"},
                {"sonos_vol_up",    "volume_up"},
                {"sonos_vol_down",  "volume_down"},
                {"sonos_mute",      "mute"},
                {"sonos_tv",        "tv"},
            };
            bool handled = false;
            for (auto& kv : SONOS_ACTIONS) {
                if (action == kv.first) {
                    _sonos->sendCommand(kv.second);
                    handled = true;
                    break;
                }
            }
            if (action == "sonos_refresh") {
                _refresh_sonos_async();
                handled = true;
            }
            if (!handled) {
                mclog::tagWarn(_tag, "unknown sonos action: {}", action);
            }
        }
        _last_ui_update = 0;
    });
}

static std::string _fmt_event_time(const std::string& iso)
{
    if (iso.size() < 16) return "";
    return iso.substr(5, 5) + " " + iso.substr(11, 5);
}

// Helper: build a simple on/off card
static ha_view::DeviceCard _make_card(const char* eid, const char* lbl,
                                       const std::string& state,
                                       const char* icon = "")
{
    ha_view::DeviceCard c;
    c.entity_id = eid;
    c.label     = lbl;
    c.icon      = icon;
    c.is_on     = (state == "on" || state == "playing");
    return c;
}

static ha_view::DeviceCard _make_fan_card(const char* eid, const char* lbl,
                                           HaClient& ha)
{
    ha_view::DeviceCard c;
    c.entity_id  = eid;
    c.label      = lbl;
    c.is_fan     = true;
    std::string st = ha.getEntityState(eid);
    c.is_on      = (st == "on");
    std::string pct = ha.getEntityAttr(eid, "percentage", "0");
    c.percentage  = pct.empty() ? 0 : std::stoi(pct);
    c.oscillating = (ha.getEntityAttr(eid, "oscillating", "false") == "true");
    c.preset_mode = ha.getEntityAttr(eid, "preset_mode", "normal");
    return c;
}

static ha_view::DeviceCard _make_lock_card(HaClient& ha)
{
    static const char* EID_OPEN = "event.lumi_cn_1025181916_bmcn05_lock_opened_e_2_1";
    static const char* EID_LOCK = "event.lumi_cn_1025181916_bmcn05_lock_locked_e_2_2";
    static const char* EID_BAT  = "sensor.lumi_cn_1025181916_bmcn05_battery_level_p_4_1";

    ha_view::DeviceCard c;
    c.entity_id = EID_OPEN;
    c.label     = "智能门锁";
    c.is_lock   = true;

    // Battery from dedicated sensor
    c.battery = ha.getEntityState(EID_BAT);

    // Determine locked/unlocked by comparing last event timestamps
    std::string open_t = ha.getEntityState(EID_OPEN);  // ISO or "unknown"
    std::string lock_t = ha.getEntityState(EID_LOCK);  // ISO or "unknown"
    bool locked = true;
    if (lock_t != "unknown" && open_t != "unknown")
        locked = (lock_t > open_t);
    else if (lock_t == "unknown" && open_t != "unknown")
        locked = false;
    c.is_on = locked;

    // State update time = whichever event is more recent
    if (lock_t != "unknown" && lock_t > open_t)
        c.value2 = lock_t;
    else if (open_t != "unknown")
        c.value2 = open_t;

    // Last open event: time + operation method
    c.value = open_t;  // ISO timestamp → formatted in view
    std::string op = ha.getEntityAttr(EID_OPEN, "操作方式", "");
    if (!op.empty()) {
        int opcode = op.empty() ? 0 : std::stoi(op);
        switch (opcode) {
            case 1: c.lock_user = "钥匙"; break;
            case 2: c.lock_user = "指纹"; break;
            case 3: c.lock_user = "人脸"; break;
            case 4: c.lock_user = "密码"; break;
            case 5: c.lock_user = "App";  break;
            default: c.lock_user = "";    break;
        }
    }
    return c;
}

static ha_view::DeviceCard _make_sensor_card(const SensorPair& sp, HaClient& ha)
{
    ha_view::DeviceCard c;
    c.entity_id = sp.temp;
    c.label     = sp.label;
    c.is_sensor = true;
    std::string tv = ha.getEntityState(sp.temp);
    c.value = tv.empty() ? "--" : tv + " °C";
    if (sp.hum && sp.hum[0]) {
        std::string hv = ha.getEntityState(sp.hum);
        c.value2 = hv.empty() ? "--" : hv + " %";
    }
    return c;
}

static int _parse_tv_volume_percent(const std::string& volume_level)
{
    if (volume_level.empty()) return 0;
    try {
        float v = std::stof(volume_level);
        if (v <= 1.0f) v *= 100.0f;
        if (v < 0.0f) return 0;
        if (v > 100.0f) return 100;
        return (int)(v + 0.5f);
    } catch (...) {
        return 0;
    }
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

static ha_view::DeviceCard _make_tv_card(HaClient& ha)
{
    ha_view::DeviceCard c;
    c.entity_id    = TV_EID;
    c.label        = "电视";
    c.is_tv_player = true;
    std::string st = ha.getEntityState(TV_EID);
    c.is_on        = (st == "on" || st == "playing" || st == "idle");
    c.value        = ha.getEntityAttr(TV_EID, "source", "");
    c.percentage   = _parse_tv_volume_percent(ha.getEntityAttr(TV_EID, "volume_level", "0"));
    c.muted        = (ha.getEntityAttr(TV_EID, "is_volume_muted", "false") == "true");
    return c;
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
    living.push_back(_make_card("light.xiaomi_cn_931286672_m200_s_3_light", "鱼缸灯",
        _ha->getEntityState("light.xiaomi_cn_931286672_m200_s_3_light"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_946012639_w1_on_p_2_1", "厨房灯",
        _ha->getEntityState("switch.xiaomi_cn_946012639_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_945954255_w1_on_p_2_1", "卫生间灯",
        _ha->getEntityState("switch.xiaomi_cn_945954255_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.zimi_cn_1067292111_dhkg02_on_p_2_1", "主人房大灯",
        _ha->getEntityState("switch.zimi_cn_1067292111_dhkg02_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.zimi_cn_1067292111_dhkg02_on_p_3_1", "主人房小灯",
        _ha->getEntityState("switch.zimi_cn_1067292111_dhkg02_on_p_3_1"), "lightbulb"));

    // ── Tab: 设备 ───────────────────────────────────────────────────────────
    std::vector<ha_view::DeviceCard> kitchen;
    // 鱼缸综合卡片（电源+水泵+水温+滤芯）
    {
        ha_view::DeviceCard fc;
        fc.entity_id   = "switch.xiaomi_cn_931286672_m200_on_p_2_1";
        fc.label       = "鱼缸";
        fc.is_fishtank = true;
        fc.is_on       = (_ha->getEntityState("switch.xiaomi_cn_931286672_m200_on_p_2_1") == "on");
        fc.pump_on     = (_ha->getEntityState("switch.xiaomi_cn_931286672_m200_water_pump_p_2_2") == "on");
        fc.water_temp  = _ha->getEntityState("sensor.xiaomi_cn_931286672_m200_temperature_p_2_6");
        fc.filter_life = _ha->getEntityState("sensor.xiaomi_cn_931286672_m200_filter_life_level_p_6_1");
        kitchen.push_back(std::move(fc));
    }
    kitchen.push_back(_make_card("switch.zimi_cn_62879818_v2_on_p_2_1", "插排",
        _ha->getEntityState("switch.zimi_cn_62879818_v2_on_p_2_1"), LV_SYMBOL_POWER));
    // Fetch lock history every 60 s
    if (_last_lock_history_ms == 0 || now - _last_lock_history_ms > 60000) {
        _last_lock_history_ms = now;
        _fetch_lock_history_async();
    }
    {
        auto lock_card = _make_lock_card(*_ha);
        {
            std::lock_guard<std::mutex> lk(_lock_records_mutex);
            if (_lock_records.size() >= 1) {
                // value = pre-formatted "HH:MM 描述" for record 1
                lock_card.value     = _lock_records[0].display_time;
                lock_card.lock_user = _lock_records[0].description;
            }
            if (_lock_records.size() >= 2) {
                lock_card.lock_event2 = _lock_records[1].display_time
                                      + "  " + _lock_records[1].description;
            }
        }
        kitchen.push_back(std::move(lock_card));
    }
    kitchen.push_back(_make_fan_card("fan.dmaker_cn_740412216_p5c_s_2_fan", "落地扇", *_ha));

    // ── Tab: 影音 (Sonos + 电视) ──────────────────────────────────────────────
    std::vector<ha_view::DeviceCard> media;

    // Sonos card (always first on media tab)
    if (_sonos) {
        SonosClient::State ss = _sonos->getState();
        ha_view::DeviceCard sc;
        sc.entity_id    = "sonos";
        sc.label        = "SONOS 客厅";
        sc.is_sonos     = true;
        sc.is_on        = (ss.raw_state == "PLAYING");
        sc.percentage   = ss.volume;
        sc.muted        = ss.muted;
        sc.is_tv        = ss.is_tv;
        sc.value2       = ss.artist;
        sc.battery      = ss.title;     // reuse: card title
        sc.lock_user    = ss.album;     // reuse: card album subtitle
        if      (ss.raw_state == "PLAYING")         sc.sonos_state = "播放中";
        else if (ss.raw_state == "PAUSED_PLAYBACK") sc.sonos_state = "已暂停";
        else if (ss.raw_state == "STOPPED")         sc.sonos_state = "已停止";
        else if (ss.raw_state == "TRANSITIONING")   sc.sonos_state = "切换中";
        else if (!ss.ok)                            sc.sonos_state = "未连接";
        media.push_back(std::move(sc));
    }

    // TV card
    media.push_back(_make_tv_card(*_ha));

    // ── 传感器 ──────────────────────────────────────────────────────────────
    std::vector<ha_view::DeviceCard> sensors;
    sensors.push_back(_make_sensor_card(SENSORS[0], *_ha)); // 客厅温湿度
    sensors.push_back(_make_sensor_card(SENSORS[1], *_ha)); // 儿子房温湿度
    sensors.push_back(_make_sensor_card(SENSORS[2], *_ha)); // 女儿房温湿度
    // 鱼缸水温已集成到鱼缸综合卡片，不重复显示

    // 净水器 TDS
    {
        ha_view::DeviceCard c;
        c.entity_id = "sensor.xiaomi_cn_964013517_lx20_tds_out_p_4_2";
        c.label     = "净水器";
        c.is_sensor = true;
        std::string out = _ha->getEntityState("sensor.xiaomi_cn_964013517_lx20_tds_out_p_4_2");
        std::string in  = _ha->getEntityState("sensor.xiaomi_cn_964013517_lx20_tds_in_p_4_1");
        c.is_text_value = true;
        c.value  = (out.empty() ? "--" : out) + " ppm";
        c.value2 = "水源 " + (in.empty() ? "--" : in) + " ppm";
        sensors.push_back(std::move(c));
    }

    // 燃气探测器
    {
        ha_view::DeviceCard c;
        c.entity_id    = "sensor.yuemee_cn_538109659_56712_status_p_2_3";
        c.label        = "燃气探测";
        c.is_sensor    = true;
        c.is_text_value = true;
        std::string st = _ha->getEntityState("sensor.yuemee_cn_538109659_56712_status_p_2_3");
        c.value = st.empty() ? "--" : st;
        sensors.push_back(std::move(c));
    }

    // 拓竹打印机
    {
        ha_view::DeviceCard c;
        c.entity_id = "sensor.x1c_00m09d561500470_print_status";
        c.label     = "打印机";
        c.is_sensor = true;
        std::string status   = _ha->getEntityState("sensor.x1c_00m09d561500470_print_status");
        std::string progress = _ha->getEntityState("sensor.x1c_00m09d561500470_print_progress");
        c.value  = (progress.empty() ? "--" : progress) + "%";
        c.value2 = status.empty() ? "--" : status;
        sensors.push_back(std::move(c));
    }

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
        _last_weather_ms = now_ms;
        _fetch_weather_async();
    }

    ha_view::WeatherInfo weather;
    {
        std::lock_guard<std::mutex> lock(_weather_mutex);
        weather = _weather;
    }
    ha_view::BatteryInfo battery = _tab5_battery_from_power();

    _view->update(living, kitchen, media, sensors,
                  weather, battery, _ha->isConnected(), tbuf, date_str);
}

// Map 操作ID → 用户名（从小米家庭app对应填写）
static const struct { uint32_t id; const char* name; } LOCK_USERS[] = {
    {2147549194, ""},   // 指纹用户 — 请填写
    {2147876868, ""},   // 人脸用户1 — 请填写
    {2147876869, ""},   // 人脸用户2 — 请填写
    {2147876872, ""},   // 人脸用户3 — 请填写
    {0, nullptr}
};

static std::string _lookup_user(uint32_t op_id)
{
    for (auto& u : LOCK_USERS)
        if (u.name && u.id == op_id && u.name[0]) return u.name;
    return "";
}

void AppHA::_refresh_sonos_async()
{
    // The card refresh button nudges the Sonos poll thread to fetch
    // immediately, regardless of its current interval. The next frame
    // (~500 ms later) will pick up the new state.
    if (_sonos) _sonos->requestRefresh();
}

void AppHA::_fetch_lock_history_async()
{
    _start_worker([this]() {
        auto records = _ha->fetchLockHistory({
            "event.lumi_cn_1025181916_bmcn05_lock_opened_e_2_1",
            "event.lumi_cn_1025181916_bmcn05_lock_locked_e_2_2",
        }, 24, 4);

        if (_closing.load()) return;

        // Inject user names
        for (auto& r : records) {
            std::string user = _lookup_user(r.op_id);
            if (!user.empty())
                r.description = user + r.description;
        }

        std::lock_guard<std::mutex> lk(_lock_records_mutex);
        _lock_records = std::move(records);
    });
}

void AppHA::_fetch_weather_async()
{
    _start_worker([this]() {
        auto resp = GetHAL()->httpGet("http://192.168.1.142:8766/weather?city=Guangzhou");
        if (!resp.ok) {
            mclog::tagWarn(_tag, "weather fetch failed: {}", resp.status);
            return;
        }
        try {
            auto j = json::parse(resp.body);
            if (!j.value("ok", false)) return;

            ha_view::WeatherInfo w;
            std::string tc = j.value("temp_c", "--");
            w.temp        = tc + "°C";
            w.humidity    = j.value("humidity", "--") + "%";
            w.description = j.value("condition", "");
            std::string fl = j.value("feels_like_c", "");
            if (!fl.empty()) w.description += "  体感" + fl + "°C";

            if (_closing.load()) return;

            std::lock_guard<std::mutex> lock(_weather_mutex);
            _weather = w;
        } catch (...) {}
    });
}

void AppHA::_start_worker(std::function<void()> fn)
{
    if (_closing.load()) return;
    _active_workers.fetch_add(1, std::memory_order_relaxed);
    std::thread([this, fn = std::move(fn)]() {
        fn();
        // Decrement and notify so onClose can wait cleanly.
        if (_active_workers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(_worker_mutex);
            _worker_cv.notify_all();
        }
    }).detach();
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
    if (_sonos) _sonos->destroy();
    _join_workers();
    // Restore launcher screen BEFORE destroying LVGL objects so there is always
    // an active screen — deleting the current active screen would leave LVGL in
    // an undefined state.
    if (_close_cb) _close_cb();
    _view.reset();
}
