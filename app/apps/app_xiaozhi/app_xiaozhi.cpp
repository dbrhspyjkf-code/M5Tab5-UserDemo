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
    // 悬浮层架构 (2026-11-11): 亮出透明 overlay, 底下原 app 页面保持不动
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

    // ── 5s 自动退出 (2026-11-10 定稿, 用户由 3s 调宽): 两种情况 ──
    // ① 唤醒后 5s 内用户没说话 (Listening 期间 VAD 没检测到人声, 识别文本也
    //    没回传) → 退出; ② 小智回答完后 5s 没接话 (回到 Idle) → 退出。
    // 听到过用户说话的 Listening (等 STT/LLM/TTS) 与 Speaking/Connecting 不计时。
    // VAD 来自 audio_service 的轻量能量检测 (本板 AFE 处理器被裁, 原 VAD 恒
    // false; 弹窗提示音已在底层 700ms 屏蔽窗内), STT 回传时间戳做兜底。
    int st = xiaozhi_get_device_state();
    if (st != _last_state) {
        if (_last_state == kDeviceStateSpeaking && st != kDeviceStateSpeaking) {
            _answered = true;   // 一次回答播完
        }
        if (st == kDeviceStateListening) {
            _heard_user = false;
            _stt_at_listen_start = xiaozhi_last_user_stt_ms();
        }
        _last_voice_ms = now_ms;
        _last_state = st;
    }
    if (st == kDeviceStateListening) {
        if (xiaozhi_is_voice_detected() ||
            xiaozhi_last_user_stt_ms() != _stt_at_listen_start) {
            _heard_user = true;
            _last_voice_ms = now_ms;
        }
    }
    bool silent5s = (now_ms - _last_voice_ms > 5000);
    if ((st == kDeviceStateListening && !_heard_user && silent5s) ||
        (_answered && st == kDeviceStateIdle && silent5s)) {
        // mclog 是 fmt 库: {} 占位符 (非 printf %d, 2026-11-11 实锤)
        mclog::tagInfo(TAG, "auto-exit: 5s rule (st={} heard={} answered={})",
                       st, (int)_heard_user, (int)_answered);
        close();
        return;
    }
}

void AppXiaoZhi::onClose()
{
    xiaozhi_deactivate_screen();   // 隐藏透明 overlay, 原页面原样露出 (无屏切换, 无需 restore)
    _removeSwipeGesture();
    // 退出即结束会话: Speaking 丢掉剩余 TTS、Listening 关通道回 Idle+重开唤醒词。
    // 否则关掉画面后后台还在听 (服务端静默超时 ~66s), 旁人说话会被当指令应答。
    // Idle 时调用为 no-op (回答完自动退出场景)。
    xiaozhi_end_conversation();
    // 注意: 不调 _close_cb (home->restoreScreen) — 活动屏从未被切走,
    // restoreScreen 反而会把底下正在显示的 app 页面拉回主页。
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
                if (code == LV_EVENT_CLICKED) {
                    // 触碰小智界面即退出 (2026-11-10 用户定稿; 点按说话已移除)。
                    // 前台守卫同上拨: indev 事件全局广播, 别的 app 里点按不得触发
                    // close() → restoreScreen 拉走页面 (2026-10-27 同款教训)。
                    if (!xiaozhi_screen_is_active()) return;
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppXiaoZhi*>(udata);
                        if (app) app->close();
                    }, lv_event_get_user_data(e));
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
