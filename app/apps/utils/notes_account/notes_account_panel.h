#pragma once
/*
 * NotesAccountPanel — ZERO 想法云 (noteserver) 账户面板 (2026-11-20)。
 *
 * 原 AppIdea 专属的「想法云账户」弹层 (TOKEN 登录 / 扫码登录 / 注册 / 退出)
 * 抽成可复用面板: 现在挂在主页齿轮「设置」弹窗的「账户」Tab, AppIdea 只保留
 * 数据面调用 (token()/user()/auth_header())。
 *
 * 用法:
 *   NotesAccountPanel panel;              // 宿主持有 (file-static 即可)
 *   panel.set_on_login_changed(cb);       // 可选: 登录态变化通知 (刷新宿主 UI)
 *   panel.open(container);                // parent = 宿主内容容器 (全宽 680×~440)
 *   panel.close();                        // 宿主弹窗关闭前必须调 (拆键盘+扫码层)
 *
 * 协议 (与 ZERO oneapp 同一套 noteserver):
 *   - TOKEN 登录:  GET  /v1/ping (Bearer) 回显用户名
 *   - 扫码登录:    POST /v1/auth/device/start {"name":"tab5"} → 二维码 (服务端
 *     原样 URL, 微信 webview 拒 IP 形态链接) → 2s 轮询 /v1/auth/device/poll,
 *     approved 一次性收下 device token (永不过期)
 *   - 注册:        POST /v1/register {"name","invite"} (注册即登录)
 *   - 凭证:        NVS notes_token / notes_user
 *
 * 平台: QR 部分包 #ifndef PLATFORM_BUILD_DESKTOP (web/桌面无 LV_USE_QRCODE);
 * TOKEN/注册全平台可用。header-only, 三平台 CMake GLOB 自动收。
 */
#include <hal/hal.h>
#include <mooncake_log.h>
#include <lvgl.h>
#include <nlohmann/json.hpp>
#include "../psram_json.h"
#include "../theme/theme.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ── 中文字体 (各 TU file-static, 与 app_home/app_idea 同款三平台分支) ──────
#if defined(PLATFORM_BUILD_DESKTOP) && !defined(PLATFORM_BUILD_WEB)
#include "../../../../platforms/tab5/managed_components/78__xiaozhi-fonts/include/cbin_font.h"
extern "C" const lv_font_t font_puhui_20_4;
namespace na_font {
inline const lv_font_t* lg() { return &font_puhui_20_4; }
inline const lv_font_t* md() { return &font_puhui_20_4; }
inline const lv_font_t* sm() { return &font_puhui_20_4; }
}  // namespace na_font
#elif defined(PLATFORM_BUILD_WEB)
#include "web_fonts.h"
namespace na_font {
inline const lv_font_t* lg() { return webui::font_30(); }
inline const lv_font_t* md() { return webui::font_24(); }
inline const lv_font_t* sm() { return webui::font_20(); }
}  // namespace na_font
#else
#include <cbin_font.h>
#include "../assets/assets_partition.h"   // assets_part::find (res 分区字体, 仅设备分支引用)
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
namespace na_font {
// 分层与主页一致: lg=内嵌 30px, md=res font_zh_24, sm=res font_zh_20,
// res 异常回退 30px (勿合并档位, app_home 同款注释)
inline const lv_font_t* lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
inline const lv_font_t* md()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f ? f : lg();
}
inline const lv_font_t* sm()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f ? f : lg();
}
}  // namespace na_font
#endif

class NotesAccountPanel {
public:
    static constexpr const char* BASE = "http://8.134.111.17:8585";

    // ── 永不抛 JSON 取值 (noteserver "ok":1 数字坑, nlohmann .value() 会 throw
    //    → worker 无人接 = abort 蓝屏; find+类型分派, bool 认 true/1/"1"/"true")
    static bool jbool(const PsramJson& j, const char* k, bool dflt = false)
    {
        if (!j.is_object()) return dflt;
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return dflt;
        if (it->is_boolean()) return it->get<bool>();
        if (it->is_number_integer()) return it->get<long long>() != 0;
        if (it->is_string()) {
            auto s = it->get<std::string>();
            return s == "1" || s == "true";
        }
        return dflt;
    }
    static std::string jstr(const PsramJson& j, const char* k, const char* dflt = "")
    {
        if (!j.is_object()) return dflt;
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return dflt;
        if (it->is_string()) return it->get<std::string>();
        if (it->is_number_integer()) return std::to_string(it->get<long long>());
        if (it->is_number_float()) {
            char b[32];
            snprintf(b, sizeof(b), "%g", it->get<double>());
            return b;
        }
        return dflt;
    }
    static double jnum(const PsramJson& j, const char* k, double dflt = 0)
    {
        if (!j.is_object()) return dflt;
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return dflt;
        if (it->is_number()) return it->get<double>();
        return dflt;
    }

    // ── 凭证 (NVS notes_token/notes_user) ──
    static std::string token()
    {
        return GetHAL()->getConfig("notes_token", "");
    }
    static std::string user()
    {
        return GetHAL()->getConfig("notes_user", "");
    }
    static std::vector<std::pair<std::string, std::string>> auth_header()
    {
        std::vector<std::pair<std::string, std::string>> h;
        auto t = token();
        if (!t.empty()) h.push_back({"Authorization", "Bearer " + t});
        return h;
    }

    void set_on_login_changed(std::function<void()> cb) { _on_login = std::move(cb); }
    bool is_open() const { return _root != nullptr; }

    // ── 打开面板 (parent = 宿主内容容器, 期望全宽 ~680×440) ──
    void open(lv_obj_t* parent)
    {
        if (_root) close();                            // 幂等重开
        _alive = std::make_shared<bool>(true);         // 成对铁律: close 置 false 后重建
        _root = parent;

        auto mk_lbl = [&](const char* txt, int x, int y, uint32_t color,
                          const lv_font_t* font, lv_obj_t** out = nullptr) {
            lv_obj_t* l = lv_label_create(_root);
            lv_label_set_text(l, txt);
            lv_obj_set_pos(l, x, y);
            lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
            lv_obj_set_style_text_font(l, font, 0);
            if (out) *out = l;
            return l;
        };
        auto mk_ta = [&](int x, int y, int w, const char* ph) {
            lv_obj_t* ta = lv_textarea_create(_root);
            lv_obj_set_size(ta, w, 52);
            lv_obj_set_pos(ta, x, y);
            lv_obj_set_style_text_color(ta, lv_color_hex(theme::cur().text_hi), 0);  // 缺省黑字暗卡不可读
            lv_textarea_set_one_line(ta, true);
            lv_textarea_set_placeholder_text(ta, ph);
            lv_obj_set_style_text_font(ta, na_font::md(), 0);
            lv_obj_set_style_bg_color(ta, lv_color_hex(theme::cur().card[1]), 0);
            lv_obj_set_style_border_width(ta, 0, 0);
            lv_obj_set_style_radius(ta, 12, 0);
            lv_obj_add_event_cb(ta, [](lv_event_t* e) {
                auto* self = static_cast<NotesAccountPanel*>(lv_event_get_user_data(e));
                if (!self->_kb || GetHAL()->usbKeyboardDetect()) return;
                lv_keyboard_set_textarea(self->_kb, static_cast<lv_obj_t*>(lv_event_get_target(e)));
                lv_obj_clear_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(self->_kb);
            }, LV_EVENT_FOCUSED, this);
            return ta;
        };
        auto mk_btn = [&](const char* txt, int x, int y, int w, int h, uint32_t bg,
                          lv_event_cb_t cb) {
            lv_obj_t* b = lv_button_create(_root);
            lv_obj_set_size(b, w, h);
            lv_obj_set_pos(b, x, y);
            lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
            lv_obj_set_style_radius(b, 14, 0);
            lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, na_font::md(), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(theme::ink_on(bg)), 0);
            lv_obj_center(l);
            return b;
        };

        _stat_user = mk_lbl("", 40, 12, theme::cur().text_soft, na_font::lg());
#ifndef PLATFORM_BUILD_DESKTOP
        mk_btn("扫码登录", 40, 64, 300, 54, theme::cur().accent,
            [](lv_event_t* e) {
                static_cast<NotesAccountPanel*>(lv_event_get_user_data(e))->qr_open();
            });
#endif
        lv_obj_t* qh = mk_lbl("手机扫码打开授权页登录, 确认后自动登录\n"
                              "家里 WiFi 拦 apisz.top 域名, 扫码请关 WiFi 用蜂窝",
                              364, 66, theme::cur().text_dim, na_font::sm());
        lv_obj_set_width(qh, 280);
        lv_label_set_long_mode(qh, LV_LABEL_LONG_WRAP);

        mk_lbl("TOKEN (ZERO 设置生成 / 登录页同值)", 40, 142,
               theme::cur().text_dim, na_font::sm());
        _ta_tok = mk_ta(40, 168, 420, "粘贴 TOKEN");
        mk_btn("登录", 480, 168, 160, 52, theme::cur().accent,
            [](lv_event_t* e) {
                static_cast<NotesAccountPanel*>(lv_event_get_user_data(e))->login();
            });

        mk_lbl("注册新账号 (邀请码在 ZERO 设置·生成注册码)", 40, 236,
               theme::cur().text_dim, na_font::sm());
        _ta_name = mk_ta(40, 262, 190, "用户名");
        _ta_inv = mk_ta(246, 262, 190, "邀请码");
        mk_btn("注册", 452, 262, 188, 52, theme::cur().accent,
            [](lv_event_t* e) {
                static_cast<NotesAccountPanel*>(lv_event_get_user_data(e))->reg();
            });

        _btn_logout = mk_btn("退出登录", 40, 334, 160, 50, theme::cur().danger,
            [](lv_event_t* e) {
                static_cast<NotesAccountPanel*>(lv_event_get_user_data(e))->logout();
            });

        _stat = mk_lbl("", 40, 400, theme::cur().text_dim, na_font::md());

        // 软键盘挂 layer_top (容器在宿主卡片里, 键盘盖下方区域)
        _kb = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(_kb, 680, 240);
        lv_obj_align(_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(_kb, LV_OBJ_FLAG_HIDDEN);
        // ✓(READY)/✗(CANCEL) 收键盘 — 不挂则键盘永不隐藏盖住按钮 (app_idea 实锤)
        lv_obj_add_event_cb(_kb, [](lv_event_t* e) {
            auto* self = static_cast<NotesAccountPanel*>(lv_event_get_user_data(e));
            if (self->_kb) lv_obj_add_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
        }, LV_EVENT_READY, this);
        lv_obj_add_event_cb(_kb, [](lv_event_t* e) {
            auto* self = static_cast<NotesAccountPanel*>(lv_event_get_user_data(e));
            if (self->_kb) lv_obj_add_flag(self->_kb, LV_OBJ_FLAG_HIDDEN);
        }, LV_EVENT_CANCEL, this);

        // 结果消费 timer (登录/注册/扫码 worker 全走原子量, 这里统一 UI 线程落)
        _ui_timer = lv_timer_create(_uiTimerCb, 120, this);
        refresh();
    }

    // ── 关闭: 宿主弹窗销毁前必须调 (键盘/扫码层/计时器都挂 layer_top, 不随宿主删) ──
    void close()
    {
        if (!_root && !_qr_overlay && !_kb) return;
        _alive = std::make_shared<bool>(false);        // 作废在途 worker 回调
        if (_ui_timer) { lv_timer_del(_ui_timer); _ui_timer = nullptr; }
#ifndef PLATFORM_BUILD_DESKTOP
        qr_teardown();                                 // 扫码层/计时器挂 layer_top, 必须先拆
#endif
        if (_kb) { lv_obj_delete(_kb); _kb = nullptr; }
        _root = nullptr;
        _stat_user = _btn_logout = _stat = nullptr;
        _ta_tok = _ta_name = _ta_inv = nullptr;
        _qr_close_ms = 0;
        _busy = false;
        {
            std::lock_guard<std::mutex> lk(_mu);
            _qr_dc.clear();
            _qr_url.clear();
            _qr_msg.clear();
        }
        _na_result = -1;
        _qr_evt = -1;
    }

private:
    // ── 状态行/退出按钮刷新 ──
    void refresh()
    {
        if (!_stat_user) return;
        auto t = token();
        auto u = user();
        lv_label_set_text(_stat_user, t.empty() ? "未登录" : ("已登录 @" + u).c_str());
        lv_obj_set_style_text_color(_stat_user,
            lv_color_hex(t.empty() ? theme::cur().text_soft : theme::cur().ok), 0);
        if (_btn_logout)
            t.empty() ? lv_obj_add_flag(_btn_logout, LV_OBJ_FLAG_HIDDEN)
                      : lv_obj_clear_flag(_btn_logout, LV_OBJ_FLAG_HIDDEN);
    }

    // ── TOKEN 登录 (GET /v1/ping 验证) ──
    void login()
    {
        if (!_root) return;
        const char* t = lv_textarea_get_text(_ta_tok);
        if (!t || !t[0]) { set_stat("先粘贴 TOKEN"); return; }
        if (_busy.exchange(true)) return;
        set_stat("验证中…");
        auto alive = _alive;
        std::string tok = t;
        GetHAL()->tryRunDetached([this, alive, tok]() {
            auto resp = GetHAL()->httpGet(std::string(BASE) + "/v1/ping",
                                          {{"Authorization", "Bearer " + tok}});
            bool ok = false;
            std::string user;
            auto j = PsramJson::parse(resp.body, nullptr, false);
            if (!j.is_discarded()) {
                ok = jbool(j, "ok");
                user = jstr(j, "user");
            }
            if (ok) {
                GetHAL()->setConfig("notes_token", tok);
                GetHAL()->setConfig("notes_user", user);
                _login_kind = 1;                       // 1=登录 (区分注册文案)
            }
            if (*alive) _na_result = ok ? 1 : 0;
            _busy = false;
        }, 16 * 1024);
    }

    // ── 邀请码注册 (POST /v1/register, 注册即登录) ──
    void reg()
    {
        if (!_root) return;
        const char* nm = lv_textarea_get_text(_ta_name);
        const char* iv = lv_textarea_get_text(_ta_inv);
        if (!nm || !nm[0] || !iv || !iv[0]) { set_stat("用户名和邀请码都要填"); return; }
        if (_busy.exchange(true)) return;
        set_stat("注册中…");
        auto alive = _alive;
        std::string name = nm, inv = iv;
        GetHAL()->tryRunDetached([this, alive, name, inv]() {
            std::string body = nlohmann::json{
                {"name", name}, {"invite", inv}}.dump();
            auto resp = GetHAL()->httpPost(std::string(BASE) + "/v1/register",
                body, {{"Content-Type", "application/json"}});
            bool ok = false;
            std::string tok, user;
            auto j = PsramJson::parse(resp.body, nullptr, false);
            if (!j.is_discarded()) {
                ok = jbool(j, "ok");
                tok = jstr(j, "token");
                user = jstr(j, "user");
            }
            if (ok) {
                GetHAL()->setConfig("notes_token", tok);
                GetHAL()->setConfig("notes_user", user);
                _login_kind = 2;                       // 2=注册
            }
            if (*alive) _na_result = ok ? 1 : 0;
            _busy = false;
        }, 16 * 1024);
    }

    void logout()
    {
        GetHAL()->setConfig("notes_token", "");
        GetHAL()->setConfig("notes_user", "");
        set_stat("已退出 · 想法不再同步");
        refresh();
        if (_on_login) _on_login();
    }

    void set_stat(const char* txt)
    {
        if (_stat) lv_label_set_text(_stat, txt);
    }

    // ── 结果消费 (120ms UI timer; worker 只写原子量) ──
    static void _uiTimerCb(lv_timer_t* t)
    {
        auto* self = static_cast<NotesAccountPanel*>(lv_timer_get_user_data(t));

        int nr = self->_na_result.exchange(-1);        // 登录/注册结果
        if (nr >= 0) {
            if (self->_root) {
                self->set_stat(nr == 1
                    ? (self->_login_kind == 2 ? "注册成功 · 已连接" : "成功 · 已连接")
                    : "失败 (TOKEN/邀请码无效或网络)");
                if (nr == 1) {                         // 输入框清密 + 刷新状态
                    if (self->_ta_tok) lv_textarea_set_text(self->_ta_tok, "");
                    if (self->_ta_name) lv_textarea_set_text(self->_ta_name, "");
                    if (self->_ta_inv) lv_textarea_set_text(self->_ta_inv, "");
                    self->refresh();
                    if (self->_on_login) self->_on_login();
                }
            }
        }

#ifndef PLATFORM_BUILD_DESKTOP
        int qe = self->_qr_evt.exchange(-1);           // 扫码 worker 结果
        if (qe >= 0) {
            std::string msg;
            {
                std::lock_guard<std::mutex> lk(self->_mu);
                msg = self->_qr_msg;
            }
            if (qe == 1) {                             // 出码就绪 → 画 QR
                std::string url;
                {
                    std::lock_guard<std::mutex> lk(self->_mu);
                    url = self->_qr_url;
                }
                if (self->_qr_obj && !url.empty()) {
                    lv_qrcode_update(self->_qr_obj, url.data(), (uint32_t)url.size());
                    lv_obj_clear_flag(self->_qr_obj, LV_OBJ_FLAG_HIDDEN);
                }
                if (self->_qr_stat)
                    lv_label_set_text(self->_qr_stat, "手机扫码, 授权页登录后自动登录");
            } else if (qe == 2) {
                if (self->_qr_stat) lv_label_set_text(self->_qr_stat, msg.c_str());
            } else if (qe == 3) {                      // 授权成功
                if (self->_qr_stat) {
                    lv_label_set_text(self->_qr_stat, msg.c_str());
                    lv_obj_set_style_text_color(self->_qr_stat,
                                                lv_color_hex(theme::cur().ok), 0);
                }
                self->set_stat(msg.c_str());
                self->refresh();
                if (self->_ta_tok) lv_textarea_set_text(self->_ta_tok, "");
                if (self->_ta_name) lv_textarea_set_text(self->_ta_name, "");
                if (self->_ta_inv) lv_textarea_set_text(self->_ta_inv, "");
                if (self->_on_login) self->_on_login();
                self->_qr_close_ms = GetHAL()->millis() + 1400;   // 停留片刻自动回账户层
            } else if (qe == 4) {                      // 过期/被领走 → 文案停留片刻再自动重出
                if (self->_qr_stat) lv_label_set_text(self->_qr_stat, msg.c_str());
                self->_qr_poll_ms = GetHAL()->millis() + 1600;
            } else if (qe == 5) {
                if (self->_qr_stat) lv_label_set_text(self->_qr_stat, msg.c_str());
                self->_qr_poll_ms = GetHAL()->millis() + 3000;
            }
        }
#endif
    }

#ifndef PLATFORM_BUILD_DESKTOP
    // ── 扫码层 (覆盖层挂 layer_top 全屏, 不占宿主容器) ──
    void qr_open()
    {
        if (!_root) return;
        qr_teardown();                                 // 幂等重开
        _alive = std::make_shared<bool>(true);         // qr_teardown 曾置 false, 重建
        _qr_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(_qr_overlay, 1280, 720);
        lv_obj_set_pos(_qr_overlay, 0, 0);
        lv_obj_set_style_bg_color(_qr_overlay, lv_color_hex(theme::cur().card[0]), 0);
        lv_obj_set_style_bg_opa(_qr_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(_qr_overlay, 0, 0);
        lv_obj_set_style_border_width(_qr_overlay, 0, 0);
        lv_obj_clear_flag(_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_qr_overlay, LV_OBJ_FLAG_CLICKABLE);    // 吃点击不透传

        auto* ttl = lv_label_create(_qr_overlay);
        lv_label_set_text(ttl, "扫码登录");
        lv_obj_set_style_text_font(ttl, na_font::lg(), 0);
        lv_obj_set_style_text_color(ttl, lv_color_hex(theme::cur().accent), 0);
        lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 20);

        auto* hint = lv_label_create(_qr_overlay);
        lv_label_set_text(hint, "手机扫码打开授权页登录, 确认后本机自动登录\n"
                                "家里 WiFi 拦 apisz.top 域名, 扫码请关 WiFi 用蜂窝");
        lv_obj_set_style_text_font(hint, na_font::md(), 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 78);

        _qr_obj = lv_qrcode_create(_qr_overlay);
        lv_qrcode_set_size(_qr_obj, 420);
        lv_qrcode_set_dark_color(_qr_obj, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(_qr_obj, lv_color_hex(0xFFFFFF));
        lv_obj_align(_qr_obj, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(_qr_obj, LV_OBJ_FLAG_HIDDEN);           // 出码成功再显示

        _qr_stat = lv_label_create(_qr_overlay);
        lv_label_set_text(_qr_stat, "出码中…");
        lv_obj_set_style_text_font(_qr_stat, na_font::md(), 0);
        lv_obj_set_style_text_color(_qr_stat, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_align(_qr_stat, LV_ALIGN_BOTTOM_MID, 0, -40);

        auto* back = lv_button_create(_qr_overlay);
        lv_obj_set_size(back, 130, 58);
        lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_radius(back, 14, 0);
        lv_obj_add_event_cb(back, [](lv_event_t* e) {
            static_cast<NotesAccountPanel*>(lv_event_get_user_data(e))->qr_teardown();
        }, LV_EVENT_CLICKED, this);
        auto* lblb = lv_label_create(back);
        lv_label_set_text(lblb, "返回");
        lv_obj_set_style_text_font(lblb, na_font::md(), 0);
        lv_obj_center(lblb);

        _qr_timer = lv_timer_create(_qrTimerCb, 500, this);
        _qr_poll_ms = 0;
        _qr_close_ms = 0;
        qr_start();
    }

    // 拆扫码层: 计时器必须先删 (回调引用 overlay), 再删对象
    void qr_teardown()
    {
        if (_qr_timer) { lv_timer_del(_qr_timer); _qr_timer = nullptr; }
        _qr_close_ms = 0;
        _busy = false;                         // 在途 worker 收尾只写原子, 结果无人消费
        if (_qr_overlay) { lv_obj_delete(_qr_overlay); _qr_overlay = nullptr; }
        _qr_obj = nullptr;
        _qr_stat = nullptr;
        {
            std::lock_guard<std::mutex> lk(_mu);
            _qr_dc.clear();
            _qr_url.clear();
            _qr_msg.clear();
        }
        _qr_evt = -1;
    }

    // 出码: POST /v1/auth/device/start {"name":"tab5"}
    void qr_start()
    {
        if (!_qr_overlay || _busy.exchange(true)) return;
        lv_label_set_text(_qr_stat, "出码中…");
        lv_obj_add_flag(_qr_obj, LV_OBJ_FLAG_HIDDEN);
        auto alive = _alive;
        GetHAL()->tryRunDetached([this, alive]() {
            std::string body = nlohmann::json{{"name", "tab5"}}.dump();
            auto resp = GetHAL()->httpPost(std::string(BASE) + "/v1/auth/device/start",
                body, {{"Content-Type", "application/json"}});
            bool ok = false;
            std::string dc;
            auto j = PsramJson::parse(resp.body, nullptr, false);
            if (!j.is_discarded()) {
                ok = jbool(j, "ok");
                dc = jstr(j, "device_code");
            }
            bool got = ok && dc.size() == 32;          // 服务端 device_code 固定 32 hex
            {
                std::lock_guard<std::mutex> lk(_mu);
                if (got) {
                    _qr_dc = dc;
                    // 二维码用服务端原样 URL (与 ZERO oneapp 逐字节一致): 微信
                    // webview 拒开 IP 形态链接; 家宽 ISP 对 apisz.top 域名全系
                    // 拦截, 手机蜂窝网络下正常 (家里扫码切蜂窝)
                    auto u = jstr(j, "url");
                    _qr_url = u.find("http") == 0 ? u
                                                  : std::string(BASE) + "/login#dc=" + dc;
                } else {
                    _qr_dc.clear();
                    _qr_url.clear();
                    _qr_msg = "出码失败, 稍后自动重试";
                }
            }
            if (*alive) _qr_evt = got ? 1 : 5;
            _busy = false;
        }, 16 * 1024);
    }

    // 轮询: POST /v1/auth/device/poll, approved 一次性收 token
    void qr_poll_once()
    {
        if (!_qr_overlay || _busy.exchange(true)) return;
        std::string dc;
        {
            std::lock_guard<std::mutex> lk(_mu);
            dc = _qr_dc;
        }
        if (dc.empty()) { _busy = false; return; }
        auto alive = _alive;
        GetHAL()->tryRunDetached([this, alive, dc]() {
            std::string body = nlohmann::json{{"device_code", dc}}.dump();
            auto resp = GetHAL()->httpPost(std::string(BASE) + "/v1/auth/device/poll",
                body, {{"Content-Type", "application/json"}});
            auto j = PsramJson::parse(resp.body, nullptr, false);
            std::string st = j.is_discarded() ? "" : jstr(j, "status");
            if (st == "approved") {
                std::string tok = jstr(j, "token");    // 一次性发放, 此刻必须收下
                std::string user = jstr(j, "user");
                if (!tok.empty()) {
                    GetHAL()->setConfig("notes_token", tok);
                    GetHAL()->setConfig("notes_user", user);
                    {
                        std::lock_guard<std::mutex> lk(_mu);
                        _qr_msg = "登录成功 @" + (user.empty() ? "?" : user);
                    }
                    if (*alive) _qr_evt = 3;
                }
            } else if (st == "pending") {
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    _qr_msg = "等待手机授权确认…";
                }
                if (*alive) _qr_evt = 2;
            } else if (st == "none" || st == "done") { // 过期/被领走 → 重出码
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    _qr_dc.clear();
                    _qr_url.clear();
                    _qr_msg = (st == "done") ? "授权已被领取, 重新出码…"
                                             : "二维码已过期, 重新出码…";
                }
                if (*alive) _qr_evt = 4;
            }
            // 其他 status / 网络失败: 静默, 下轮 timer 再试
            _busy = false;
        }, 16 * 1024);
    }

    // 500ms 调度: 成功自动收层 + 2s 节流轮询/重出码
    static void _qrTimerCb(lv_timer_t* t)
    {
        auto* self = static_cast<NotesAccountPanel*>(lv_timer_get_user_data(t));
        if (!self->_qr_overlay) return;
        uint32_t now = GetHAL()->millis();
        if (self->_qr_close_ms && (int32_t)(now - self->_qr_close_ms) >= 0) {
            self->qr_teardown();                       // 成功后停留片刻自动回账户层
            return;
        }
        if (self->_busy.load()) return;
        if ((int32_t)(now - self->_qr_poll_ms) < 0) return;
        self->_qr_poll_ms = now + 2000;                // 与 oneapp 同款 2s 节流
        bool has_dc;
        {
            std::lock_guard<std::mutex> lk(self->_mu);
            has_dc = !self->_qr_dc.empty();
        }
        if (has_dc) self->qr_poll_once();
        else self->qr_start();                         // 无码: 出码/重试
    }
#endif  // PLATFORM_BUILD_DESKTOP

    // ── UI (面板本身在宿主容器内; 键盘/扫码层挂 layer_top) ──
    lv_obj_t* _root = nullptr;
    lv_obj_t* _stat_user = nullptr;
    lv_obj_t* _ta_tok = nullptr;
    lv_obj_t* _ta_name = nullptr;
    lv_obj_t* _ta_inv = nullptr;
    lv_obj_t* _btn_logout = nullptr;
    lv_obj_t* _stat = nullptr;
    lv_obj_t* _kb = nullptr;

    // 扫码 (web/桌面恒空)
    lv_obj_t* _qr_overlay = nullptr;
    lv_obj_t* _qr_obj = nullptr;
    lv_obj_t* _qr_stat = nullptr;
    lv_timer_t* _qr_timer = nullptr;
    lv_timer_t* _ui_timer = nullptr;

    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::atomic<bool> _busy{false};
    std::atomic<int> _na_result{-1};       // 登录/注册: -1 idle / 0 fail / 1 ok
    std::atomic<int> _login_kind{0};       // 结果文案区分: 1=登录 2=注册
    std::atomic<int> _qr_evt{-1};          // worker→UI: 1 出码就绪 /2 文案 /3 成功 /4 重出 /5 失败
    std::mutex _mu;
    std::string _qr_dc, _qr_url, _qr_msg;  // worker 写, _mu 保护
    uint32_t _qr_poll_ms = 0;              // 下次轮询/重出码时点 (仅 UI 线程)
    uint32_t _qr_close_ms = 0;             // 成功后自动收层时点 (仅 UI 线程)
    std::function<void()> _on_login;
};
