#pragma once
/*
 * app_ssh — SSH 终端 (Phase 2: 连接 + PTY shell + 终端渲染 + 键盘输入)。
 *
 * 核心三件套移植自 airpocket-soundman/Tab5_SSH_Client:
 *   ssh_client.h/cpp          — libssh 薄封装 (密码认证/PTY/非阻塞读写)
 *   terminal_emulator.h/cpp   — ANSI/VT 模拟器 (副屏/回滚/宽字符/16色)
 *   terminus_font.h/cpp       — Terminus 点阵 (14x28 → 91 列 x 25 行)
 * 渲染: lv_canvas RGB565 (1274x700), 会话任务在 lvglLock 下直写像素、
 * 脏单元格增量重绘。
 *
 * 输入: 官方键盘面板经 hal raw sink (hal_tab5_keyboard_set_raw_sink)
 * 拿原始 HID 事件 → Ctrl/方向键/F 键映射转义序列 → FreeRTOS 队列 →
 * 会话任务单线程写 SSH 通道 (libssh noop 锁, 读写必须同任务)。
 *
 * UI 状态机: 连接表单(host/user/pass) → 会话屏(全屏终端+右上×断开)。
 * 上拨退出 app = 断开会话。桌面模拟器: 表单可用, 连接报错(libssh 仅真机)。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "ssh_client.h"
#include "terminal_emulator.h"

class AppSsh : public mooncake::AppAbility {
public:
    AppSsh();

    // 工具页 app 标配: close_cb = openApp(AppSettings)
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::function<void()> _close_cb;

    // ── 屏 ──
    lv_obj_t* _scr = nullptr;
    // 连接表单
    lv_obj_t* _form        = nullptr;
    lv_obj_t* _ta_host     = nullptr;
    lv_obj_t* _ta_user     = nullptr;
    lv_obj_t* _ta_pass     = nullptr;
    lv_obj_t* _form_status = nullptr;
    // 会话屏
    lv_obj_t* _canvas   = nullptr;
    void*     _cbuf     = nullptr;   // canvas 像素堆缓冲(malloc, 大块落 PSRAM)
    lv_obj_t* _disc_btn = nullptr;   // 右上 × 断开

    void _buildForm();
    void _destroyUi();

#ifndef PLATFORM_BUILD_DESKTOP
    // ── 终端会话 (仅真机) ──
    std::unique_ptr<TerminalEmulator> _emu;
    SshClient _ssh;
    std::atomic<int>  _sess_state{0};   // 0 表单/空闲 1 连接中 2 会话中 3 失败
    std::atomic<bool> _sess_closing{false};
    std::atomic<bool> _task_running{false};
    SshProfile _profile;                 // 连接参数(按钮点击时填, 任务读)
    char _err[96] = {0};                 // 失败信息(任务写完置 state=3 才读)
    void _buildSession();
    void _teardownSession();
    void _renderTerminal();              // 会话任务调用(lvglLock 内)
    void _drawCell(int col, int row, bool cursor_here);
    void _startTask();
    void _sessionTask();
    void _installKeySink();
    void _removeKeySink();

    static bool _rawKeyCb(uint8_t modifier, uint8_t keycode, bool shift, void* user);
    void _drawImeBar(int len, const char* buf, int syl, int page);
#endif

    // ── 上拨手势退出 (app_rfid 同款, 工具页模式) ──
    struct SwipeCtx {
        AppSsh* app;
        std::weak_ptr<bool> alive;
    };
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void* _gesture_ctx = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose();

#ifndef PLATFORM_BUILD_DESKTOP
    // 表单事件(仅真机有连接动作)
    static void _connect_cb(lv_event_t* e);
    // 会话屏事件
    static void _disc_cb(lv_event_t* e);
#endif

    // 表单事件(两平台共用)
    static void _focus_ta_cb(lv_event_t* e);
};
