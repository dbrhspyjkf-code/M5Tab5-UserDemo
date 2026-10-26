#pragma once
/*
 * app_timer — 计时器(参考 M5DIAL app_timer 移植到 Tab5 触摸大屏)。
 *
 * 状态机同源: EDIT(设定) → RUNNING(倒计时) ⇄ PAUSED → DONE(到时闪烁+5连响)。
 * 提示音频率同 M5DIAL(启动 2600/暂停 1800/继续 2600/重置 2200/半程 1800→2200/
 * 到时 2200×5),经 utils/audio play_tone 走 ES8388。
 * UI: 中央 120px 大数字 + lv_arc 进度环;点住数字上下拖动调时(±1 分钟/40px)
 * + 常用预设 chips;主按钮(开始/暂停/继续/重置)。上拨退出(退出即取消)。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>

class AppTimer : public mooncake::AppAbility {
public:
    AppTimer();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    enum class State { EDIT, RUNNING, PAUSED, DONE };

    std::function<void()> _close_cb;

    State _state = State::EDIT;
    int _set_min = 5, _set_sec = 0;         // EDIT 态设定值
    int _remain = 300;                       // 剩余秒(RUNNING/PAUSED/DONE)
    int _duration = 300;                     // 本次总时长(锁定,驱动进度环)
    bool _half_notified = false;
    uint32_t _last_tick_ms = 0;
    // DONE 闪烁 + 5 连响节拍
    uint32_t _last_blink_ms = 0;
    bool _blink_on = true;
    int _beep_count = 0;
    uint32_t _last_beep_ms = 0;

    // ── UI ──
    lv_obj_t* _scr      = nullptr;
    lv_obj_t* _state_chip = nullptr;   // 已取消, 保留成员防引用崩
    lv_obj_t* _hint_lbl  = nullptr;   // 顶部提示: 触摸后 3s 内显示
    lv_obj_t* _arc      = nullptr;           // 进度环(RUNNING/PAUSED/DONE)
    lv_obj_t* _time_lbl = nullptr;           // 120px 大数字
    lv_obj_t* _drag_pad = nullptr;           // 中央数字拖动调时命中区(左=分)
    lv_obj_t* _drag_pad_sec = nullptr;       // 右半命中区(=秒)
    int _drag_last_y = 0;
    int _drag_acc    = 0;                    // 累计像素(上正); 40px = ±1 分钟
    lv_obj_t* _presets  = nullptr;           // EDIT 态: 预设 chips 容器
    lv_obj_t* _main_btn = nullptr;           // 开始/暂停/继续/重置
    lv_obj_t* _main_lbl = nullptr;

    void _enterState(State s);
    void _refresh();                         // 全量刷新(状态/数字/环/按钮)
    void _tick();                            // onRunning 每秒节拍
    void _adjust(int dmin, int dsec);
    static void _mainClick_cb(lv_event_t* e);
    static void _presetClick_cb(lv_event_t* e); // user_data = 分钟数
    static void _drag_cb(lv_event_t* e);       // 数字上下拖动 = ±分钟

    // ── 上拨手势退出(app_radio 同款) ──
    struct SwipeCtx {
        AppTimer* app;
        std::weak_ptr<bool> alive;
    };
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void* _gesture_ctx = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose() { close(); }
};
