#include "app_home.h"
#include <mooncake.h>
#include <mooncake_log.h>
#include <smooth_lvgl.h>

static const std::string _tag = "app-home";

// Chinese font defined in app_ha/view/font_zh_36.c
extern lv_font_t font_zh_36;

AppHome::AppHome()
{
    setAppInfo().name = "AppHome";
}

void AppHome::addApp(const std::string& name, int app_id)
{
    _apps.push_back({name, app_id});
}

void AppHome::restoreScreen()
{
    if (_scr) {
        lv_screen_load(_scr);
    }
    _child_app_id = -1;
}

void AppHome::onCreate()
{
    open();  // home auto-opens immediately after install
}

void AppHome::onOpen()
{
    mclog::tagInfo(_tag, "on open");

    // Create a dedicated screen so other apps can own their own screens
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x0D1B2A), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Stable storage for button user_data pointers — must not reallocate after reserve
    _btn_data.clear();
    _btn_data.reserve(_apps.size());

    // Center column of buttons
    static constexpr int BTN_W   = 420;
    static constexpr int BTN_H   = 90;
    static constexpr int BTN_GAP = 28;

    int total_h = (int)_apps.size() * BTN_H + ((int)_apps.size() - 1) * BTN_GAP;
    int start_y = (720 - total_h) / 2;  // 720 = screen height

    for (int i = 0; i < (int)_apps.size(); i++) {
        _btn_data.push_back({this, _apps[i].app_id});
        BtnData& bd = _btn_data.back();

        lv_obj_t* btn = lv_obj_create(_scr);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        lv_obj_set_pos(btn, (1280 - BTN_W) / 2, start_y + i * (BTN_H + BTN_GAP));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E3A5F), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E5A8F), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, _apps[i].name.c_str());
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl, &font_zh_36, 0);

        lv_obj_add_event_cb(btn, _btn_event_cb, LV_EVENT_CLICKED, &bd);
    }

    lv_screen_load(_scr);
}

void AppHome::onRunning()
{
    // Nothing needed — restoreScreen() is called via callback before child closes
}

void AppHome::onClose()
{
    mclog::tagInfo(_tag, "on close");
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _btn_data.clear();
}

void AppHome::_btn_event_cb(lv_event_t* e)
{
    auto* bd = static_cast<BtnData*>(lv_event_get_user_data(e));
    bd->home->_child_app_id = bd->app_id;
    mooncake::GetMooncake().openApp(bd->app_id);
}
