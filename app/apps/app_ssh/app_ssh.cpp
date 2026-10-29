/*
 * app_ssh — SSH 终端主实现 (Phase 2)。
 * UI 状态机: 表单(_ui_sess 0/1/3) ⇄ 会话屏(2); 状态跃迁只在 onRunning
 * (LVGL 线程)里做, 会话任务只写原子量 + 在 lvglLock 下画 canvas。
 */
#include "app_ssh.h"
#include "../utils/theme/theme.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <cstdlib>
#include <cstring>

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "hal/components/hal_tab5_keyboard.h"
#include "../utils/assets/assets_partition.h"
#include "terminus_font.h"
#include "cjk_font.h"

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
    mclog::tagInfo(TAG, "ready (libssh 0.11.5)");
#endif
}

void AppSsh::_destroyUi()
{
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _form = _ta_host = _ta_user = _ta_pass = _form_status = nullptr;
#ifndef PLATFORM_BUILD_DESKTOP
    _canvas = nullptr;
    _disc_btn = nullptr;
    if (_cbuf) {
        free(_cbuf);
        _cbuf = nullptr;
    }
#endif
}

void AppSsh::onOpen()
{
    if (_scr) _destroyUi();   // 工具页模式: 幂等收割
    mclog::tagInfo(TAG, "on open");
#ifndef PLATFORM_BUILD_DESKTOP
    if (!_emu) _emu = std::make_unique<TerminalEmulator>();
#endif
    _buildForm();
    _alive = std::make_shared<bool>(true);
    _installSwipeGesture();
    lv_screen_load(_scr);
}

void AppSsh::onRunning()
{
#ifndef PLATFORM_BUILD_DESKTOP
    static int last = -1;
    int st = _sess_state.load();
    if (st != last) {
        // 状态跃迁: 表单 ⇄ 会话屏 的屏切换都在 LVGL 线程做
        if (st == 2) {
            if (_form) { lv_obj_delete(_form); _form = nullptr; }
            _buildSession();
            mclog::tagInfo(TAG, "ui: session screen up");
        } else {
            if (_canvas || _disc_btn) _teardownSession();
            if (!_form) _buildForm();
            if (_form_status) {
                if (st == 1)      lv_label_set_text(_form_status, "连接中…");
                else if (st == 3) lv_label_set_text_fmt(_form_status, "连接失败: %s", _err);
                else if (last == 2) lv_label_set_text(_form_status, "已断开");
            }
        }
        last = st;
    }
#endif
}

void AppSsh::onClose()
{
    mclog::tagInfo(TAG, "on close");
#ifndef PLATFORM_BUILD_DESKTOP
    _sess_closing = true;
    _removeKeySink();
#endif
    *_alive = false;
    _removeSwipeGesture();
    _destroyUi();
}

// ═══════════════════════════════════════════════════════════════════════
// 连接表单
// ═══════════════════════════════════════════════════════════════════════
void AppSsh::_buildForm()
{
    const theme::Palette& T = theme::cur();
    if (!_scr) {
        _scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
        lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);
    }

    _form = lv_obj_create(_scr);
    lv_obj_set_size(_form, 680, 560);
    lv_obj_align(_form, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(_form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_form, 0, 0);
    lv_obj_set_style_pad_all(_form, 0, 0);
    lv_obj_clear_flag(_form, LV_OBJ_FLAG_SCROLLABLE);

    auto mk_label = [&](const char* txt, int y) {
        lv_obj_t* l = lv_label_create(_form);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh_font_md(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(T.text_dim), 0);
        lv_obj_set_pos(l, 0, y);
        return l;
    };
    auto mk_ta = [&](int y, const char* def, bool password) {
        lv_obj_t* ta = lv_textarea_create(_form);
        lv_obj_set_size(ta, 680, 64);
        lv_obj_set_pos(ta, 0, y);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_password_mode(ta, password);
        if (def && def[0]) lv_textarea_set_text(ta, def);
        lv_obj_set_style_text_font(ta, zh_font_md(), 0);
        lv_obj_set_style_bg_color(ta, lv_color_hex(T.card[1]), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(T.border), 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_radius(ta, 10, 0);
        lv_obj_add_event_cb(ta, _focus_ta_cb, LV_EVENT_CLICKED, nullptr);
        // 关键: 输入框必须加入 hal 键盘组, 物理键盘的字符才会投递到
        // 组内被聚焦的 textarea(lv_group_focus_obj 只对组内对象生效)。
        // hal 键盘驱动路径: kb_forward_to_lvgl → 组内聚焦对象若是
        // textarea → lv_textarea_add_char。删除对象时 LVGL 自动退组。
        if (hal::HalBase* hal = GetHAL(); hal && hal->lvKbGroup) {
            lv_group_add_obj(hal->lvKbGroup, ta);
        }
        return ta;
    };

    mk_label("主机", 66);
    _ta_host = mk_ta(96, "192.168.1.200", false);
    mk_label("用户", 176);
    _ta_user = mk_ta(206, "orangepi", false);
    mk_label("密码", 286);
    _ta_pass = mk_ta(316, "", true);

#ifndef PLATFORM_BUILD_DESKTOP
    lv_obj_t* btn = lv_btn_create(_form);
    lv_obj_set_size(btn, 240, 72);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 420);
    lv_obj_set_style_bg_color(btn, lv_color_hex(T.accent), 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, _connect_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, "连  接");
    lv_obj_set_style_text_font(bl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x000000), 0);
    lv_obj_center(bl);
#else
    lv_obj_t* dh = lv_label_create(_form);
    lv_label_set_text(dh, "SSH 仅真机可用 (libssh 组件)");
    lv_obj_set_style_text_font(dh, zh_font_md(), 0);
    lv_obj_set_style_text_color(dh, lv_color_hex(T.text_dim), 0);
    lv_obj_align(dh, LV_ALIGN_TOP_MID, 0, 440);
#endif

    _form_status = lv_label_create(_scr);
    lv_label_set_text(_form_status, "");
    lv_obj_set_style_text_font(_form_status, zh_font_md(), 0);
    lv_obj_set_style_text_color(_form_status, lv_color_hex(T.text_dim), 0);
    lv_obj_align(_form_status, LV_ALIGN_TOP_MID, 0, 650);

#ifndef PLATFORM_BUILD_DESKTOP
    // 默认聚焦主机框: 打开表单即可直接打字(跳过点击这一步)
    if (hal::HalBase* hal = GetHAL(); hal && hal->lvKbGroup && _ta_host) {
        lv_group_focus_obj(_ta_host);
    }
#endif
}

// ── 表单事件(两平台共用) ───────────────────────────────────────────────
void AppSsh::_focus_ta_cb(lv_event_t* e)
{
    auto* ta = static_cast<lv_obj_t*>(lv_event_get_target(e));
    hal::HalBase* hal = GetHAL();
    if (hal && hal->lvKbGroup) lv_group_focus_obj(ta);
}

#ifndef PLATFORM_BUILD_DESKTOP

// ═══════════════════════════════════════════════════════════════════════
// 会话屏 + 终端渲染
// ═══════════════════════════════════════════════════════════════════════
namespace {
constexpr int COLS = 91, ROWS = 25;
constexpr int CW = 14, CH = 28;
constexpr int TW = COLS * CW, TH = ROWS * CH;   // 1274 x 700

// xterm 16 色 (RGB565)
uint16_t sshPal(int i)
{
    static const uint16_t t[16] = {
        0x0000, 0xC986, 0x0DEF, 0xE722,
        0x2399, 0xB9F7, 0x2DDB, 0xE73C,
        0x632C, 0xF269, 0x2691, 0xF7A8,
        0x3C7D, 0xD39A, 0x2DDB, 0xFFFF,
    };
    return t[i & 15];
}

uint16_t resolveColor(uint32_t c, bool bold)
{
    if (c & 0x01000000UL) return static_cast<uint16_t>(c & 0xFFFF);   // 直接色
    int i = static_cast<int>(c) & 15;
    if (bold && i < 8) i += 8;   // bold 的标准色提亮
    return sshPal(i);
}
}  // namespace

void AppSsh::_buildSession()
{
    _cbuf = malloc((size_t)TW * TH * 2);
    if (!_cbuf) {
        mclog::tagError(TAG, "canvas buf alloc failed");
        return;
    }
    _canvas = lv_canvas_create(_scr);
    lv_canvas_set_buffer(_canvas, _cbuf, TW, TH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(_canvas, (1280 - TW) / 2, (720 - TH) / 2);

    _disc_btn = lv_btn_create(_scr);
    lv_obj_set_size(_disc_btn, 56, 56);
    lv_obj_align(_disc_btn, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_set_style_bg_color(_disc_btn, lv_color_hex(theme::cur().danger), 0);
    lv_obj_set_style_radius(_disc_btn, 28, 0);
    lv_obj_set_style_shadow_width(_disc_btn, 0, 0);
    lv_obj_add_event_cb(_disc_btn, _disc_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* x = lv_label_create(_disc_btn);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(x, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(x);

    _installKeySink();
    _renderTerminal();   // 首帧
}

void AppSsh::_teardownSession()
{
    _removeKeySink();
    if (_disc_btn) { lv_obj_delete(_disc_btn); _disc_btn = nullptr; }
    if (_canvas)   { lv_obj_delete(_canvas); _canvas = nullptr; }
    if (_cbuf)     { free(_cbuf); _cbuf = nullptr; }
}

void AppSsh::_drawCell(int col, int row, bool cursor_here)
{
    const TerminalEmulator::Cell& c = _emu->displayCell(col, row);
    uint16_t fg = resolveColor(c.fg, c.bold);
    uint16_t bg = resolveColor(c.bg, false);
    if (c.inverse || cursor_here) std::swap(fg, bg);

    uint16_t* buf = (uint16_t*)_cbuf;
    const int x0 = col * CW, y0 = row * CH;
    // 背景
    for (int y = y0; y < y0 + CH; ++y) {
        uint16_t* line = buf + (size_t)y * TW;
        for (int x = x0; x < x0 + CW; ++x) line[x] = bg;
    }
    if (c.continuation) return;   // 宽字符右半: 只铺背景

    // 字形: Terminus 252 码点; 无字形(如 CJK)画豆腐框
    if (c.ch.empty()) return;
    uint32_t cp = 0;
    const uint8_t* p = (const uint8_t*)c.ch.data();
    if (p[0] < 0x80) cp = p[0];
    else if ((p[0] & 0xE0) == 0xC0 && c.ch.size() >= 2)
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    else if ((p[0] & 0xF0) == 0xE0 && c.ch.size() >= 3)
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else if ((p[0] & 0xF8) == 0xF0 && c.ch.size() >= 4)
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);

    const TerminusFont::Font& font = TerminusFont::fontForHeight(CH);
    if (TerminusFont::hasGlyph(font, cp)) {
        TerminusFont::drawGlyphRgb565(buf, TW, TH, font, cp,
                                      x0 + (CW - font.targetW) / 2, y0, fg);
    } else if (c.wide && CjkFont::hasGlyph(cp)) {
        // CJK 宽字形: 24x24 点阵居中于 2 格 (28x28)
        CjkFont::drawGlyphRgb565(buf, TW, TH, cp, x0 + (2 * CW - 24) / 2,
                                 y0 + (CH - 24) / 2, fg);
    } else if (cp >= 0x20) {
        // 豆腐框 (1px 边线): 宽字符跨 2 格, 窄字符 1 格
        const int span = c.wide ? 2 * CW : CW;
        for (int x = x0 + 1; x < x0 + span - 1; ++x) {
            buf[(size_t)y0 * TW + x] = fg;
            buf[(size_t)(y0 + CH - 1) * TW + x] = fg;
        }
        for (int y = y0 + 1; y < y0 + CH - 1; ++y) {
            buf[(size_t)y * TW + x0 + 1] = fg;
            buf[(size_t)y * TW + x0 + span - 2] = fg;
        }
    }
}

void AppSsh::_renderTerminal()
{
    if (!_canvas || !_emu || !_cbuf) return;
    GetHAL()->lvglLock();
    bool cursor = _emu->cursorVisible() && _emu->scrollbackOffset() == 0;
    bool any = false;
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            if (!_emu->displayCell(col, row).dirty) continue;
            _drawCell(col, row, cursor && (int)_emu->cursorColumn() == col &&
                                       (int)_emu->cursorRow() == row);
            any = true;
        }
    }
    _emu->clearDirty();
    if (any) lv_obj_invalidate(_canvas);
    GetHAL()->lvglUnlock();
}

// ═══════════════════════════════════════════════════════════════════════
// 键盘输入: raw sink → 队列 → 会话任务写通道 (libssh noop 锁=读写同任务)
// ═══════════════════════════════════════════════════════════════════════
struct KeyEvt { uint8_t len; uint8_t b[6]; };
static QueueHandle_t s_keyq = nullptr;

// USB HID keycode → 终端字节 (xterm 转义序列)
static int mapKey(uint8_t modifier, uint8_t keycode, bool shift, uint8_t* out)
{
    bool ctrl = modifier & 0x11;   // LCTRL|RCTRL
    if (keycode >= 0x04 && keycode <= 0x1D) {          // A-Z
        char c = 'a' + (keycode - 0x04);
        if (ctrl) { out[0] = (uint8_t)(c & 0x1F); return 1; }
        out[0] = (uint8_t)(shift ? c - 32 : c);
        return 1;
    }
    if (ctrl && keycode == 0x2C) { out[0] = 0; return 1; }   // Ctrl+Space → NUL
    switch (keycode) {
        case 0x28: out[0] = '\r'; return 1;                    // Enter
        case 0x29: out[0] = 0x1B; return 1;                    // Esc
        case 0x2A: out[0] = 0x7F; return 1;                    // Backspace
        case 0x2B: if (shift) { memcpy(out, "\x1b[Z", 3); return 3; }   // Shift+Tab
                   out[0] = '\t'; return 1;
        case 0x2C: out[0] = ' '; return 1;
        case 0x2D: out[0] = shift ? '_' : '-'; return 1;
        case 0x2E: out[0] = shift ? '+' : '='; return 1;
        case 0x2F: out[0] = shift ? '{' : '['; return 1;
        case 0x30: out[0] = shift ? '}' : ']'; return 1;
        case 0x31: out[0] = shift ? '|' : '\\'; return 1;
        case 0x33: out[0] = shift ? ':' : ';'; return 1;
        case 0x34: out[0] = shift ? '"' : '\''; return 1;
        case 0x35: out[0] = shift ? '~' : '`'; return 1;
        case 0x36: out[0] = shift ? '<' : ','; return 1;
        case 0x37: out[0] = shift ? '>' : '.'; return 1;
        case 0x38: out[0] = shift ? '?' : '/'; return 1;
        case 0x1E: case 0x1F: case 0x20: case 0x21:             // 数字行
        case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
            static const char n[] = "1234567890";
            static const char s[] = "!@#$%^&*()";
            out[0] = shift ? s[keycode - 0x1E] : n[keycode - 0x1E];
            return 1;
        }
        case 0x4A: memcpy(out, "\x1b[H", 3); return 3;          // Home
        case 0x4D: memcpy(out, "\x1b[F", 3); return 3;          // End
        case 0x4B: memcpy(out, "\x1b[5~", 4); return 4;         // PgUp
        case 0x4E: memcpy(out, "\x1b[6~", 4); return 4;         // PgDn
        case 0x4C: memcpy(out, "\x1b[3~", 4); return 4;         // Del
        case 0x4F: memcpy(out, "\x1b[C", 3); return 3;          // Right
        case 0x50: memcpy(out, "\x1b[D", 3); return 3;          // Left
        case 0x51: memcpy(out, "\x1b[B", 3); return 3;          // Down
        case 0x52: memcpy(out, "\x1b[A", 3); return 3;          // Up
        case 0x3A: memcpy(out, "\x1bOP", 3); return 3;          // F1
        case 0x3B: memcpy(out, "\x1bOQ", 3); return 3;          // F2
        case 0x3C: memcpy(out, "\x1bOR", 3); return 3;          // F3
        case 0x3D: memcpy(out, "\x1bOS", 3); return 3;          // F4
        case 0x3E: memcpy(out, "\x1b[15~", 5); return 5;        // F5
        case 0x3F: memcpy(out, "\x1b[17~", 5); return 5;        // F6
        case 0x40: memcpy(out, "\x1b[18~", 5); return 5;        // F7
        case 0x41: memcpy(out, "\x1b[19~", 5); return 5;        // F8
        case 0x42: memcpy(out, "\x1b[20~", 5); return 5;        // F9
        case 0x43: memcpy(out, "\x1b[21~", 5); return 5;        // F10
        case 0x44: memcpy(out, "\x1b[23~", 5); return 5;        // F11
        case 0x45: memcpy(out, "\x1b[24~", 5); return 5;        // F12
    }
    return 0;
}

bool AppSsh::_rawKeyCb(uint8_t modifier, uint8_t keycode, bool shift, void* user)
{
    auto* app = static_cast<AppSsh*>(user);
    if (!app || app->_sess_state.load() != 2) return false;   // 非会话态不拦截
    KeyEvt e{};
    e.len = (uint8_t)mapKey(modifier, keycode, shift, e.b);
    if (!e.len) return false;
    if (s_keyq) xQueueSend(s_keyq, &e, 0);   // 满则丢(打字速率远低于容量)
    return true;
}

void AppSsh::_installKeySink()
{
    if (!s_keyq) s_keyq = xQueueCreate(64, sizeof(KeyEvt));
    xQueueReset(s_keyq);
    hal_tab5_keyboard_set_raw_sink(_rawKeyCb, this);
}

void AppSsh::_removeKeySink()
{
    hal_tab5_keyboard_set_raw_sink(nullptr, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// 会话任务
// ═══════════════════════════════════════════════════════════════════════
void AppSsh::_startTask()
{
    if (_task_running.load()) return;
    _task_running.store(true);
    // SSH+mbedtls 握手深调用链: 8KB 会在 ssh_connect 内打穿栈(Stack
    // protection fault 实测), 给 32KB
    GetHAL()->tryRunDetached([this]() { _sessionTask(); }, 32 * 1024);
}

void AppSsh::_sessionTask()
{
    std::string err;
    if (!_ssh.connect(_profile, err, COLS, ROWS)) {
        snprintf(_err, sizeof(_err), "%s", err.c_str());
        _sess_state.store(3);
        _task_running.store(false);
        mclog::tagWarn(TAG, "connect failed: {}", err);
        return;
    }
    _emu->resize(COLS, ROWS);
    _emu->reset();
    _emu->putStatusLine("已连接 " + _profile.user + "@" + _profile.host);
    _sess_state.store(2);
    _renderTerminal();

    char buf[4096];
    while (!_sess_closing.load()) {
        int n = _ssh.read(buf, sizeof(buf));
        if (n > 0) {
            _emu->write(buf, (size_t)n);
            _renderTerminal();
        } else if (n < 0) {
            break;
        }
        if (s_keyq) {
            KeyEvt e;
            while (xQueueReceive(s_keyq, &e, 0) == pdTRUE) {
                if (!_ssh.write(e.b, e.len)) { _sess_closing.store(true); break; }
            }
        }
        if (!_ssh.connected()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _ssh.disconnect();
    _emu->putStatusLine("已断开");
    _renderTerminal();
    mclog::tagInfo(TAG, "session end");
    _sess_state.store(0);
    _task_running.store(false);
}

// ── 表单/按钮事件(仅真机) ──────────────────────────────────────────────
void AppSsh::_connect_cb(lv_event_t* e)
{
    auto* app = static_cast<AppSsh*>(lv_event_get_user_data(e));
    app->_profile.host = lv_textarea_get_text(app->_ta_host);
    app->_profile.user = lv_textarea_get_text(app->_ta_user);
    app->_profile.password = lv_textarea_get_text(app->_ta_pass);
    app->_profile.port = 22;
    if (app->_profile.host.empty() || app->_profile.user.empty()) {
        lv_label_set_text(app->_form_status, "主机 / 用户要填(密码可空=走密钥)");
        return;
    }
    app->_err[0] = 0;
    app->_sess_state.store(1);
    app->_startTask();
}

void AppSsh::_disc_cb(lv_event_t* e)
{
    auto* app = static_cast<AppSsh*>(lv_event_get_user_data(e));
    app->_sess_closing.store(true);
}

#endif  // PLATFORM_BUILD_DESKTOP

// ═══════════════════════════════════════════════════════════════════════
// 上拨手势退出 (app_rfid 同款)
// ═══════════════════════════════════════════════════════════════════════
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
}

void AppSsh::_requestClose()
{
    // 工具页模式: _close_cb = openApp(工具页), 异步切换; 会话在 onClose 停
#ifndef PLATFORM_BUILD_DESKTOP
    _sess_closing.store(true);
#endif
    if (_close_cb) _close_cb();
}
