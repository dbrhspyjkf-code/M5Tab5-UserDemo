/*
 * app_timer — 计时器实现(参考 M5DIAL app_timer,Tab5 触摸版)。
 */
#include "app_timer.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include "../utils/audio/audio.h"
#include <cstdio>
#include <cstdint>

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
#include "../utils/assets/assets_partition.h"
static const lv_font_t* zh_font_md()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p))
                 : cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
#endif

extern const lv_font_t font_digits_120;   // app_timer/font_digits_120.c

static const char* TAG = "timer";

// 配色(驾驶舱色板)
static constexpr uint32_t C_BG     = 0x000000;
static constexpr uint32_t C_CARD   = 0x13304E;
static constexpr uint32_t C_ON     = 0x1E5288;
static constexpr uint32_t C_ACCENT = 0x4FA3FF;
static constexpr uint32_t C_AMBER  = 0xF5A263;
static constexpr uint32_t C_TEXT   = 0xEAF3FB;
static constexpr uint32_t C_TEXT2  = 0x8FA8C4;

static const int kPresets[] = {1, 3, 5, 10, 15, 25, 30};   // 分钟
static const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

AppTimer::AppTimer()
{
    setAppInfo().name = "AppTimer";
}

void AppTimer::onCreate() {}

static lv_obj_t* mk_round_btn(lv_obj_t* parent, int w, int h, uint32_t bg,
                              lv_event_cb_t cb, void* user)
{
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

static lv_obj_t* mk_center_label(lv_obj_t* parent, const lv_font_t* font,
                                 uint32_t color, const char* text)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return l;
}

void AppTimer::onOpen()
{
    if (_scr) return;
    mclog::tagInfo(TAG, "on open");

    _state = State::EDIT;
    _set_min = 5; _set_sec = 0;
    _remain = 300; _duration = 300;

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部提示(触摸后 3s 内显示, 平时隐藏; 标题/状态 chip 已按要求取消)
    _hint_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_hint_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_hint_lbl, lv_color_hex(C_TEXT2), 0);
    lv_label_set_text(_hint_lbl, "左半数字拖=分钟 右半=秒 · 上拨退出");
    lv_obj_align(_hint_lbl, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_add_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);

    // 进度环(360px,中央大数字叠在上面)
    _arc = lv_arc_create(_scr);
    lv_obj_set_size(_arc, 360, 360);   // 400 再缩 10%; 中心 y=270
    lv_obj_align(_arc, LV_ALIGN_TOP_MID, 0, 90);
    lv_arc_set_rotation(_arc, 270);
    lv_arc_set_bg_angles(_arc, 0, 360);
    lv_arc_set_range(_arc, 0, 100);
    lv_arc_set_value(_arc, 100);
    lv_obj_remove_style(_arc, NULL, LV_PART_KNOB);   // 无旋钮
    lv_obj_clear_flag(_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(_arc, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_arc_width(_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_color(_arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(_arc, 14, LV_PART_INDICATOR);

    // 大数字(居中于环)
    _time_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_time_lbl, &font_digits_120, 0);
    lv_obj_set_style_text_color(_time_lbl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(_time_lbl, "05:00");
    lv_obj_align(_time_lbl, LV_ALIGN_CENTER, 0, -90);   // 环心 y=270

    // 中央数字拖动调时: 命中区拆左右两块, 分别盖住 MM 与 SS 段 ——
    // 左块上下拖 = ±1 分钟/40px, 右块 = ±10 秒/40px(EDIT/PAUSED 生效)
    auto mk_drag = [&](int cx, intptr_t part) {
        lv_obj_t* pad = lv_obj_create(_scr);
        lv_obj_set_size(pad, 200, 260);
        lv_obj_align(pad, LV_ALIGN_CENTER, cx, -90);
        lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pad, 0, 0);
        lv_obj_set_style_pad_all(pad, 0, 0);
        lv_obj_set_scrollbar_mode(pad, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(pad, (void*)part);   // 0=分钟 1=秒
        lv_obj_add_event_cb(pad, _drag_cb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(pad, _drag_cb, LV_EVENT_PRESSING, this);
        return pad;
    };
    _drag_pad = mk_drag(-100, 0);
    _drag_pad_sec = mk_drag(100, 1);

    // EDIT 态: 预设 chips(步进器上方一行)
    _presets = lv_obj_create(_scr);
    lv_obj_set_size(_presets, 1000, 70);
    lv_obj_align(_presets, LV_ALIGN_TOP_MID, 0, 500);
    lv_obj_set_style_bg_opa(_presets, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_presets, 0, 0);
    lv_obj_set_style_pad_all(_presets, 0, 0);
    lv_obj_clear_flag(_presets, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_presets, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_presets, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < kPresetCount; i++) {
        char txt[12];
        snprintf(txt, sizeof(txt), "%d分", kPresets[i]);
        lv_obj_t* chip = mk_round_btn(_presets, 118, 56, C_CARD,
                                      _presetClick_cb, this);
        lv_obj_set_user_data(chip, (void*)(intptr_t)kPresets[i]);
        mk_center_label(chip, zh_font_md(), C_ACCENT, txt);
    }

    // 主按钮(底部居中)
    _main_btn = mk_round_btn(_scr, 272, 82, C_ON, _mainClick_cb, this);   // 缩 15%
    lv_obj_align(_main_btn, LV_ALIGN_BOTTOM_MID, 0, -40);   // 屏幕水平居中
    _main_lbl = mk_center_label(_main_btn, zh_font_lg(), C_TEXT, "开始");

    _installSwipeGesture();
    _refresh();
    lv_screen_load_anim(_scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ─── 状态机 ────────────────────────────────────────────────────────────────

void AppTimer::_enterState(State s)
{
    switch (s) {
        case State::EDIT:
            _state = State::EDIT;
            audio::play_tone(2200, 0.08);   // 重置音(M5DIAL 同款)
            break;
        case State::RUNNING:
            _state = State::RUNNING;
            _last_tick_ms = GetHAL()->millis();
            _half_notified = false;
            audio::play_tone(2600, 0.06);   // 启动/继续音
            break;
        case State::PAUSED:
            _state = State::PAUSED;
            _set_min = _remain / 60;
            _set_sec = _remain % 60;
            audio::play_tone(1800, 0.05);   // 暂停音
            break;
        case State::DONE:
            _state = State::DONE;
            _remain = 0;
            _blink_on = true;
            _last_blink_ms = GetHAL()->millis();
            _beep_count = 0;
            _last_beep_ms = GetHAL()->millis() - 300;   // 立即响第一声
            break;
    }
    _refresh();
}

void AppTimer::_adjust(int dmin, int dsec)
{
    if (_state != State::EDIT && _state != State::PAUSED) return;
    int total = _set_min * 60 + _set_sec + dmin * 60 + dsec;
    if (total < 0) total = 0;
    if (total > 99 * 60 + 59) total = 99 * 60 + 59;
    _set_min = total / 60;
    _set_sec = total % 60;
    _refresh();
}

void AppTimer::_tick()
{
    uint32_t now = GetHAL()->millis();
    if (_state == State::RUNNING) {
        if (now - _last_tick_ms >= 1000) {
            _last_tick_ms += 1000;
            _remain -= 1;
            if (!_half_notified && _remain > 0 && _remain <= _duration / 2) {
                _half_notified = true;
                audio::play_tone(1800, 0.07);   // 半程两音上行(M5DIAL 同款)
                // 第二音由下一轮 onRunning 延时补(不阻塞)
            }
            if (_remain <= 0) {
                _enterState(State::DONE);
                return;
            }
            _refresh();
        }
    }
    if (_state == State::DONE) {
        // 闪烁(500ms 相位)
        if (now - _last_blink_ms >= 500) {
            _last_blink_ms = now;
            _blink_on = !_blink_on;
            lv_obj_set_style_text_color(_time_lbl,
                lv_color_hex(_blink_on ? C_AMBER : C_CARD), 0);
        }
        // 到时 3 声滴滴(2026-10-26 指定; 150ms 响 / 150ms 歇)
        if (_beep_count < 3 && now - _last_beep_ms >= 300) {
            _last_beep_ms = now;
            _beep_count++;
            audio::play_tone(2200, 0.15);
        }
    }
}

// ─── 刷新 ──────────────────────────────────────────────────────────────────

void AppTimer::_refresh()
{
    int show = (_state == State::EDIT) ? _set_min * 60 + _set_sec : _remain;
    lv_label_set_text_fmt(_time_lbl, "%02d:%02d", show / 60, show % 60);

    // 进度环
    int pct = (_duration > 0) ? (int)(_remain * 100 / _duration) : 100;
    if (_state == State::EDIT) {
        lv_arc_set_value(_arc, 100);
        lv_obj_set_style_text_color(_time_lbl, lv_color_hex(C_TEXT), 0);
    } else {
        lv_arc_set_value(_arc, pct);
        if (_state != State::DONE) {
            lv_obj_set_style_text_color(_time_lbl,
                lv_color_hex(_state == State::PAUSED ? C_TEXT2 : C_TEXT), 0);
        }
    }

    // 状态 chip + 主按钮 + 编辑区可见性
    const char* chip = "?";
    const char* btn  = "?";
    switch (_state) {
        case State::EDIT:    chip = "设定";  btn = "开始"; break;
        case State::RUNNING: chip = "计时中"; btn = "暂停"; break;
        case State::PAUSED:  chip = "已暂停"; btn = "继续"; break;
        case State::DONE:    chip = "到时!";  btn = "重置"; break;
    }
    if (_state_chip) lv_label_set_text(_state_chip, chip);
    lv_label_set_text(_main_lbl, btn);

    if (_state == State::EDIT) lv_obj_clear_flag(_presets, LV_OBJ_FLAG_HIDDEN);
    else                       lv_obj_add_flag(_presets, LV_OBJ_FLAG_HIDDEN);
}

// ─── 事件 ──────────────────────────────────────────────────────────────────

void AppTimer::_mainClick_cb(lv_event_t* e)
{
    auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
    switch (self->_state) {
        case State::EDIT:
            if (self->_set_min * 60 + self->_set_sec > 0) {
                self->_remain = self->_set_min * 60 + self->_set_sec;
                self->_duration = self->_remain;
                self->_enterState(State::RUNNING);
            }
            break;
        case State::RUNNING:
            self->_enterState(State::PAUSED);
            break;
        case State::PAUSED:
            self->_remain = self->_set_min * 60 + self->_set_sec;
            self->_enterState(State::RUNNING);
            break;
        case State::DONE:
            self->_enterState(State::EDIT);
            break;
    }
}

void AppTimer::_drag_cb(lv_event_t* e)
{
    auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
    lv_point_t p;
    lv_indev_get_point(lv_event_get_indev(e), &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->_drag_last_y = p.y;
        self->_drag_acc = 0;
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
    bool is_sec = (intptr_t)lv_obj_get_user_data(
        static_cast<lv_obj_t*>(lv_event_get_current_target(e))) == 1;   // 0=分 1=秒
    self->_drag_acc += self->_drag_last_y - p.y;   // 上拖为正
    self->_drag_last_y = p.y;
    while (self->_drag_acc >= 40) {
        is_sec ? self->_adjust(0, 10) : self->_adjust(1, 0);
        self->_drag_acc -= 40;
    }
    while (self->_drag_acc <= -40) {
        is_sec ? self->_adjust(0, -10) : self->_adjust(-1, 0);
        self->_drag_acc += 40;
    }
}

void AppTimer::_presetClick_cb(lv_event_t* e)
{
    auto* self = static_cast<AppTimer*>(lv_event_get_user_data(e));
    lv_obj_t* chip = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int minutes = (int)(intptr_t)lv_obj_get_user_data(chip);
    self->_set_min = minutes;
    self->_set_sec = 0;
    self->_refresh();
}

// ─── 生命周期 ──────────────────────────────────────────────────────────────

void AppTimer::onRunning()
{
    // 顶部提示: 触摸后 3s 内显示, 平时隐藏
    if (_hint_lbl) {
        bool touched = lv_display_get_inactive_time(nullptr) < 3000;
        if (touched == lv_obj_has_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN)) {
            if (touched) lv_obj_clear_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);
            else         lv_obj_add_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    _tick();
}

void AppTimer::onClose()
{
    mclog::tagInfo(TAG, "on close");
    if (_close_cb) _close_cb();
    *_alive = false;
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
}

// ─── 上拨手势(app_radio 同款) ─────────────────────────────────────────────

void AppTimer::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (lv_indev_get_gesture_dir(
                        static_cast<lv_indev_t*>(lv_event_get_target(e))) == LV_DIR_TOP) {
                    auto* defer = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestClose();
                        delete c;
                    }, defer);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppTimer::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
}
