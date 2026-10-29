/*
 * app_ssh — SSH 终端骨架 (Phase 1: libssh 组件链通验证)。
 *
 * Phase 计划: ① libssh 0.11.5 client-only(mbedtls) 进 IDF 组件 ← 本阶段
 * ② SshClient/TerminalEmulator/TerminalBuffer 移植 + 80x24 终端
 * ③ profile(NVS) + 软键盘 + SCP。
 * 本文件: onCreate 做一次 ssh_new/free 探测(开机串口即证库活着),
 * onOpen 显示版本信息卡。
 */
#include "app_ssh.h"
#include "../utils/theme/theme.h"
#include <mooncake_log.h>

#ifndef PLATFORM_BUILD_DESKTOP
#include <libssh/libssh.h>
#include <mbedtls/build_info.h>
#include "../utils/assets/assets_partition.h"
#include <cbin_font.h>

extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_md()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
#endif

static const char* TAG = "ssh";

AppSsh::AppSsh()
{
    setAppInfo().name = "AppSsh";
}

void AppSsh::onCreate()
{
#ifndef PLATFORM_BUILD_DESKTOP
    // 开机探测: 串口出现即证 libssh 组件链通(此阶段的核心验收点)
    ssh_session s = ssh_new();
    bool alloc_ok = s != nullptr;
    ssh_free(s);
    mclog::tagInfo(TAG, "probe: libssh {} ssh_new={} mbedtls {}",
                   ssh_version(0), alloc_ok ? "ok" : "null", MBEDTLS_VERSION_STRING);
#endif
}

void AppSsh::_destroyUi()
{
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _ver_lbl = nullptr;
}

void AppSsh::onOpen()
{
    if (_scr) _destroyUi();   // 工具页模式: _requestClose 只切 app 不拆屏, 幂等收割
    mclog::tagInfo(TAG, "on open");
    const theme::Palette& T = theme::cur();

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_scr);
    lv_label_set_text(title, "SSH 终端");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(T.text_hi), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

#ifndef PLATFORM_BUILD_DESKTOP
    lv_obj_t* ver = lv_label_create(_scr);
    lv_label_set_text_fmt(ver, "libssh %s  ·  mbedtls %s",
                          ssh_version(0), MBEDTLS_VERSION_STRING);
    lv_obj_set_style_text_font(ver, zh_font_md(), 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(T.accent), 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 140);
#endif

    lv_obj_t* hint = lv_label_create(_scr);
    lv_label_set_text(hint, "Phase 1: 库链通验证\n终端功能施工中…");
    lv_obj_set_style_text_font(hint, zh_font_md(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(T.text_dim), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);

    // 手势 alive 旗标成对铁律: onClose 置 false 后 onOpen 必须重建 (b275e71)
    _alive = std::make_shared<bool>(true);
    _installSwipeGesture();
    lv_screen_load(_scr);
    mclog::tagInfo(TAG, "ui-geom: phase1 title=60 ver=140 hint=center");
}

void AppSsh::onRunning() {}

void AppSsh::onClose()
{
    // 安全网 (工具页 app 未必走到, 见 _requestClose)
    mclog::tagInfo(TAG, "on close");
    *_alive = false;
    _removeSwipeGesture();
    _destroyUi();
}

// ── 上拨手势退出 (app_rfid 同款) ────────────────────────────────────────
void AppSsh::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            static lv_point_t s_gpress{0, 0};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSED) {
                    lv_point_t p; lv_indev_get_point(dev, &p);
                    s_gpress = p;
                    return;
                }
                if (code != LV_EVENT_GESTURE) return;
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // 双守卫: 本 app 前台 + 屏幕最下缘起手 (y>650)
                    if (!ctx->app->_scr || lv_screen_active() != ctx->app->_scr) return;
                    if (s_gpress.y <= 650) return;
                    auto* app_ctx = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestClose();
                        delete c;
                    }, app_ctx);
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppSsh::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // _gesture_ctx 故意不释放 — 同 app_rfid: 移除后回调不再触达, 泄漏 16B 可忽略
}

void AppSsh::_requestClose()
{
    // 工具页模式: _close_cb = openApp(工具页), 异步切换, 此刻 _scr 仍是
    // active screen —— 不能在这里删屏, 留给下次 onOpen 顶部的幂等收割。
    if (_close_cb) _close_cb();
}
