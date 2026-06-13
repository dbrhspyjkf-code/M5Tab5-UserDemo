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
enum class TabPage { LIVING = 0, KITCHEN_BATH, MEDIA };

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
    void update(const std::vector<DeviceCard>& living,   // 灯光
                const std::vector<DeviceCard>& kitchen,  // 设备
                const std::vector<DeviceCard>& media,    // 影音
                const std::vector<DeviceCard>& sensors,  // 传感器（所有房间）
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

    // ─── Throttle: tab content is the LVGL-heavy part (~30–50 lv_obj_create
    // + delete per rebuild). Rebuilding it every 500 ms fragmented PSRAM and
    // caused lv_obj_create() to return NULL after a few hours, crashing the
    // next lv_obj_set_size on a dangling pointer. Header is cheap (4 label
    // text updates), so it stays at the full onRunning cadence.
    static constexpr uint32_t TAB_REBUILD_INTERVAL_MS = 2000;
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

    // Root screen objects
    lv_obj_t* _scr          = nullptr;
    lv_obj_t* _lbl_time     = nullptr;
    lv_obj_t* _lbl_date     = nullptr;
    lv_obj_t* _lbl_temp     = nullptr;
    lv_obj_t* _lbl_weather  = nullptr;
    lv_obj_t* _lbl_battery  = nullptr;
    lv_obj_t* _content_area = nullptr;
    lv_obj_t* _tab_bar      = nullptr;
    lv_obj_t* _tab_btns[3]  = {};

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
                     const std::vector<DeviceCard>& sensors);
};

}  // namespace ha_view
