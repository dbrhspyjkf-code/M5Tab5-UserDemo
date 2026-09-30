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
    std::string icon;
    bool is_on      = false;
    bool is_offline = false;

    bool operator==(const DeviceCard& o) const
    {
        return entity_id == o.entity_id && label == o.label && is_on == o.is_on
            && is_offline == o.is_offline && icon == o.icon;
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

class HaView {
public:
    ~HaView();

    // action: "toggle" | "set_percentage" | "oscillate" | "set_preset_mode"
    using ActionCb = std::function<void(const std::string& entity_id,
                                        const std::string& action,
                                        const std::string& value)>;

    ActionCb _on_action_fn;  // public for card callbacks

    // Outlives `this`: the swipe-up-to-exit gesture defers via lv_async_call,
    // and lv_async_call_cancel() in the destructor has proven unreliable at
    // actually stopping every queued call before it fires against a freed
    // HaView (seen crashing in production — Guru Meditation in _exit_async_cb).
    // The deferred callback holds a weak_ptr to this flag and checks it before
    // touching `this`; flipped false as the very first thing in ~HaView().
    // Public so the gesture-registration lambda (a free function) can read it.
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);

    void init(ActionCb on_action);

    // Skip the throttle on the next update() call — used by action callbacks
    // so a tap reflects its optimistic state on the very next frame.
    void requestRebuild() { _force_rebuild = true; }

    // Called every frame by AppHA::onRunning()
    void update(const std::vector<DeviceCard>& living,
                const WeatherInfo& weather,
                const BatteryInfo& battery,
                bool connected,
                const std::string& time_str,
                const std::string& date_str);

private:
    ActionCb _on_action;

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

    std::vector<DeviceCard> _living_cache;

    // Indev for swipe-up-to-exit gesture
    lv_indev_t* _gesture_indev = nullptr;
    // Opaque heap context (ExitCbCtx, defined in view.cpp) passed as the
    // indev gesture callback's user_data instead of `this`. See the comment
    // above _build_skeleton's gesture registration in view.cpp for why: a
    // raw `this` there was observed crashing in production (dangling access
    // after HaView was destroyed but lv_indev_remove_event_cb_with_user_data
    // didn't actually stop the callback in time).
    void* _gesture_ctx = nullptr;

    // Root screen objects
    lv_obj_t* _scr          = nullptr;
    lv_obj_t* _lbl_time     = nullptr;
    lv_obj_t* _lbl_date     = nullptr;
    lv_obj_t* _lbl_temp     = nullptr;
    lv_obj_t* _lbl_weather  = nullptr;
    lv_obj_t* _lbl_battery  = nullptr;
    lv_obj_t* _content_area = nullptr;

    lv_obj_t* _tab_content  = nullptr;

    void _build_skeleton();
    void _build_header();

    void _update_header(const WeatherInfo& w, const BatteryInfo& battery, bool connected,
                        const std::string& time_str, const std::string& date_str);
    void _update_lights(const std::vector<DeviceCard>& living);
};

}  // namespace ha_view
