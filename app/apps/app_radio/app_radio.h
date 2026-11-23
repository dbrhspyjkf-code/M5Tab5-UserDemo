#pragma once
/*
 * app_radio — 网络收音机（oneapp 同款, 2026-11-15 改版）。
 *
 * 数据链路不变：OrangePi 8769 radio_relay.py 把电台流转码成 16kHz 单声道
 * s16le PCM，设备 HTTP 直读 → PSRAM 环形缓冲 → I2S 直写，零解码。外部输出
 * (客厅 Sonos / 起居室 HomePod) 经 relay → Music Assistant 转投。
 * 电台表 = oneapp 同源(radio_stations.py 经 relay /radio/stations 下发):
 * 四组页签(★收藏/国家/省级/地方/国际) + 收藏(NVS) + IP 归属地本地置顶。
 *
 * UI(oneapp 电台页同构): 左=分组页签+电台列表(点台播放/长按收藏, 列表
 * 单容器绘制事件直绘 — 204 台若一行一个对象会压垮内部 RAM), 右=播放面板
 * (状态/输出 chip/台名/ICY 歌名/VU 电平/播放控制/音量)。上拨回主屏(后台
 * 继续播放, 主页状态栏 ♪ 图标可回), 彻底停止需在收音机页按暂停。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <memory>
#include <functional>
#include <string>
#include <vector>

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
    void pauseForFocus();                      // 音频焦点: 油管开播时被踢暂停 (不退出后台播放语义)
    void setYtStopper(std::function<void()> cb) { _yt_stopper = std::move(cb); }   // 音频焦点: 开播踢油管
    std::string streamInfo() const;            // 播放中的台名, 未播返回 ""

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::function<void()> _close_cb;
    std::function<void()> _yt_stopper;
    std::function<void(bool)> _suppress;
    std::function<void(bool)> _block;
    // ── UI ──
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _status_lbl = nullptr;   // 右面板状态(未播放/连接中/播放中/失败)
    lv_obj_t* _play_icon  = nullptr;   // 播放/暂停按钮内容
    lv_obj_t* _now_name   = nullptr;   // 右侧当前台名
    lv_obj_t* _now_title  = nullptr;   // ICY 正在播放歌名(20px 副行, relay 收割)
    uint32_t  _np_seen    = 0;         // 已刷进 UI 的歌名序号
    lv_obj_t* _hdr_count  = nullptr;   // 头部 "· N 台"
    lv_obj_t* _hdr_loc    = nullptr;   // 头部归属地徽标 "本:广东·深圳"
    lv_obj_t* _tabs       = nullptr;   // 分组页签容器(★/国家/省级/地方/国际)
    lv_obj_t* _tab_btns[5] = {nullptr};
    lv_obj_t* _list       = nullptr;   // 电台列表容器(内容直绘, 零行对象)
    lv_obj_t* _toast      = nullptr;   // OSD 提示(已收藏·xx / 输出:xx, 1.5s 自隐)
    uint32_t  _toast_at   = 0;         // 显示起始 tick(0=未显示)
    void _showToast(const char* msg);

    // ── oneapp 同款列表状态(LVGL 线程专用) ──
    int  _radio_grp = 0;               // 0=★收藏 1..4=四组
    std::vector<int> _rows;            // 当前组显示的绝对下标序列
    lv_obj_t* _list_spacer = nullptr;  // 撑起滚动内容高度(直绘无行对象)
    void _setGroup(int g);
    void _rebuildTabs();
    void _rebuildRows();
    void _scrollToPlaying();
    int  _rowAt(int y) const;          // 屏幕纵坐标 → 行号(-1 无效)
    static void _list_draw_cb(lv_event_t* e);
    static void _list_event_cb(lv_event_t* e);   // CLICKED=播/停 LONG_PRESSED=收藏
    static void _tab_cb(lv_event_t* e);

    // ── 动态电台表(2026-11-02): relay /radio/stations 下发, 内置表兜底 ──
    std::atomic<bool> _stations_dirty{false};
    void _fetchStations();

    // ── 电平图(真分频 VU): 单对象绘制事件直绘 20 根琥珀条 ──
    // (Passport 教训: 每根条一个对象的 20fps 刷新会抢 CPU 致音频断续)
    lv_obj_t* _vu_strip = nullptr;   // 单条带对象, draw 回调直绘
    float     _vu_cur[20] = {0};     // 平滑后的目标高度(px)
    uint32_t  _vu_ms = 0;
    std::atomic<int> _lvl_b{0}, _lvl_m{0}, _lvl_t{0};   // 0..100, 流任务写
    void _updateVu();
    static void _vu_draw_cb(lv_event_t* e);

    // ── 流任务通信（全部原子量，任务绝不触碰 LVGL）──
    std::atomic<int>  _want_station{-2};   // -2 初始 / -1 停止 / 其余=电台下标
    std::atomic<bool> _app_closing{false};
    std::atomic<int>  _state{0};           // 0待机 1连接中 2播放中 3失败
    std::atomic<bool> _task_running{false};
    int64_t _spawn_retry_at = 0;           // 孵化失败退避截止(lv_tick ms), 仅 LVGL 线程访问
    int _ui_state    = -1;                 // 上次刷进 UI 的值（变化才更新）
    int _ui_station  = -1;
    int _last_station = 0;                 // 暂停/失败后 播放键恢复用

    void _startTask();
    void _play(int idx);
    void _stopPlayback();                  // 暂停: 外部→停投 + 本机断流(web 同)
    void _stepInGroup(int dir);            // 上一台/下一台: 当前组内循环
    void _refreshStationUi();
    void _refreshStateUi();
    static void _playClick_cb(lv_event_t* e);
    static void _prevClick_cb(lv_event_t* e);
    static void _nextClick_cb(lv_event_t* e);
    static void _vol_cb(lv_event_t* e);

    // ── 收藏(oneapp radio_fav 同款; NVS radio_favs 逗号分隔 key 表) ──
    std::vector<std::string> _favs;        // key 顺序表(仅 LVGL 线程访问)
    void _favLoad();
    void _favSave();
    bool _favHas(const std::string& key) const;
    void _favToggle(int idx);              // 绝对下标 + OSD

    // ── IP 归属地(oneapp radio_local 同款; 本地台置顶 + "本" 徽标) ──
    std::string _loc_prov, _loc_city;      // norm 后短名(空=未定位; 锁内读写)
    std::atomic<bool> _loc_dirty{false};
    void _locLoad();
    void _fetchLocation();                 // worker: ip-api → NVS radio_loc
    std::string _locText() const;          // "广东·深圳" 或 ""
    int  _localRank(int idx) const;        // 0=同城 1=同省 2=其他

    // ── 播放输出选择(2026-11-01): 本机 / 客厅 Sonos / 起居室 HomePod ──
    // 外部播放经 relay(:8769)→Music Assistant 转投; 命令一律在 detach
    // 短线程里发(阻塞 HTTP), UI 回调只写原子量。外部播放期间 _want_station
    // 仍指向所选台(♪/歌名/屏保语义复用), 流任务靠 _ext_on 抑制本机拉流。
    int               _output_sel = 0;   // 当前输出(0=本机; NVS radio_out)
    std::atomic<bool> _ext_on{false};    // 外部设备正在播(流任务读)
    std::atomic<int>  _ext_dev{-1};      // 正在播的外部设备下标
    std::atomic<int>  _ext_vol{-1};      // 外部音量查询结果(消费即清 -1)
    std::atomic<bool> _ext_vol_wait{false};
    uint32_t _ext_vol_poll_ms = 0;       // 上次外部音量轮询发起 tick(在播期 5s 轮询)
    uint32_t _vol_local_ms  = 0;         // 用户最近一次拖音量的 tick(3s 内不回写, 防跳回)
    std::atomic<bool> _np_kick{false};   // 请求流任务立即刷新歌名
    lv_obj_t* _out_chip = nullptr;       // 右面板输出 chip
    lv_obj_t* _out_modal = nullptr;      // 输出选择弹窗(layer_top, delete_async 关)
    lv_obj_t* _vol_slider = nullptr;
    lv_obj_t* _vol_lbl    = nullptr;
    void _openOutputModal();
    void _closeOutputModal();
    void _setOutput(int i);
    void _refreshOutputUi();
    void _extStart(int station);
    void _extStop();
    void _extVolSync();
    bool _extPlay(const char* pid, const char* path);   // 以下三个阻塞, 只在 detach 线程用
    bool _extSimple(const char* pid, const char* cmd, int v = -1);
    static void _outChip_cb(lv_event_t* e);
    static void _outRow_cb(lv_event_t* e);
    static void _modalBg_cb(lv_event_t* e);
    static void _modalDel_cb(lv_event_t* e);

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
