#pragma once
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace ha_view {

// ─── Data types ───────────────────────────────────────────────────────────────

struct DeviceCard {
    std::string entity_id;
    std::string label;
    bool is_on          = false;
    bool is_offline     = false;  // HA state unavailable/unknown → can't control
    bool is_sensor      = false;
    bool is_fan         = false;
    int  percentage     = 0;
    bool oscillating    = false;
    std::string preset_mode;
    std::string value;        // sensor primary value
    std::string value2;       // sensor secondary value
    bool is_text_value = false; // use zh font for value (Chinese text)
    std::string icon;    // emoji/symbol label (optional)
    bool is_lock        = false;
    std::string battery;     // lock battery level e.g. "85"
    std::string lock_user;   // most recent open: method
    std::string lock_event2; // second most recent: "方式 MM-DD HH:MM"
    bool is_tv_player   = false;

    // Fishtank combined card
    bool is_fishtank   = false;
    bool pump_on       = false;
    std::string water_temp;   // fish tank water temperature
    std::string filter_life;  // filter remaining life %

    // Sonos card fields (only used when is_sonos = true)
    bool   is_sonos  = false;
    bool   muted     = false;
    bool   is_tv     = false;  // true → show "电视输入" instead of title
    std::string sonos_state;  // "播放中" / "已暂停" / "已停止"

    // Vacuum (扫地机器人): value = state text (Chinese), is_on = cleaning.
    bool is_vacuum = false;
    // Washing machine (洗衣机): value = 运行状态, value2 = 剩余/程序; read-only.
    bool is_washer = false;
    // 3D printer detail (拓竹 X1C): percentage = progress %, value = status
    // (Chinese), value2 = multi-line detail (temps / layer / remaining).
    bool is_printer = false;

    // Equality over every rendered field, so update() can skip rebuilding a tab
    // whose data hasn't changed (avoids destroying/recreating ~30–50 LVGL
    // objects every frame, which fragments PSRAM and opens crash windows).
    bool operator==(const DeviceCard& o) const
    {
        return entity_id == o.entity_id && label == o.label && is_on == o.is_on
            && is_offline == o.is_offline
            && is_sensor == o.is_sensor && is_fan == o.is_fan
            && percentage == o.percentage && oscillating == o.oscillating
            && preset_mode == o.preset_mode && value == o.value
            && value2 == o.value2 && is_text_value == o.is_text_value
            && icon == o.icon && is_lock == o.is_lock && battery == o.battery
            && lock_user == o.lock_user && lock_event2 == o.lock_event2
            && is_tv_player == o.is_tv_player && is_fishtank == o.is_fishtank
            && pump_on == o.pump_on && water_temp == o.water_temp
            && filter_life == o.filter_life && is_sonos == o.is_sonos
            && muted == o.muted && is_tv == o.is_tv
            && sonos_state == o.sonos_state
            && is_vacuum == o.is_vacuum && is_washer == o.is_washer
            && is_printer == o.is_printer;
    }
    bool operator!=(const DeviceCard& o) const { return !(*this == o); }
};

struct WeatherInfo {
    std::string condition;   // "sunny" / "cloudy" / etc.
    std::string temp;        // "32°C"
    std::string humidity;    // "65%"
    std::string description; // "晴" / "多云"
};

struct BatteryInfo {
    int  percentage = -1;
    bool charging   = false;
};

// Tab pages
enum class TabPage { LIVING = 0, KITCHEN_BATH, MEDIA, APPLIANCE };

class HaView {
public:
    ~HaView();

    // action: "toggle" | "set_percentage" | "oscillate" | "set_preset_mode"
    using ActionCb = std::function<void(const std::string& entity_id,
                                        const std::string& action,
                                        const std::string& value)>;

    ActionCb _on_action_fn;  // public for card callbacks

    // Accessible from file-scope LVGL callbacks
    int       _device_tab_page = 0;
    void _set_dot_page(int page);

    void init(ActionCb on_action);
    void _switch_tab(TabPage tab);  // called by tab button callbacks

    // Skip the throttle on the next update() call — used by action callbacks
    // so a tap reflects its optimistic state on the very next frame.
    void requestRebuild() { _force_rebuild = true; }

    // Called every frame by AppHA::onRunning()
    void update(const std::vector<DeviceCard>& living,    // 灯光
                const std::vector<DeviceCard>& kitchen,   // 设备
                const std::vector<DeviceCard>& media,     // 影音
                const std::vector<DeviceCard>& sensors,   // 传感器（所有房间）
                const std::vector<DeviceCard>& appliance, // 家电（鱼缸/门锁/扫地机/洗衣机）
                const WeatherInfo& weather,
                const BatteryInfo& battery,
                bool connected,
                const std::string& time_str,
                const std::string& date_str);

private:
    ActionCb _on_action;
    TabPage  _active_tab = TabPage::LIVING;
    // Last-seen Sonos state, so the card can still render across a single
    // tab re-render gap. Refreshed by update() and used by _rebuild_tab_content.
    DeviceCard _sonos_card;

    // ─── Tab content is the LVGL-heavy part (~30–50 lv_obj_create + delete per
    // rebuild). Rebuilding it on a timer fragmented PSRAM and eventually made
    // lv_obj_create() return NULL, crashing the next lv_obj_set_size. update()
    // now rebuilds only when the active tab's data actually changed (or on an
    // explicit tap), and never while a touch/scroll is live. _last_tab_rebuild_ms
    // doubles as the "have we built at least once" sentinel. The header is cheap
    // (a few label updates) and refreshes at the full onRunning cadence.
    uint32_t _last_tab_rebuild_ms = 0;
    bool     _force_rebuild       = false;
    uint32_t _get_millis() const;

    // ─── Last card data, cached so _switch_tab can rebuild the new tab
    // synchronously without waiting up to 2 s for the next onRunning tick
    // (which would otherwise leave a blank content area after a tab tap).
    std::vector<DeviceCard> _living_cache;
    std::vector<DeviceCard> _kitchen_cache;
    std::vector<DeviceCard> _media_cache;
    std::vector<DeviceCard> _sensors_cache;
    std::vector<DeviceCard> _appliance_cache;

    // Indev for swipe-up-to-exit gesture
    lv_indev_t* _gesture_indev = nullptr;

    // Root screen objects
    lv_obj_t* _scr          = nullptr;
    lv_obj_t* _lbl_time     = nullptr;
    lv_obj_t* _lbl_date     = nullptr;
    lv_obj_t* _lbl_temp     = nullptr;
    lv_obj_t* _lbl_weather  = nullptr;
    lv_obj_t* _lbl_battery  = nullptr;
    lv_obj_t* _content_area = nullptr;
    lv_obj_t* _tab_bar      = nullptr;
    lv_obj_t* _tab_btns[4]  = {};

    // Per-tab content containers (recreated on tab switch)
    lv_obj_t* _tab_content  = nullptr;

    // Device tab horizontal page state
    lv_obj_t* _dot_container   = nullptr;
    static constexpr int MAX_PAGE_DOTS = 4;
    lv_obj_t* _page_dots[MAX_PAGE_DOTS] = {};
    int       _n_page_dots = 0;

    void _build_skeleton();
    void _build_header();
    void _build_tab_bar();
    void _rebuild_tab_content(const std::vector<DeviceCard>& cards,
                               const std::vector<DeviceCard>& sensors);
    void _create_page_dots(int n_pages);

    void _update_header(const WeatherInfo& w, const BatteryInfo& battery, bool connected,
                        const std::string& time_str, const std::string& date_str);
    void _update_tab(TabPage tab,
                     const std::vector<DeviceCard>& living,
                     const std::vector<DeviceCard>& kitchen,
                     const std::vector<DeviceCard>& media,
                     const std::vector<DeviceCard>& sensors,
                     const std::vector<DeviceCard>& appliance);
};

}  // namespace ha_view
