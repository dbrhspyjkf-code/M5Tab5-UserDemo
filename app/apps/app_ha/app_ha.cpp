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

// std::stoi throws on non-numeric input; an uncaught throw on this device
// terminates → reboots. HA can return "unavailable"/"unknown"/null for an
// attribute at any time (entity offline, HA restarting), so every parse of a
// HA value goes through this guard instead of std::stoi directly.
static int _safe_stoi(const std::string& s, int def = 0)
{
    if (s.empty()) return def;
    try {
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

static double _safe_stod(const std::string& s, double def = 0.0)
{
    if (s.empty()) return def;
    try {
        return std::stod(s);
    } catch (...) {
        return def;
    }
}

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
static std::string ha_url()    { return "http://" + ha_host()  + ":8123"; }

// ─── Entity config ────────────────────────────────────────────────────────────
// HA long-lived token — shared with app_home via ha_weather.h (single source).
static const char* HA_TOKEN = ha_weather::TOKEN;
// 客厅电视机（2026-06 HA 重新集成，实体名变更）。
//   播放控制（音量/静音/音源）走 media_player，开关按钮映射到"是否为音箱模式"开关。
static const char* TV_EID        = "media_player.xiaomi_esprh1_0bc4_play_control";
static const char* TV_SWITCH_EID = "switch.xiaomi_esprh1_0bc4_is_on";
// Sonos 客厅音响 — 直连 HA 的 media_player 实体（不再经 hermes :8900 桥）。
static const char* SONOS_EID = "media_player.ke_ting_ke_ting";

// 家电 tab
static const char* VACUUM_EID        = "vacuum.yun_jing_xiao_yao_002_max_cx7_vacuum";
static const char* VAC_CHILD_LOCK_EID = "switch.yun_jing_xiao_yao_002_max_cx7_child_lock";
// 米家把剩余时间拆成 HH/MM 两个 sensor，HH 在 <1h 时返回 "0" 字符串 → 不能用。
// MM 是单一真值；判断"是否在跑"用 binary_sensor 的 laundrycyclestatus。
// 主显示用 cyclephase（漂洗/洗涤/脱水/烘干等阶段，比 runningmode 启动/暂停 更细）。
static const char* WASHER_RUNNING_EID = "binary_sensor.04e2297c8113_laundrycyclestatus"; // 程序是否在跑
static const char* WASHER_PHASE_EID   = "sensor.04e2297c8113_cyclephase";                // 洗涤阶段
static const char* WASHER_REMAIN_EID  = "sensor.04e2297c8113_remainingtimemm";            // 剩余分钟
static const char* WASHER_PROG_EID    = "select.04e2297c8113_laundrycycle";               // 洗衣程序
static const char* WASHER_STATE_EID   = "select.04e2297c8113_runningmode";                // 启动/暂停

// 小米人脸识别智能门锁 X — 三个事件 sensor（state = ISO 时间，unknown 表示未触发）
static const char* LOCK_OPEN_EID = "event.lumi_cn_1025181916_bmcn05_lock_opened_e_2_1"; // 开锁
static const char* LOCK_LOCK_EID = "event.lumi_cn_1025181916_bmcn05_lock_locked_e_2_2"; // 上锁
static const char* LOCK_BACK_EID = "event.lumi_cn_1025181916_bmcn05_back_locking_e_2_3"; // 反锁
static const char* LOCK_BAT_EID  = "sensor.lumi_cn_1025181916_bmcn05_battery_level_p_4_1";

// (was: Lights entity_id → display label table LIGHTS[]. Removed when the fish-
// tank light moved into the fish-tank card; the table itself had no remaining
// consumers — onRunning() builds the lighting tab inline.)

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
        cfg.token = HA_TOKEN;
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
        } else if (action == "toggle") {
            _ha->toggle(entity_id);
        } else if (action == "set_percentage") {
            _ha->setFanPercentage(entity_id, _safe_stoi(value, 0));
        } else if (action == "oscillate") {
            _ha->setFanOscillation(entity_id, value == "true");
        } else if (action == "set_preset_mode") {
            _ha->setFanPresetMode(entity_id, value);
        } else if (entity_id == TV_EID) {
            if (action == "tv_power") {
                // Power button toggles the "是否为音箱模式" switch.
                bool on = (_ha->getEntityState(TV_SWITCH_EID) == "on");
                _ha->callService("switch", on ? "turn_off" : "turn_on", TV_SWITCH_EID);
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
        } else if (entity_id == "sonos") {
            // Sonos transport / volume controls — direct HA media_player calls
            // (faster than the old hermes :8900 bridge round-trip).
            //   sonos_play | sonos_pause | sonos_prev | sonos_next |
            //   sonos_vol_up | sonos_vol_down | sonos_mute | sonos_tv |
            //   sonos_refresh
            if (action == "sonos_play") {
                _ha->callService("media_player", "media_play", SONOS_EID);
            } else if (action == "sonos_pause") {
                _ha->callService("media_player", "media_pause", SONOS_EID);
            } else if (action == "sonos_prev") {
                _ha->callService("media_player", "media_previous_track", SONOS_EID);
            } else if (action == "sonos_next") {
                _ha->callService("media_player", "media_next_track", SONOS_EID);
            } else if (action == "sonos_vol_up" || action == "sonos_vol_down") {
                double cur = _safe_stod(_ha->getEntityAttr(SONOS_EID, "volume_level", "0"), 0.0);
                double nv  = (action == "sonos_vol_up") ? cur + 0.05 : cur - 0.05;
                if (nv < 0.0) nv = 0.0;
                if (nv > 1.0) nv = 1.0;
                char buf[48];
                snprintf(buf, sizeof(buf), "\"volume_level\":%.2f", nv);
                _ha->callService("media_player", "volume_set", SONOS_EID, buf);
            } else if (action == "sonos_mute") {
                bool muted = (_ha->getEntityAttr(SONOS_EID, "is_volume_muted", "false") == "true");
                _ha->callService("media_player", "volume_mute", SONOS_EID,
                    std::string("\"is_volume_muted\":") + (muted ? "false" : "true"));
            } else if (action == "sonos_tv") {
                _ha->callService("media_player", "select_source", SONOS_EID, "\"source\":\"TV\"");
            } else if (action == "sonos_refresh") {
                if (_ha) _ha->requestRefresh();
            } else {
                mclog::tagWarn(_tag, "unknown sonos action: {}", action);
            }
        } else if (entity_id == VACUUM_EID) {
            // 扫地机器人：开始/暂停/回充/定位
            if      (action == "vac_start")  _ha->callService("vacuum", "start", VACUUM_EID);
            else if (action == "vac_pause")  _ha->callService("vacuum", "pause", VACUUM_EID);
            else if (action == "vac_dock")   _ha->callService("vacuum", "return_to_base", VACUUM_EID);
            else if (action == "vac_locate") _ha->callService("vacuum", "locate", VACUUM_EID);
            else if (action == "vac_child_lock")
                _ha->callService("switch", "toggle", VAC_CHILD_LOCK_EID);
            else mclog::tagWarn(_tag, "unknown vacuum action: {}", action);
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
    c.is_offline = (state == "unavailable" || state == "unknown");
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
    c.percentage  = _safe_stoi(pct, 0);
    c.oscillating = (ha.getEntityAttr(eid, "oscillating", "false") == "true");
    c.preset_mode = ha.getEntityAttr(eid, "preset_mode", "normal");
    return c;
}

static ha_view::DeviceCard _make_lock_card(HaClient& ha)
{
    ha_view::DeviceCard c;
    c.entity_id = LOCK_OPEN_EID;
    c.label     = "智能门锁";
    c.is_lock   = true;

    // Battery from dedicated sensor
    c.battery = ha.getEntityState(LOCK_BAT_EID);

    // Three event entities: open / lock (上锁) / back (反锁). All carry ISO
    // timestamps in their state ("unknown" means the event hasn't fired yet).
    // Use the newest of the three to decide state + update time — otherwise a
    // back-lock from inside is missed and the card sticks on "已开锁".
    std::string open_t = ha.getEntityState(LOCK_OPEN_EID);
    std::string lock_t = ha.getEntityState(LOCK_LOCK_EID);
    std::string back_t = ha.getEntityState(LOCK_BACK_EID);
    auto is_ts = [](const std::string& s) {
        return !s.empty() && s != "unknown" && s != "unavailable";
    };

    // Newest of the three
    std::string latest;
    if (is_ts(open_t)) latest = open_t;
    if (is_ts(lock_t) && (latest.empty() || lock_t > latest)) latest = lock_t;
    if (is_ts(back_t) && (latest.empty() || back_t > latest)) latest = back_t;

    // State: locked unless the newest event is "open"
    bool open_is_newest =
        is_ts(open_t)
        && (!is_ts(lock_t) || open_t > lock_t)
        && (!is_ts(back_t) || open_t > back_t);
    c.is_on = !open_is_newest;

    // Update time = newest event's ISO (view layer converts UTC→local)
    if (is_ts(latest)) c.value2 = latest;

    // Last open event: time + operation method (for the row-3 history)
    c.value = open_t;
    std::string op = ha.getEntityAttr(LOCK_OPEN_EID, "操作方式", "");
    if (!op.empty()) {
        int opcode = _safe_stoi(op, 0);
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
    // Power on/off reflects the "是否为音箱模式" switch (per user mapping).
    c.is_on        = (ha.getEntityState(TV_SWITCH_EID) == "on");
    // Source / volume / mute come from the play-control media_player.
    c.value        = ha.getEntityAttr(TV_EID, "source", "");
    c.percentage   = _parse_tv_volume_percent(ha.getEntityAttr(TV_EID, "volume_level", "0"));
    c.muted        = (ha.getEntityAttr(TV_EID, "is_volume_muted", "false") == "true");
    // 正在播放：米家电视用 app_name（如 "CIBN酷喵"）；空闲时为空
    std::string app_name = ha.getEntityAttr(TV_EID, "app_name", "");
    if (!app_name.empty() && app_name != "unknown" && app_name != "unavailable") {
        c.value2 = app_name;
    }
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
    living.push_back(_make_card("switch.xiaomi_cn_946012639_w1_on_p_2_1", "厨房灯",
        _ha->getEntityState("switch.xiaomi_cn_946012639_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.xiaomi_cn_945954255_w1_on_p_2_1", "卫生间灯",
        _ha->getEntityState("switch.xiaomi_cn_945954255_w1_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.zimi_cn_1067292111_dhkg02_on_p_2_1", "主人房大灯",
        _ha->getEntityState("switch.zimi_cn_1067292111_dhkg02_on_p_2_1"), "lightbulb"));
    living.push_back(_make_card("switch.zimi_cn_1067292111_dhkg02_on_p_3_1", "主人房小灯",
        _ha->getEntityState("switch.zimi_cn_1067292111_dhkg02_on_p_3_1"), "lightbulb"));

    // ── Tab: 设备 ───────────────────────────────────────────────────────────
    // 设备 tab 单页：sensors grid（含拓竹和联想并排）+ 鱼缸 | 门锁。
    // 鱼缸 + 门锁在 appliance vector 里也用于家电 tab（共用数据）。
    // 落地扇已在 家电 tab。
    std::vector<ha_view::DeviceCard> kitchen;  // empty — 设备 tab 改从 sensors + appliance 读

    // ── Tab: 家电 (落地扇 / 鱼缸 / 门锁 / 扫地机 / 洗衣机) ─────────────────────────────────
    std::vector<ha_view::DeviceCard> appliance;
    // 落地扇（独占顶部一行，高 FAN_H=430）— 从 设备 tab 移过来
    appliance.push_back(_make_fan_card("fan.dmaker_cn_740412216_p5c_s_2_fan", "落地扇", *_ha));
    // 鱼缸综合卡片（电源 + 水泵 + 灯 + 水温 + 滤芯）。
    // 灯光按钮在 view.cpp 的鱼缸卡片内（第三个按钮），灯光 tab 不再列它。
    {
        ha_view::DeviceCard fc;
        fc.entity_id     = "switch.xiaomi_cn_931286672_m200_on_p_2_1";
        fc.label         = "鱼缸";
        fc.is_fishtank   = true;
        fc.is_on         = (_ha->getEntityState("switch.xiaomi_cn_931286672_m200_on_p_2_1") == "on");
        fc.pump_on       = (_ha->getEntityState("switch.xiaomi_cn_931286672_m200_water_pump_p_2_2") == "on");
        fc.fish_light_on = (_ha->getEntityState("light.xiaomi_cn_931286672_m200_s_3_light") == "on");
        fc.water_temp    = _ha->getEntityState("sensor.xiaomi_cn_931286672_m200_temperature_p_2_6");
        fc.filter_life   = _ha->getEntityState("sensor.xiaomi_cn_931286672_m200_filter_life_level_p_6_1");
        appliance.push_back(std::move(fc));
    }
    // 智能门锁（开锁记录每 60 s 拉一次）
    if (_last_lock_history_ms == 0 || now - _last_lock_history_ms > 60000) {
        _last_lock_history_ms = now;
        _fetch_lock_history_async();
    }
    {
        auto lock_card = _make_lock_card(*_ha);
        {
            std::lock_guard<std::mutex> lk(_lock_records_mutex);
            if (_lock_records.size() >= 1) {
                lock_card.value     = _lock_records[0].display_time;
                lock_card.lock_user = _lock_records[0].description;
            }
            if (_lock_records.size() >= 2) {
                lock_card.lock_event2 = _lock_records[1].display_time
                                      + "  " + _lock_records[1].description;
            }
        }
        appliance.push_back(std::move(lock_card));
    }
    // 扫地机器人（云鲸逍遥002 Max）
    {
        ha_view::DeviceCard vc;
        vc.entity_id = VACUUM_EID;
        vc.label     = "云鲸逍遥002 Max";
        vc.is_vacuum = true;
        std::string vs = _ha->getEntityState(VACUUM_EID);
        vc.is_on = (vs == "cleaning");
        static const std::pair<const char*, const char*> VAC_ST[] = {
            {"docked", "在充电座"}, {"cleaning", "清扫中"}, {"paused", "已暂停"},
            {"returning", "返回充电"}, {"idle", "空闲"}, {"error", "故障"},
        };
        vc.value = vs;
        for (auto& kv : VAC_ST) if (vs == kv.first) { vc.value = kv.second; break; }
        if (vs.empty() || vs == "unavailable" || vs == "unknown") vc.value = "未连接";

        // 充电状态（云鲸集成用 sensor.charing 暴露 enum state）
        std::string chg = _ha->getEntityState("sensor.yun_jing_xiao_yao_002_max_cx7_charging");
        if (!chg.empty() && chg != "unknown" && chg != "unavailable") {
            if      (chg == "charging")      vc.charge_status = "充电中";
            else if (chg == "fully_charged") vc.charge_status = "已充满";
            else if (chg == "not_charging")  vc.charge_status = "未充电";
            else                              vc.charge_status = chg;
        }
        vc.vac_battery_pct = _safe_stoi(_ha->getEntityState("sensor.yun_jing_xiao_yao_002_max_cx7_battery"), -1);
        vc.child_lock_on  = (_ha->getEntityState(VAC_CHILD_LOCK_EID) == "on");
        appliance.push_back(std::move(vc));
    }
    // 洗衣机（只读状态）
    // HA 米家集成剩余时间是 HH+MM 双 sensor，HH 在 <1h 时返回 "0" → 改用 MM。
    // 判断"在跑"看 laundrycyclestatus (binary)，避免待机/暂停时误显示"剩余 0 分钟"。
    {
        ha_view::DeviceCard wc;
        wc.entity_id = WASHER_RUNNING_EID;
        wc.label     = "洗衣机";
        wc.is_washer = true;
        std::string running = _ha->getEntityState(WASHER_RUNNING_EID);
        std::string mode    = _ha->getEntityState(WASHER_STATE_EID);
        wc.is_offline = (running.empty() || running == "unavailable" || running == "unknown")
                     && (mode.empty()    || mode    == "unavailable" || mode    == "unknown");
        if (!wc.is_offline) {
            std::string phase = _ha->getEntityState(WASHER_PHASE_EID);
            std::string prog  = _ha->getEntityState(WASHER_PROG_EID);
            std::string mm    = _ha->getEntityState(WASHER_REMAIN_EID);

            // 主显示：在跑 → 洗涤阶段（漂洗/洗涤/脱水/烘干等）；暂停 → "已暂停"；待机 → "待机"
            bool is_running = (running == "on");
            bool is_paused  = (mode    == "暂停");
            if (is_paused)               wc.value = "已暂停";
            else if (is_running) {
                wc.value = (phase.empty() || phase == "无" || phase == "unavailable" || phase == "unknown")
                           ? std::string("运行中") : phase;
            } else                       wc.value = "待机";

            // 副标题：洗衣程序名 + （仅在跑且分钟>0 时）剩余 N 分钟
            std::string line;
            if (!prog.empty() && prog != "unavailable" && prog != "unknown") line = prog;
            int minutes = 0;
            try { minutes = std::stoi(mm); } catch (...) {}
            if (is_running && minutes > 0) {
                if (!line.empty()) line += "  ";
                line += "剩余 " + std::to_string(minutes) + " 分钟";
            }
            wc.value2 = line;
        }
        appliance.push_back(std::move(wc));
    }

    // ── Tab: 影音 (Sonos + 电视) ──────────────────────────────────────────────
    std::vector<ha_view::DeviceCard> media;

    // Sonos card (always first on media tab) — read straight from HA state.
    {
        std::string st     = _ha->getEntityState(SONOS_EID);
        std::string source = _ha->getEntityAttr(SONOS_EID, "source", "");
        std::string cid    = _ha->getEntityAttr(SONOS_EID, "media_content_id", "");
        bool is_tv = (source == "TV") ||
                     cid.find("htastream") != std::string::npos ||
                     cid.find("spdif") != std::string::npos ||
                     cid.find("linein") != std::string::npos;
        double vol = _safe_stod(_ha->getEntityAttr(SONOS_EID, "volume_level", "0"), 0.0);

        ha_view::DeviceCard sc;
        sc.entity_id    = "sonos";
        sc.label        = "SONOS 客厅";
        sc.is_sonos     = true;
        sc.is_on        = (st == "playing");
        sc.percentage   = (int)(vol * 100 + 0.5);
        sc.muted        = (_ha->getEntityAttr(SONOS_EID, "is_volume_muted", "false") == "true");
        sc.is_tv        = is_tv;
        sc.value2       = _ha->getEntityAttr(SONOS_EID, "media_artist", "");
        sc.battery      = _ha->getEntityAttr(SONOS_EID, "media_title", "");      // reuse: card title
        sc.lock_user    = _ha->getEntityAttr(SONOS_EID, "media_album_name", ""); // reuse: card album subtitle
        if      (st == "playing")      sc.sonos_state = "播放中";
        else if (st == "paused")       sc.sonos_state = "已暂停";
        else if (st == "idle")         sc.sonos_state = "已停止";
        else if (st == "buffering")    sc.sonos_state = "切换中";
        else if (st.empty() || st == "unavailable" || st == "unknown")
                                       sc.sonos_state = "未连接";
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

    // 插排（可控开关，放在燃气探测右边）
    sensors.push_back(_make_card("switch.zimi_cn_62879818_v2_on_p_2_1", "插排",
        _ha->getEntityState("switch.zimi_cn_62879818_v2_on_p_2_1"), LV_SYMBOL_POWER));

    // 拓竹 3D 打印机 X1C（详细卡片）
    {
        const char* P = "sensor.x1c_00m09d561500470_";
        ha_view::DeviceCard c;
        c.entity_id  = "sensor.x1c_00m09d561500470_print_status";
        c.label      = "拓竹3D打印机-X1CC";
        c.is_printer = true;

        bool online = (_ha->getEntityState("binary_sensor.x1c_00m09d561500470_online") == "on");
        std::string status = _ha->getEntityState(std::string(P) + "print_status");
        c.is_offline = !online || status.empty() || status == "unavailable" || status == "unknown";

        // 状态英文 → 中文
        static const std::pair<const char*, const char*> ST[] = {
            {"finish", "完成"}, {"running", "打印中"}, {"printing", "打印中"},
            {"prepare", "准备中"}, {"slicing", "切片中"}, {"pause", "已暂停"},
            {"paused", "已暂停"}, {"idle", "空闲"}, {"failed", "失败"}, {"offline", "离线"},
        };
        c.value = status;
        for (auto& kv : ST) if (status == kv.first) { c.value = kv.second; break; }

        c.percentage = _safe_stoi(_ha->getEntityState(std::string(P) + "print_progress"), 0);

        // 多行详情：喷嘴/热床/打印仓温度，剩余时间，层数
        std::string nozzle  = _ha->getEntityState(std::string(P) + "nozzle_temperature");
        std::string bed     = _ha->getEntityState(std::string(P) + "bed_temperature");
        std::string chamber = _ha->getEntityState(std::string(P) + "chamber_temperature");
        std::string remain  = _ha->getEntityState(std::string(P) + "remaining_time");
        std::string cur_l   = _ha->getEntityState(std::string(P) + "current_layer");
        std::string tot_l   = _ha->getEntityState(std::string(P) + "total_layer_count");
        auto na = [](const std::string& s){ return s.empty() ? "--" : s; };
        c.value2 = "喷嘴 " + na(nozzle) + "°C   热床 " + na(bed) + "°C   打印仓 " + na(chamber) + "°C\n"
                 + "剩余 " + na(remain) + "h   层 " + na(cur_l) + "/" + na(tot_l);

        sensors.push_back(std::move(c));
    }

    // 联想 L100DW 打印机（紧跟拓竹，设备 tab 渲染时两者并排，联想在拓竹右边）
    {
        ha_view::DeviceCard lp;
        lp.entity_id         = "sensor.lenovo_l100dw";
        lp.label             = "Lenovo L100DW";
        lp.is_lenovo_printer = true;
        std::string lpst     = _ha->getEntityState("sensor.lenovo_l100dw");
        lp.is_offline        = (lpst.empty() || lpst == "unavailable" || lpst == "unknown");
        if (!lp.is_offline) {
            static const std::pair<const char*, const char*> LP_ST[] = {
                {"idle", "空闲"}, {"printing", "打印中"}, {"stopped", "已停止"},
            };
            lp.value = lpst;
            for (auto& kv : LP_ST) if (lpst == kv.first) { lp.value = kv.second; break; }
            lp.cartridge_pct = _safe_stoi(_ha->getEntityState("sensor.lenovo_l100dw_black_cartridge"), -1);
        }
        sensors.push_back(std::move(lp));
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

    _view->update(living, kitchen, media, sensors, appliance,
                  weather, battery, _ha->isConnected(), tbuf, date_str);
}

// Map 操作ID → 用户名（从小米家庭App查询对应operationId后填写）
static const struct { uint32_t id; const char* name; } LOCK_USERS[] = {
    // {YOUR_OPERATION_ID, "姓名"},  // 示例：指纹用户
    {0, nullptr}
};

static std::string _lookup_user(uint32_t op_id)
{
    for (auto& u : LOCK_USERS)
        if (u.name && u.id == op_id && u.name[0]) return u.name;
    return "";
}

void AppHA::_fetch_lock_history_async()
{
    _start_worker([this]() {
        auto records = _ha->fetchLockHistory({
            LOCK_OPEN_EID, LOCK_LOCK_EID, LOCK_BACK_EID,
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
