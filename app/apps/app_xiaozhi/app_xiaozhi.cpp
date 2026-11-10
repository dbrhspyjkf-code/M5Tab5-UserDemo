#include "app_xiaozhi.h"
#include "xiaozhi_ctl.h"
#include "device_state.h"

#include <mooncake_log.h>
#include <hal/hal.h>
#include <esp_timer.h>

static const char* TAG = "AppXiaoZhi";

// 桌面/web 平台的中文字体回退 (与 app_home 等同款惯例; M1 起 web 走 cbin 三档)
#if defined(PLATFORM_BUILD_DESKTOP)
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh20()
{
    return &font_puhui_20_4;
}
#endif

AppXiaoZhi::AppXiaoZhi()
{
    setAppInfo().name = "小智";
}

void AppXiaoZhi::onCreate()
{
    // Lazy init: do NOT start xiaozhi here. Starting at install time means any
    // failure in xiaozhi init crashes the device into a boot loop before the
    // launcher is usable. We start it on first open instead.
}

void AppXiaoZhi::onOpen()
{
#ifdef PLATFORM_BUILD_WEB
    // web 版: 浏览器无麦克风/扬声器音频管道, xiaozhi 语音对话不可用。
    // 显示占位说明屏, 点按任意处返回主页。(后续单独立项做 WebRTC 麦克风)
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    auto* title = lv_label_create(_scr);
    lv_label_set_text(title, "小  智");
    lv_obj_set_style_text_font(title, zh20(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8E6E3), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 160);

    auto* msg = lv_label_create(_scr);
    lv_label_set_text(msg, "网页版暂不支持语音对话\n\n请在 Tab5 上使用小智\n\n点按任意位置返回");
    lv_obj_set_style_text_font(msg, zh20(), 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x8A8A8A), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 40);

    lv_obj_add_event_cb(_scr, [](lv_event_t* e) {
        lv_async_call([](void* ud) {
            static_cast<AppXiaoZhi*>(ud)->close();
        }, lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, this);
    lv_screen_load(_scr);
#else
    xiaozhi_start_task();
    // 记录唤醒前的画面 (onOpen 在 LvglLockGuard 内由 mooncake update 调用):
    // 模糊背景显示它, 关闭时也回到它。已在小智屏上时保留原记录不覆盖。
    if (!xiaozhi_screen_is_active()) {
        _return_scr = lv_screen_active();
    }
    xiaozhi_activate_screen();
    _last_voice_ms = esp_timer_get_time() / 1000;
    _last_state = xiaozhi_get_device_state();
    _installSwipeGesture();
#endif
}

#ifndef PLATFORM_BUILD_WEB
void AppXiaoZhi::onRunning()
{
    // Update battery reading every 10 s for the xiaozhi info bar.
    static int64_t s_last_batt_ms = 0;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_batt_ms > 10000) {
        s_last_batt_ms = now_ms;
        auto* hal = GetHAL();
        if (hal) {
            hal->updatePowerMonitorData();
            float v = hal->powerMonitorData.busVoltage;
            int pct = -1;
            if (v > 0.5f) {
                if      (v >= 8.4f) pct = 100;
                else if (v <= 6.0f) pct = 0;
                else pct = (int)((v - 6.0f) / 2.4f * 100.0f);
            }
            xiaozhi_set_battery_percent(pct);
        }
    }

    // ── 回答完成后 3s 无接话自动退出 (2026-11-10 用户定稿) ──
    // 只有小智完整回答过至少一次(_answered: 状态离开 Speaking)后, 回到 Idle
    // 再静默 3s 才退出; 唤醒后等用户开口的 Listening / 连接中 Connecting /
    // 回答间隙一律不计时 —— 画面刚显示就开始倒计时是错误行为(用户明确纠正)。
    // 每次状态跳转都重置静默计时(进入 Idle / 重新唤醒都从零起算)。
    int st = xiaozhi_get_device_state();
    if (st != _last_state) {
        if (_last_state == kDeviceStateSpeaking && st != kDeviceStateSpeaking) {
            _answered = true;   // 一次回答播完
        }
        _last_voice_ms = now_ms;
        _last_state = st;
    }
    if (_answered && st == kDeviceStateIdle && now_ms - _last_voice_ms > 3000) {
        mclog::tagInfo(TAG, "auto-exit: 3s no follow-up after answer");
        close();
        return;
    }
}

void AppXiaoZhi::onClose()
{
    xiaozhi_deactivate_screen();
    _removeSwipeGesture();
    if (_return_scr) {
        // 回到唤醒前的画面 (同样在 LvglLockGuard 内; app 屏幕常驻, 指针有效)
        lv_screen_load(_return_scr);
        _return_scr = nullptr;
    } else if (_close_cb) {
        _close_cb();   // 兜底: 无记录时回主页 (home->restoreScreen)
    }
}
#else
void AppXiaoZhi::onRunning() {}

void AppXiaoZhi::onClose()
{
    if (_close_cb) {
        _close_cb();
    }
}
#endif

void AppXiaoZhi::_installSwipeGesture()
{
    _removeSwipeGesture();

    // Register on the pointer indev directly so the gesture fires regardless of
    // which LVGL object the user touches (chat area, emotion area, etc.).
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            static lv_point_t s_gpress{0, 0};   // PRESSED 起点 (底部起手判定)
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSED) {
                    lv_point_t p; lv_indev_get_point(dev, &p);
                    s_gpress = p;
                    return;
                }
                if (code != LV_EVENT_GESTURE) return;
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // 双守卫: ① 小智屏在前台 (无此前台守卫时, 别的 app 里
                    // 上拨会触发 close() → restoreScreen 拉回主页, 2026-10-27
                    // "邮件/汇率等子页上拨退到主页" 真凶); ② 屏幕最下缘起手.
                    if (!xiaozhi_screen_is_active()) return;
                    if (s_gpress.y <= 650) return;
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppXiaoZhi*>(udata);
                        if (app) app->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_ALL, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppXiaoZhi::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}
