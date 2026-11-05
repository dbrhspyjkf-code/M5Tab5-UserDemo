#pragma once
#include "../utils/theme/theme.h"
#include <mooncake.h>
#include <lvgl.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>

/**
 * 想法 — 复刻 car-dash 车机页「想法」面板 (录音 memo + 文字灵感 + 灵感时间线),
 * 数据与车机网页同源 (hermes :8766 idea 接口, Pi ~/idea_data/), 跨设备一致。
 *
 * 入口: 主页底部按钮行 (2026-11-05 起替代「计时器」位; 计时器改从工具页进)。
 * 设备版支持录音上传 (wav 64KB 分片, esp_hosted 大 POST 断言规避) 与回放
 * (非 wav 条目走 hermes ?fmt=wav 按需转码); web/桌面构建裁剪录音/回放,
 * 文字 + 时间线照常可用 (httpPost 网关自动带 CSRF 头)。
 */
class AppIdea : public mooncake::AppAbility {
public:
    AppIdea();
    ~AppIdea() override;
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate() override {}
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // ── Layout ────────────────────────────────────────────────────────────
    static constexpr int SCREEN_W = 1280;
    static constexpr int SCREEN_H = 720;
    static constexpr int HEADER_H = 80;
    static constexpr int KB_H     = 340;   // 屏幕虚拟键盘高度 (lora_chat 同款)

    // ── 数据模型 (与 car.html / hermes _idea.py 字段一一对应) ─────────────
    struct IdeaItem {
        std::string id;
        std::string text;      // 文字内容; 语音项 = 备注 (本端不编辑, 恒空)
        std::string cat;       // work / life / idea / todo
        long long created = 0; // epoch ms
        bool voice = false;
        int dur = 0;           // 语音秒数
        std::string mime;      // 语音项存储 mime (audio/wav / audio/webm / audio/mp4)
    };

    // ── 手势 ctx (gallery 同款: PRESSED 记起点+滚动位, 滚动位移>10px 不退出) ──
    struct SwipeCtx {
        AppIdea* app;
        std::weak_ptr<bool> alive;
        int press_x = 0, press_y = 0;
        int scroll_at_press = 0;
    };

    // ── 状态 ──
    std::function<void()> _close_cb;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);

    lv_obj_t* _scr         = nullptr;
    lv_obj_t* _count_lbl   = nullptr;
    lv_obj_t* _ta          = nullptr;
    lv_obj_t* _keyboard    = nullptr;
    lv_obj_t* _mic_btn     = nullptr;
    lv_obj_t* _mic_lbl     = nullptr;
    lv_obj_t* _save_btn    = nullptr;
    lv_obj_t* _stt_btn     = nullptr;
    lv_obj_t* _status_lbl  = nullptr;
    lv_obj_t* _filter_row  = nullptr;
    lv_obj_t* _list_scroll = nullptr;
    lv_obj_t* _list_panel  = nullptr;
    lv_obj_t* _modal       = nullptr;
    lv_obj_t* _play_fill   = nullptr;   // 回放进度条填充 (单实例)

    std::string _filter = "all";
    int _sel_cat = 0;                   // 左栏当前选中分类下标 (CATS)
    std::vector<IdeaItem> _items;
    std::mutex _mu;
    std::atomic<bool> _list_ready{false};
    std::atomic<bool> _fetch_in_flight{false};
    std::atomic<bool> _save_in_flight{false};
    std::atomic<bool> _refresh_needed{false};
    std::atomic<int> _saved_sec{-1};    // >=0: 刚保存完成 (语音 Ns / 文字 0), UI 消费后置回 -1
    std::atomic<bool> _op_failed{false};
    uint32_t _retry_ms = 0;             // 拉取失败后的自动重试时点 (UINT32_MAX=停)

    // 回放进行中的进度条/按钮 label (单实例, 只在 UI 线程读写)
    std::string _play_id;
    int _play_dur = 0;
    lv_obj_t* _play_btn_lbl = nullptr;

    // 手势
    lv_indev_t* _gesture_indev = nullptr;
    void* _gesture_ctx = nullptr;

    // ── 设备专属: 录音 / 回放 (web/桌面构建整体裁掉) ──
    // 管线状态全部文件域静态 (static 成员): 录音攒盘 worker 与上传 lambda
    // 不捕获 this, onClose/析构后仍安全跑完上传 (离开 app 不丢录音);
    // UI 侧在 onRunning 只读这些原子量。
#ifndef PLATFORM_BUILD_DESKTOP
    static constexpr int REC_RATE     = 16000;                       // mic tap 固定 16kHz mono s16
    static constexpr int REC_MAX_SEC  = 60;
    static constexpr size_t REC_CAP   = (size_t)REC_RATE * REC_MAX_SEC * 2;   // 1.92MB PSRAM
    static constexpr size_t UP_CHUNK  = 64 * 1024;                   // esp_hosted 实锤安全档 (双色球 70KB)
    static constexpr size_t PLAY_CAP  = (size_t)REC_RATE * 90 * 2 + 4096;    // 转码 wav 上限 90s
    static constexpr int PLAY_BAR_W   = 470;

    static void s_tap(const int16_t* data, size_t n);   // mic tap 全局单订阅, 静态路由
    static void s_recWorker();                          // 攒 PCM (结束时顺带上传)
    static void s_uploadRun();                          // 录音分片上传 (worker 收尾调用)
    static void s_playWorker(std::string id, std::string mime);

    static AppIdea* s_tap_target;
    static std::mutex s_rec_mu;
    static std::condition_variable s_rec_cv;
    static std::deque<int16_t> s_backlog;               // tap 回调只入队 (audio task 不许阻塞)
    static uint8_t* s_pcm;                              // SPIRAM 线性 PCM, WAV 头上传时再补
    static size_t s_pcm_len;                            // bytes (仅 worker 在 s_rec_mu 内写)
    static std::atomic<bool> s_recording;
    static std::atomic<bool> s_rec_full;                // 缓冲写满 60s, UI 侧自动停
    static std::atomic<bool> s_uploading;
    static std::atomic<int> s_up_part, s_up_parts;
    static std::atomic<bool> s_up_failed;
    static std::atomic<bool> s_play_failed;
    static std::atomic<int> s_saved_sec;                // >=0 刚保存完成 (语音 Ns), UI 消费后置 -1
    static std::atomic<bool> s_need_refresh;
    static uint32_t s_rec_start;
    static std::string s_rec_id, s_rec_cat;
    static uint8_t* s_play_buf;                         // SPIRAM 转码 wav 下载缓冲
    static std::atomic<bool> s_playing;
    static std::atomic<bool> s_play_stop;
    static std::atomic<size_t> s_play_pos;              // 已播 bytes
    static std::atomic<int> s_play_total;               // wav 总 bytes

    void _recStart();
    void _recStop(bool upload);      // detach tap; upload=false = 丢弃
    void _playStart(const std::string& id, int dur, const std::string& mime,
                    lv_obj_t* fill, lv_obj_t* lbl);
    void _playStop();
#endif

    // ── UI / 数据 ──
    void _buildUi();
    void _teardown();
    static std::string _base();
    void _fetch();
    void _renderList();
    void _renderFilter();
    void _saveText();
    void _deleteItem(const std::string& id, bool voice);
    void _clearAll();
    void _setStatus(const char* txt, uint32_t color);
    void _showConfirm(const std::string& msg, const std::string& id,
                      bool voice, bool clear);
    void _showKeyboard();
    void _hideKeyboard();
    uint32_t track_color();   // chip 回调里取主题快照 track 色 (file-static 转发)
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose() { if (_close_cb) _close_cb(); }
};
