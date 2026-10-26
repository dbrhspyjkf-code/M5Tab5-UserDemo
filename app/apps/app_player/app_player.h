#pragma once
/*
 * app_player — Sonos/HomePod 播放器(参考 M5DIAL app_sonos 移植到 Tab5 触摸大屏)。
 *
 * 控制走 Home Assistant media_player 服务(play_pause/next/previous/volume_set),
 * 复用 app_ha 的 HaClient(独立实例,4s 轮询,随 app 开关创建/销毁):
 *   - 客厅 SONOS  = media_player.ke_ting_2(Music Assistant 实体;原生 Sonos
 *     集成已从 HA 消失,MA 实体既是状态源也是控制目标 —— M5DIAL 2026-10-22 结论)
 *   - 起居室 HOMEPOD = media_player.qi_ju_shi_2(MA 的 RAOP/AirPlay1 送不了
 *     AirPlay2 音频,必须控制 HomePod 自身实体 —— 同上结论)
 *
 * UI(1280×720 触摸): 顶部页签二选一 + 大字 now-playing + 上一曲/播放暂停/
 * 下一曲 触摸键 + 音量滑条;上拨手势退出(app_radio 同款)。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>
#include <string>
#include "../app_ha/ha_client.h"

class AppPlayer : public mooncake::AppAbility {
public:
    AppPlayer();

    // 关闭回调(app_installer 注入: 回主屏)
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::function<void()> _close_cb;

    // ── 页配置(Sonos/HomePod 双页) ──
    struct PageCfg {
        const char* tab;       // 页签名
        const char* entity;    // HA media_player 实体
    };
    static constexpr int kPageCount = 2;
    static const PageCfg _pages[kPageCount];
    int _page = 0;   // 当前页

    // ── HA(随开随关的独立客户端) ──
    std::shared_ptr<HaClient> _ha;

    // 当前页状态缓存(仅 LVGL 线程读写)
    std::string _title, _artist, _state;
    int  _volume   = -1;    // 0..100;-1 未知
    bool _has_data = false;

    // ── UI ──
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _tabs[kPageCount] = {};   // 已弃用(左右拨动切页); 保留数组免改引用
    lv_obj_t* _page_lbl  = nullptr;
    lv_obj_t* _hint_lbl  = nullptr;   // 底部提示: 触摸后 3s 内显示   // 顶行居中的当前源名
    lv_obj_t* _state_chip = nullptr;   // 播放中/暂停/空闲/离线
    lv_obj_t* _title_lbl  = nullptr;
    lv_obj_t* _artist_lbl = nullptr;
    lv_obj_t* _play_lbl   = nullptr;   // 播放键内容(▶/⏸)
    lv_obj_t* _vol_slider = nullptr;
    lv_obj_t* _vol_lbl    = nullptr;
    bool _slider_sync = false;         // 程序设值时屏蔽回调

    // ── 播放进度条(seek) ──
    lv_obj_t* _prog_slider = nullptr;
    lv_obj_t* _prog_lbl    = nullptr;  // 已播时间(条左侧)
    lv_obj_t* _prog_total  = nullptr;  // 总时长(条右侧)
    bool _prog_sync = false;           // 程序设值时屏蔽回调
    int _pos = -1, _dur = -1;          // 秒; -1 未知(无时长则整条隐藏)
    uint32_t _pos_ms = 0;              // _pos 的本地时钟戳(播放中自行走秒)
    static void _prog_cb(lv_event_t* e);
    void _advancePos();                // 播放中本地走 1 秒

    // ── 歌词(LRCLIB 经 OrangePi 8769 代理; LRC 时间轴与进度共用 _pos) ──
    lv_obj_t* _lyr_now  = nullptr;    // 当前行(亮)
    lv_obj_t* _lyr_next = nullptr;    // 下一行(暗)
    std::vector<float> _lyr_t;        // 行起始秒
    std::vector<std::string> _lyr_l;
    std::mutex _lyr_mu;
    std::atomic<bool> _lyr_fetching{false};
    std::string _lyr_key;             // 当前展示歌词的 key(artist|title)
    std::string _lyr_want_title, _lyr_want_artist;   // 最新期望(在途任务收尾捡起)
    int _lyr_want_dur = 0;
    int _lyr_idx = -1;                // 当前行号
    void _startLyricsFetch(const std::string& title, const std::string& artist, int duration);
    void _updateLyricLine();

    // ── 大封面(hermes /assets/cover.rgb565,OrangePi PIL 转码零设备解码) ──
    static constexpr int _COVER = 460;              // px(半屏; PSRAM ~820KB; 服务端 ?size= 任意)
    lv_obj_t* _cover_img  = nullptr;                // lv_image 对象
    lv_obj_t* _cover_card = nullptr;                // 占位暗卡
    lv_image_dsc_t* _cover_dsc = nullptr;           // PSRAM: dsc + 像素一体分配
    void*     _cover_buf  = nullptr;
    std::string _art_key;
    uint32_t _art_retry_ms = 0;      // 拉取失败后的重试时间点(10s 退避)             // entity_picture URL(变化才重拉)
    std::atomic<bool> _art_fetching{false};
    std::atomic<bool> _art_ready{false};            // 拉取完成待上屏(LVGL 线程消费)
    void _startArtFetch();           // detached 线程拉当前页封面
    static void _artFetchTask(AppPlayer* self);

    void _switchPage(int idx);
    void _refreshUi();                 // 缓存→UI(全量)
    void _pollEntity();                // HA→缓存(onRunning 周期调)
    void _sendCmd(const char* service, const char* extra_json = "");

    static void _tabClick_cb(lv_event_t* e);
    static void _prevClick_cb(lv_event_t* e);
    static void _playClick_cb(lv_event_t* e);
    static void _nextClick_cb(lv_event_t* e);
    static void _vol_cb(lv_event_t* e);

    // ── 上拨手势退出(app_radio/app_stocks 同款) ──
    struct SwipeCtx {
        AppPlayer* app;
        std::weak_ptr<bool> alive;
    };
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void* _gesture_ctx = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose();
};
