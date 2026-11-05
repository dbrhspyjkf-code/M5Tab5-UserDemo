#pragma once
/*
 * app_yt — 油管 (2026-11-06, car-dash 油管面板移植, 音频版)。
 *
 * car-dash 车机页油管面板的 Tab5 设备版: 数据链路同一份 Pi 中继
 * (hermes :8766 的 /yt/ 系端点), 但 P4 无 H.264/AAC 解码器 → **音频播放**:
 *   /yt/search?q=        搜索 (yt-dlp 中继, 服务端缓存 10min)
 *   /yt/thumb/<id>.rgb565  320×180 缩略图 (服务端 PIL 转好, 设备零解码直显)
 *   /yt/pcm/<id>         16k mono s16le 音频流 (收音机 8769 同款消费链:
 *                         esp_http_client 流式读 → ring 128KB → 16k→24k
 *                         线性重采样 → xiaozhi_codec_write 直写)
 *
 * UI 两视图 (car 同款): 搜索/结果列表 ↔ 播放页 (标题/缩略图/进度/停止)。
 * 搜索输入: 硬件键盘 (textarea 入 lvKbGroup) + 语音输入 (idea 同款
 * requestMicInput); 搜索历史 NVS yt_hist (JSON, 点击重搜/长按删除, 上限 10)。
 * 关 app = 停播 (car 同语义, 与收音机后台播相反)。上拨退出回工具页
 * (games/lottery 同款手势)。桌面模拟器: 无流引擎, UI 冒烟可用。
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

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    static constexpr int SCREEN_W = 1280;
    static constexpr int SCREEN_H = 720;
    static constexpr int THUMB_W  = 320;
    static constexpr int THUMB_H  = 180;
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
    static uint32_t C_DANGER(){ return theme::cur().danger; }

    // ── 数据 (worker 写 / UI 读, _mu 保护; worker 绝不碰 LVGL) ──
    void _searchAsync(const std::string& q);
    void _applySearch();             // worker 完成 → 重建结果列表 (onRunning 消费)
    void _thumbWorker(const std::vector<YtItem> items);
    std::mutex _mu;
    std::vector<YtItem> _items;
    std::string _search_err;
    std::atomic<bool> _search_ready{false};
    std::atomic<bool> _search_in_flight{false};
    std::atomic<uint32_t> _thumb_gen{0};          // 新结果作废在途缩略图 worker

    // 播放状态 (worker 写原子量, UI 读)
    std::atomic<int> _want{-1};       // 目标 item 下标, -1=停
    std::atomic<int> _state{0};       // 0 停 / 1 连接中 / 2 播放 / 3 失败
    std::atomic<int64_t> _pcm_bytes{0};
    std::atomic<bool> _app_closing{false};
    std::atomic<bool> _task_started{false};
    void _startTask();
    void _play(int i);                // UI: 播第 i 项
    void _stop();                     // UI: 停 + 回列表

    // ── 历史 (NVS yt_hist JSON) ──
    void _histLoad();
    void _histSave();
    void _histAdd(const std::string& q);
    void _histDel(const std::string& q);
    void _histRender();
    std::vector<std::string> _hist;

    // ── UI 骨架 ──
    void _buildUi();
    void _destroyUi();
    void _nullUiPtrs();
    void _showView(bool play);
    void _buildRows();
    lv_obj_t* _scr = nullptr;
    lv_obj_t* _home_view = nullptr;
    lv_obj_t* _play_view = nullptr;
    lv_obj_t* _ta = nullptr;           // 搜索输入 (硬键盘 lvKbGroup + 语音)
    lv_obj_t* _hist_row = nullptr;
    lv_obj_t* _grid = nullptr;         // 结果列表 (纵向滚动)
    lv_obj_t* _status_lbl = nullptr;   // 搜索中/失败提示
    std::vector<lv_obj_t*> _rows;
    std::vector<void*> _thumb_bufs;    // 行序 rgb565 缓冲 (nullptr=未到)
    std::mutex _thumb_mu;
    std::vector<std::pair<int, void*>> _thumb_ready;   // worker → UI (onRunning 消费)
    int _thumb_pending_row = -1;       // 行点击时缩略图未到 → 显示占位

    // 播放页控件
    lv_obj_t* _play_thumb = nullptr;
    lv_obj_t* _play_title = nullptr;
    lv_obj_t* _play_sub   = nullptr;
    lv_obj_t* _play_bar   = nullptr;
    lv_obj_t* _play_time  = nullptr;
    lv_obj_t* _play_status = nullptr;
    int _cur_item = -1;                // UI 视角当前播放 (切播放页时记)
    int64_t _last_ui_ms = 0;           // 进度刷新 500ms 节流

    // ── 手势 (上拨退出; games/lottery 同款) ──
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    int64_t _last_exit_ms = 0;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose() { if (_close_cb) _close_cb(); }
    void _requestExitOnce();

    std::function<void()> _close_cb;
};
