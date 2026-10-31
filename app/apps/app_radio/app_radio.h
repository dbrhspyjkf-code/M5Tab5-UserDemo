#pragma once
/*
 * app_radio — 网络收音机（参考 AI Passport 的 radio_app，适配 Tab5 触摸大屏）。
 *
 * 数据链路与 Passport 相同：OrangePi 8769 端口的 radio_relay.py 用 ffmpeg 把
 * 电台流转码成 16kHz 单声道 s16le PCM，设备 HTTP 直读 → PSRAM 环形缓冲 →
 * I2S 直写，零解码。播放期间必须关 WiFi 省电（Passport 实测：省电开着必断流）。
 *
 * UI 差异：Tab5 是 1280×720 触摸屏 —— 左侧电台列表（点击换台），右侧播放
 * 控制（上一台/播放暂停/下一台 + 音量滑条），上拨回主屏（后台继续播放，
 * 2026-10-28；主页状态栏 ♪ 图标可回）。彻底停止需在收音机页按暂停。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <memory>
#include <functional>
#include <string>

class AppRadio : public mooncake::AppAbility {
public:
    AppRadio();

    // 关闭回调(app_installer 注入: 回主屏), onClose 里先切屏再删对象
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    // 播放期间抑制屏保时钟遮盖(app_installer 注入)。
    // 2026-10-28 后台播放: 抑制条件从"app 打开"改为"正在播放"——收音机页
    // 关掉后流任务继续, 时钟遮盖依旧不弹(熄屏仍允许, 听歌熄屏更合理)。
    void setSuppressor(std::function<void(bool)> cb) { _suppress = std::move(cb); }

    // 前台独占屏保抑制(2026-10-31): 收音机界面期间完全不进屏保(不弹遮盖
    // 也不熄屏), 与播放状态无关; 最小化/关闭后恢复常规屏保语义。
    void setBlocker(std::function<void(bool)> cb) { _block = std::move(cb); }

    // ── 后台播放状态 (主页状态栏入口用; UI 关掉后流任务仍活着) ──
    bool streamActive() const;                 // 流任务在播/重连中
    std::string streamInfo() const;            // 播放中的台名, 未播返回 ""

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::function<void()> _close_cb;
    std::function<void(bool)> _suppress;
    std::function<void(bool)> _block;
    // ── UI ──
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _status_lbl = nullptr;   // 顶栏右侧状态
    lv_obj_t* _now_name   = nullptr;   // 右侧当前台名
    lv_obj_t* _now_title  = nullptr;   // ICY 正在播放歌名(20px 副行, relay 收割)
    uint32_t  _np_seen    = 0;         // 已刷进 UI 的歌名序号
    lv_obj_t* _play_icon  = nullptr;   // 播放/暂停按钮内容
    lv_obj_t* _vol_slider = nullptr;
    lv_obj_t* _vol_lbl    = nullptr;
    lv_obj_t* _row_sel    = nullptr;   // 列表选中高亮条
    int       _row_y(int i) const;

    // ── 电平图(真分频 VU): 单对象绘制事件直绘 20 根琥珀条 ──
    // (Passport 教训: 每根条一个对象的 20fps 刷新会抢 CPU 致音频断续)
    lv_obj_t* _vu_strip = nullptr;   // 单条带对象, draw 回调直绘
    float     _vu_cur[20] = {0};     // 平滑后的目标高度(px)
    uint32_t  _vu_ms = 0;
    std::atomic<int> _lvl_b{0}, _lvl_m{0}, _lvl_t{0};   // 0..100, 流任务写
    void _updateVu();
    static void _vu_draw_cb(lv_event_t* e);

    // ── 刻度盘(参考 leo-radio: 88-108MHz 刻度 + 琥珀指针扫动) ──
    lv_obj_t* _dial       = nullptr;   // 刻度盘容器(刻度走绘制事件, 零对象)
    lv_obj_t* _needle     = nullptr;   // 调谐指针
    lv_obj_t* _needle_glow= nullptr;   // 指针光晕
    lv_obj_t* _now_freq   = nullptr;   // 频率读数(FM 台才有, 网络台显示"网络电台")
    int  _freq_of(int idx) const;      // 电台在刻度上的位置(0.1MHz; 网络台均布)
    int  _needle_x(int freq_tenths) const;
    void _move_needle(int idx, bool animate);
    static void _needle_anim_cb(void* var, int32_t v);
    static void _dial_draw_cb(lv_event_t* e);

    // ── 流任务通信（全部原子量，任务绝不触碰 LVGL）──
    std::atomic<int>  _want_station{-2};   // -2 初始 / -1 停止 / 其余=电台下标
    std::atomic<bool> _app_closing{false};
    std::atomic<int>  _state{0};           // 0待机 1连接中 2播放中 3失败
    std::atomic<bool> _task_running{false};
    int _ui_state    = -1;                 // 上次刷进 UI 的值（变化才更新）
    int _ui_station  = -1;
    int _last_station = 0;                 // 暂停/失败后 播放键恢复用

    void _startTask();
    void _play(int idx);
    void _refreshStationUi();
    void _refreshStateUi();
    static void _rowClick_cb(lv_event_t* e);
    static void _playClick_cb(lv_event_t* e);
    static void _prevClick_cb(lv_event_t* e);
    static void _nextClick_cb(lv_event_t* e);
    static void _vol_cb(lv_event_t* e);

    // ── 上拨手势退出（app_stocks 同款）──
    struct SwipeCtx {
        AppRadio* app;
        std::weak_ptr<bool> alive;
    };
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void* _gesture_ctx = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose();
};
