#pragma once
/*
 * app_ssh — SSH 终端 (Phase 1: libssh 0.11.5 client-only 组件验证)。
 *
 * 移植自 airpocket-soundman/Tab5_SSH_Client 的核心三件套计划:
 * SshClient(libssh 薄封装) + TerminalEmulator(ANSI/VT) + TerminalBuffer
 * (回滚缓冲), Arduino String → std::string, 壳换 mooncake + LVGL。
 * 本阶段只做库链通 + 骨架屏: onCreate 探测 libssh(开机串口可见),
 * 屏上显示版本号。终端/连接/软键盘在后续阶段补。
 *
 * 输入: 官方键盘面板经 hal_tab5_keyboard(hal 已把按键喂进 LVGL 键盘组,
 * 插上即用, 无需本 app 做任何事); 无面板时用屏幕软键盘兜底(后续阶段)。
 * 工具页入口 app: 上拨 = openApp(工具页), 同 rfid/stocks 模式 —— 注意与
 * 主页系 app(radio 等)的 close() 模式区分。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>

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
    lv_obj_t* _scr    = nullptr;
    lv_obj_t* _ver_lbl = nullptr;   // libssh/mbedtls 版本行
    void _destroyUi();

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
};
