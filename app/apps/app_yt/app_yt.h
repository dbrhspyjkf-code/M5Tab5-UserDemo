#pragma once
/*
 * app_yt — 油管 (2026-11-06, car-dash 油管面板移植, 音频版)。
 *
 * car-dash 车机页油管面板的 Tab5 设备版: 数据链路同一份 Pi 中继
 * (hermes :8766 的 /yt/ 系端点), 但 P4 无 H.264/AAC 解码器 → **音频播放**:
 *   /yt/search?q=                       搜索 (yt-dlp 中继, 服务端缓存 10min)
 *   /yt/thumb/<id>.rgb565[?w=640&h=360] 列表缩略图 / 播放页大封面 (服务端转好零解码)
 *   /yt/pcm/<id>                        本机: 16k mono s16le (收音机同款消费链)
 *   外放 (客厅 Sonos / 起居室 HomePod; 收音机同款 relay → Music Assistant):
 *     /yt/prepare/<id> 预热管线 → relay :8769 /radio/yt/<id>/play_on?p=<pid>;
 *     暂停/续播/停止/音量一律走 relay /radio/cmd (与收音机外放同一套)
 *
 * UI 两视图: 搜索/结果 (两列卡片) ↔ 播放页 (大封面/标题/进度/播放暂停/输出/音量)。
 * 搜索输入: 点按弹软键盘 + 硬键盘 (lvKbGroup) + 语音 (识别成功自动搜)。
 * 失败/播完绝不自动重连 (11-06 两次 Pi 卡死教训), 重试只由用户点击触发。
 * 关 app / 失焦 = 停播 (本机与外放都停, car 同语义)。上拨退出回工具页。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#include "../utils/theme/theme.h"

class AppYt : public mooncake::AppAbility {
public:
    AppYt();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    // 后台播放 (2026-11-06): 主页 ♪ 图标读此状态; 再点主页油管入口直达播放页。
    // 返回空串 = 没在播。收音机同款语义 (app 常驻, atomics 跨会话存活)。
    // 音频焦点: 开播前踢收音机 (setRadioStopper 注入, installer 接线)。
    void setRadioStopper(std::function<void()> cb) { _radio_stopper = std::move(cb); }
    void pauseForFocus();             // 音频焦点: 收音机开播时被踢 (暂停本机/停外放)
    std::string nowPlayingInfo();   // 非 const: 顺带做外放播完检测 (后台时 onRunning 不跑)

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    static constexpr int SCREEN_W = 1280;
    static constexpr int SCREEN_H = 720;
    static constexpr int THUMB_W  = 320;     // 列表缩略图源尺寸 (显示 160×90)
    static constexpr int THUMB_H  = 180;
    static constexpr int COVER_W  = 640;     // 播放页大封面
    static constexpr int COVER_H  = 360;
    static constexpr int PCM_BYTES_PER_SEC = 32000;   // 16k mono s16le

    struct YtItem {
        std::string id, t, c;
        int d = 0;
    };

    // ── 主题色 ──
    static uint32_t C_BG()    { return 0x000000; }
    static uint32_t C_CARD()  { return theme::cur().card[1]; }
    static uint32_t C_TEXT()  { return theme::cur().text_hi; }
    static uint32_t C_SOFT()  { return theme::cur().text_soft; }
    static uint32_t C_DIM()   { return theme::cur().text_dim; }
    static uint32_t C_ACC()   { return theme::cur().accent; }
    static uint32_t C_TRACK() { return theme::cur().track; }
    static uint32_t C_BORDER(){ return theme::cur().border; }

    // ── 搜索数据 (worker 写 / UI 读, _mu 保护; worker 绝不碰 LVGL) ──
    void _searchAsync(const std::string& q);
    void _applySearch();
    std::mutex _mu;
    std::vector<YtItem> _items;
    std::string _search_err;
    std::atomic<bool> _search_ready{false};
    std::atomic<bool> _search_in_flight{false};
    std::atomic<uint32_t> _thumb_gen{0};          // 新结果作废在途缩略图 worker

    // ── 播放状态 (流任务/外放 worker 写原子量, UI 读) ──
    std::atomic<int> _want{-1};       // 本机流任务目标条目, -1=停
    std::atomic<int> _state{0};       // 0 停 / 1 连接中 / 2 播放 / 3 失败 / 4 播完
    std::atomic<int> _err_code{0};    // 失败原因: HTTP 状态码 / 1xxx=音箱转投 / -1 连不上 / 其它负数=本地错误
    std::atomic<int64_t> _pcm_bytes{0};
    std::atomic<bool> _paused{false}; // 本机: 流任务停读 (TCP 背压让服务端自然暂停); 外放: MA pause
    std::atomic<bool> _app_closing{false};
    std::atomic<bool> _task_started{false};
    // 正在播的条目 = 开播时从 _items 拷出的副本 (UI/mooncake 线程独占)。
    // 不能直接读 _items[_cur_item]: 搜索 worker 会在另一线程整表替换 _items,
    // 后台播放期间主页 ♪YT 也在读 —— 副本消除数据竞争与下标漂移。
    YtItem _cur;
    std::string _play_fid;                    // 流任务读 (受 _mu 保护)
    std::atomic<uint32_t> _play_gen{0};       // 每次开播 +1: 同一条目重播也强制流任务重建
    std::atomic<int> _pos_base_s{0};  // 当前流的起播偏移 (秒, ?t= 断点续播)
    std::atomic<int> _resume_s{0};    // 已停 (暂停离开/焦点被踢/失败) 后 ▶ 的续播点
    int64_t _pause_since_ms = 0;      // 本次暂停起点 (mooncake 线程独占; 5 分钟自动释放)
    void _startTask();
    void _play(int i);                // 列表点条目: 从头播
    void _startFrom(int t);           // 统一开播入口 (_cur, t 秒起, 按当前输出)
    void _stopPlayback();             // 本机 + 外放全停
    void _releaseIfPaused();          // 暂停中离开 app: 彻底停流释放 Pi 资源, 记断点
    int  _curPosSec() const;          // 当前播放位置 (秒)
    int  _resumePos() const;          // 停流时记的断点 (本机扣 ring 未播部分)
    void _extFinishCheck();           // 外放: 墙钟走到时长 → 播完态 (MA 不回报进度)
    void _togglePause();

    // ── 外放 (relay → Music Assistant → 音箱) ──
    int _output_sel = 0;                      // 0 本机 / 1 客厅 Sonos / 2 起居室 HomePod (NVS yt_out)
    std::atomic<bool> _ext_on{false};         // 当前播放走外放
    std::atomic<int> _ext_dev{-1};            // 已在播的外放设备 (play_on 成功后记)
    std::atomic<uint32_t> _ext_gen{0};        // 作废在途 prepare/play_on
    std::atomic<int64_t> _ext_t0_ms{0};       // 外放进度 = 墙钟 (音频不经本机, 无字节可数)
    std::atomic<int64_t> _ext_pause_acc_ms{0};
    std::atomic<int64_t> _ext_pause_start_ms{0};
    std::atomic<int> _ext_vol{-1};            // MA 音量回读 → onRunning 落 UI
    std::atomic<bool> _ext_vol_wait{false};
    void _extStart();
    void _extStop();
    void _extFail(uint32_t gen, int code);
    void _extVolSync();
    static bool _relayCmd(const char* pid, const char* cmd, int v);
    void _setOutput(int i);
    void _openOutputModal();
    void _closeOutputModal();
    void _refreshOutputUi();
    void _applyVolumeUi();

    // ── 历史 (NVS yt_hist JSON) ──
    void _histLoad();
    void _histSave();
    void _histAdd(const std::string& q);
    void _histDel(const std::string& q);
    void _histRender();
    std::vector<std::string> _hist;

    // ── UI 骨架 ──
    void _buildUi();
    void _buildHomeView();
    void _buildPlayView();
    void _destroyUi();
    void _nullUiPtrs();
    std::vector<void*> _takeImageBufs();   // 摘下全部图片缓冲 (调用方在删屏之后释放)
    void _showView(bool play);
    void _buildRows();
    void _showKeyboard();
    void _hideKeyboard();
    void _fillPlayPage(bool resume = false);   // 按 _cur 填; resume=后台恢复 (不覆盖状态/进度)
    void _fetchCover();
    void _fetchThumbs();                  // 按当前 _items 逐行拉列表缩略图
    void _updatePlayPage();                // 500ms 节流: 状态/进度/时间/按钮
    void _updateMiniBar();                 // 列表页底部迷你播放条 (有内容在播/待续时显示)
    void _refreshPlayBtn();
    lv_obj_t* _scr = nullptr;
    lv_obj_t* _home_view = nullptr;
    lv_obj_t* _play_view = nullptr;
    lv_obj_t* _ta = nullptr;               // 搜索输入 (点按弹软键盘 / 硬键盘 / 语音)
    lv_obj_t* _keyboard = nullptr;         // 屏幕虚拟键盘 (idea 同款, 默认隐藏)
    lv_obj_t* _hist_row = nullptr;
    lv_obj_t* _grid = nullptr;             // 结果列表 (两列卡片, 纵向滚动)
    lv_obj_t* _status_lbl = nullptr;
    lv_obj_t* _out_chip_home = nullptr;    // 输出选择 chip (搜索行右侧)
    lv_obj_t* _out_chip_play = nullptr;    // 输出选择 chip (播放页)
    lv_obj_t* _out_modal = nullptr;
    std::vector<lv_obj_t*> _rows;
    std::vector<void*> _thumb_bufs;        // 行序 rgb565 缓冲 (nullptr=未到)
    std::mutex _thumb_mu;
    std::vector<std::pair<int, void*>> _thumb_ready;   // worker → UI (onRunning 消费)

    // 播放页控件
    lv_obj_t* _play_thumb  = nullptr;      // 640×360 固定框, STRETCH 填充 (小图占位→大图替换不跳版)
    lv_obj_t* _play_title  = nullptr;
    lv_obj_t* _play_sub    = nullptr;
    lv_obj_t* _play_status = nullptr;
    lv_obj_t* _play_bar    = nullptr;
    lv_obj_t* _play_el     = nullptr;      // 已播时长
    lv_obj_t* _play_dur    = nullptr;      // 总时长
    lv_obj_t* _pp_btn      = nullptr;      // 播放/暂停
    lv_obj_t* _pp_lbl      = nullptr;
    lv_obj_t* _vol_slider  = nullptr;
    lv_obj_t* _vol_lbl     = nullptr;
    lv_obj_t* _mini_bar     = nullptr;     // 列表页底部迷你播放条 (点条回播放页)
    lv_obj_t* _mini_pp_lbl  = nullptr;     // ⏸/▶
    lv_obj_t* _mini_title   = nullptr;
    bool _mini_shown = false;              // 变更检测缓存 (onRunning 每帧调, 只在变化时动 LVGL)
    int  _mini_icon = -1;                  // 0=▶ 1=⏸
    std::string _mini_title_txt;
    void* _cover_buf = nullptr;            // 当前显示的大封面 (UI 持有)
    std::mutex _cover_mu;
    void* _cover_ready = nullptr;          // worker → UI
    uint32_t _cover_ready_gen = 0;
    std::atomic<uint32_t> _cover_gen{0};
    int _cur_item = -1;                    // 播放页当前条目
    int64_t _last_ui_ms = 0;

    // ── NVS ──
    static int _nvsOutput();          // yt_out (0 本机 / 1 客厅 / 2 起居室)

    // ── 手势 (上拨退出; games/lottery 同款 + 列表滚动共存) ──
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    int64_t _last_exit_ms = 0;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose() { if (_close_cb) _close_cb(); }
    void _requestExitOnce();
    int _gridScrollY() const { return _grid ? lv_obj_get_scroll_y(_grid) : 0; }

    // ── 硬键盘/语音会话收尾 (失焦 + 关闭 共用; 防 lvKbGroup 悬垂已删 textarea) ──
    void _detachInput();
    int _voice_retry = 0;              // 语音空结果自动重试一次

    std::function<void()> _close_cb;
    std::function<void()> _radio_stopper;
};
