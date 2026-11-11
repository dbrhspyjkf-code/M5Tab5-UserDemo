#include "tab5_bridge_lcd.h"
#include "application.h"
#include "xiaozhi_ctl.h"
#include <cbin_font.h>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <ctime>
#include <cstring>
#include "theme/theme.h"

// 配色: 主题快照(BuildRichUi 时刷新; 动态更新处读这些全局)
static uint32_t XZ_ACCENT, XZ_SOFT, XZ_DIM, XZ_PRESS, XZ_CARD, XZ_HI,
                XZ_TRACK, XZ_OK, XZ_WARN, XZ_DANGER;
static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    XZ_ACCENT = T.accent;   XZ_SOFT = T.text_soft;  XZ_DIM = T.text_dim;
    XZ_PRESS = T.press;     XZ_CARD = T.card[1];    XZ_HI = T.text_hi;
    XZ_TRACK = T.track;     XZ_OK = T.ok;  XZ_WARN = T.warn;  XZ_DANGER = T.danger;
}
static const char* TAG = "Tab5BridgeLcd";

LV_FONT_DECLARE(font_awesome_30_4);

// ─── 官方 Twemoji64 表情包 (xiaozhi-fonts 组件, RGB565A8 带透明, 64×64) ───────
// 情绪名映射与上游 emoji_collection.cc Twemoji64 一致; 服务端情绪串直接查表。
extern "C" const lv_image_dsc_t emoji_1f636_64;  // neutral
extern "C" const lv_image_dsc_t emoji_1f642_64;  // happy
extern "C" const lv_image_dsc_t emoji_1f606_64;  // laughing
extern "C" const lv_image_dsc_t emoji_1f602_64;  // funny
extern "C" const lv_image_dsc_t emoji_1f614_64;  // sad
extern "C" const lv_image_dsc_t emoji_1f620_64;  // angry
extern "C" const lv_image_dsc_t emoji_1f62d_64;  // crying
extern "C" const lv_image_dsc_t emoji_1f60d_64;  // loving
extern "C" const lv_image_dsc_t emoji_1f633_64;  // embarrassed
extern "C" const lv_image_dsc_t emoji_1f62f_64;  // surprised
extern "C" const lv_image_dsc_t emoji_1f631_64;  // shocked
extern "C" const lv_image_dsc_t emoji_1f914_64;  // thinking
extern "C" const lv_image_dsc_t emoji_1f609_64;  // winking
extern "C" const lv_image_dsc_t emoji_1f60e_64;  // cool
extern "C" const lv_image_dsc_t emoji_1f60c_64;  // relaxed
extern "C" const lv_image_dsc_t emoji_1f924_64;  // delicious
extern "C" const lv_image_dsc_t emoji_1f618_64;  // kissy
extern "C" const lv_image_dsc_t emoji_1f60f_64;  // confident
extern "C" const lv_image_dsc_t emoji_1f634_64;  // sleepy
extern "C" const lv_image_dsc_t emoji_1f61c_64;  // silly
extern "C" const lv_image_dsc_t emoji_1f644_64;  // confused

static const lv_image_dsc_t* emotion_to_emoji(const char* e)
{
    if (!e)                       return &emoji_1f636_64;
    if (!strcmp(e, "neutral"))    return &emoji_1f636_64;
    if (!strcmp(e, "happy"))      return &emoji_1f642_64;
    if (!strcmp(e, "laughing"))   return &emoji_1f606_64;
    if (!strcmp(e, "funny"))      return &emoji_1f602_64;
    if (!strcmp(e, "excited"))    return &emoji_1f606_64;   // 同上游映射
    if (!strcmp(e, "sad"))        return &emoji_1f614_64;
    if (!strcmp(e, "angry"))      return &emoji_1f620_64;
    if (!strcmp(e, "crying"))     return &emoji_1f62d_64;
    if (!strcmp(e, "loving"))     return &emoji_1f60d_64;
    if (!strcmp(e, "embarrassed"))return &emoji_1f633_64;
    if (!strcmp(e, "surprised"))  return &emoji_1f62f_64;
    if (!strcmp(e, "shocked"))    return &emoji_1f631_64;
    if (!strcmp(e, "thinking"))   return &emoji_1f914_64;
    if (!strcmp(e, "winking"))    return &emoji_1f609_64;
    if (!strcmp(e, "cool"))       return &emoji_1f60e_64;
    if (!strcmp(e, "relaxed"))    return &emoji_1f60c_64;
    if (!strcmp(e, "delicious"))  return &emoji_1f924_64;
    if (!strcmp(e, "kissy"))      return &emoji_1f618_64;
    if (!strcmp(e, "confident"))  return &emoji_1f60f_64;
    if (!strcmp(e, "sleepy"))     return &emoji_1f634_64;
    if (!strcmp(e, "silly"))      return &emoji_1f61c_64;
    if (!strcmp(e, "confused"))   return &emoji_1f644_64;
    // microchip_ai 等无对应图: 回落 neutral (与上游 GetEmojiImage 缺失行为一致)
    return &emoji_1f636_64;
}

// ─── Fonts ────────────────────────────────────────────────────────────────────
// GB2312 common ~3500 glyphs, referenced in-place from flash (no big RAM copy).
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");

static const lv_font_t* zh_font()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}

// ─── Helper mappings ──────────────────────────────────────────────────────────

static const char* rssi_to_icon(int rssi)
{
    if (rssi == 0)   return FONT_AWESOME_WIFI_SLASH;
    if (rssi >= -60) return FONT_AWESOME_WIFI;
    if (rssi >= -75) return FONT_AWESOME_WIFI_FAIR;
    return FONT_AWESOME_WIFI_WEAK;
}

static const char* pct_to_batt_icon(int pct)
{
    if (pct < 0)   return FONT_AWESOME_BATTERY_SLASH;
    if (pct >= 75) return FONT_AWESOME_BATTERY_FULL;
    if (pct >= 50) return FONT_AWESOME_BATTERY_THREE_QUARTERS;
    if (pct >= 25) return FONT_AWESOME_BATTERY_HALF;
    if (pct >= 10) return FONT_AWESOME_BATTERY_QUARTER;
    return FONT_AWESOME_BATTERY_EMPTY;
}

// ─── LVGL timer callbacks (run inside LVGL lock — no DisplayLockGuard) ────────

static void info_timer_cb(lv_timer_t* t)
{
    static_cast<Tab5BridgeLcdDisplay*>(lv_timer_get_user_data(t))->UpdateInfoBar();
}

// 2026-11-10: 点按说话已移除 —— 触碰小智界面即退出 (AppXiaoZhi 在 indev 层
// 处理 CLICKED, 见 app_xiaozhi.cpp; 语音直聊, 无需点按交互)。

// ─── Construction / destruction ───────────────────────────────────────────────

Tab5BridgeLcdDisplay::Tab5BridgeLcdDisplay(lv_display_t* existing_disp,
                                            int width, int height)
    : LcdDisplay(nullptr, nullptr, width, height)
{
    display_ = existing_disp;
    BuildRichUi();
    ESP_LOGI(TAG, "rich display ready %dx%d scr=%p", width, height, scr_);
}

Tab5BridgeLcdDisplay::~Tab5BridgeLcdDisplay()
{
    // Delete timer before LVGL objects (it references 'this').
    // Must be within LVGL lock since the timer runs in LVGL context.
    DisplayLockGuard lock(this);
    if (info_timer_) { lv_timer_delete(info_timer_); info_timer_ = nullptr; }
    // Null out borrowed handles so ~LcdDisplay() doesn't double-free them.
    display_  = nullptr;
    panel_    = nullptr;
    panel_io_ = nullptr;
}

// ─── Backdrop blur / snapshot 已撤 (2026-11-11 用户定稿: 背景不虚化不透明,
// ─── 只保留底部实底对话条; LV_USE_SNAPSHOT 同步关回)。

void Tab5BridgeLcdDisplay::ActivateScreen()
{
    if (!scr_) return;
    DisplayLockGuard lock(this);
    lv_screen_load(scr_);
}

void Tab5BridgeLcdDisplay::DeactivateScreen()
{
    // The active screen is restored by the AppXiaoZhi close callback
    // (home->restoreScreen()). This method exists solely as an
    // entrypoint for xiaozhi_ctl; no LVGL action is needed here.
}

// ─── UI construction ──────────────────────────────────────────────────────────

void Tab5BridgeLcdDisplay::BuildRichUi()
{
    DisplayLockGuard lock(this);
    snap_theme();

    scr_ = lv_obj_create(NULL);
    lv_obj_set_size(scr_, width_, height_);
    lv_obj_set_style_bg_color(scr_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr_, 0, 0);
    lv_obj_clear_flag(scr_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr_, LV_OBJ_FLAG_CLICKABLE);   // 点按不说话, 触碰退出在 AppXiaoZhi indev 层

    // 分层(创建顺序即 z 序, 底→顶): 表情 → 底部消息条。
    // 2026-11-11 用户定稿: 整页纯黑背景(不虚化不透明), 只有底部对话条有实底。

    // The top info bar (clock/WiFi/battery) now lives on the home screen, so
    // xiaozhi runs full-screen with no top bar. 底部状态行(聆听中)也已移除
    // (2026-11-10 定稿: 语音直聊, 底部只留当前消息)。
    BuildEmotionArea();   // 屏幕正中偏下 ≈(640,420), 4×
    BuildBottomStrip();   // y=540, h=180, 只显示当前消息
}

void Tab5BridgeLcdDisplay::BuildInfoBar()
{
    lv_obj_t* bar = lv_obj_create(scr_);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, width_, 56);
    lv_obj_set_style_bg_color(bar, lv_color_hex(XZ_TRACK), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Title (left)
    lv_obj_t* title = lv_label_create(bar);
    lv_obj_set_style_text_font(title, zh_font(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(XZ_ACCENT), 0);
    lv_label_set_text(title, "小智 AI");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    // Clock (center)
    time_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(time_lbl_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(time_lbl_, lv_color_hex(XZ_SOFT), 0);
    lv_label_set_text(time_lbl_, "00:00");
    lv_obj_align(time_lbl_, LV_ALIGN_CENTER, 0, 0);

    // WiFi icon
    wifi_icon_ = lv_label_create(bar);
    lv_obj_set_style_text_font(wifi_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(wifi_icon_, lv_color_hex(XZ_ACCENT), 0);
    lv_label_set_text(wifi_icon_, FONT_AWESOME_WIFI);
    lv_obj_align(wifi_icon_, LV_ALIGN_RIGHT_MID, -150, 0);

    // Battery icon
    batt_icon_ = lv_label_create(bar);
    lv_obj_set_style_text_font(batt_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(batt_icon_, lv_color_hex(XZ_ACCENT), 0);
    lv_label_set_text(batt_icon_, FONT_AWESOME_BATTERY_FULL);
    lv_obj_align(batt_icon_, LV_ALIGN_RIGHT_MID, -80, 0);

    // Battery %
    batt_pct_lbl_ = lv_label_create(bar);
    lv_obj_set_style_text_font(batt_pct_lbl_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(batt_pct_lbl_, lv_color_hex(XZ_SOFT), 0);
    lv_label_set_text(batt_pct_lbl_, "--%");
    lv_obj_align(batt_pct_lbl_, LV_ALIGN_RIGHT_MID, -20, 0);
}

void Tab5BridgeLcdDisplay::BuildEmotionArea()
{
    // 2026-11-10: 官方 Twemoji64 表情包替代 font_awesome 字形 (64×64 RGB565A8
    // 带透明, 直读 flash)。2× 显示 (512=2×256, 128px), 双线性抗锯齿由 LVGL
    // image 默认开启。锚点=屏幕正中偏下 (640,420), pivot 设图心保持居中。
    // 用户定稿: 位置固定, 空闲不再漫游 (原 EmojiWanderTimerCb 已删)。
    emotion_img_ = lv_image_create(scr_);
    lv_image_set_src(emotion_img_, &emoji_1f636_64);
    lv_image_set_scale(emotion_img_, 512);
    lv_image_set_pivot(emotion_img_, 32, 32);
    lv_obj_clear_flag(emotion_img_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_align(emotion_img_, LV_ALIGN_CENTER, 0, 60);
}

void Tab5BridgeLcdDisplay::BuildBottomStrip()
{
    // 底部 1/4 (y=540..720): 实底消息条 (2026-11-11 用户定稿: 不透明, 主题
    // track 色; 整页其余纯黑无背景)。只显示当前一条消息(无泡泡, 新消息替换)。
    lv_obj_t* strip = lv_obj_create(scr_);
    lv_obj_set_pos(strip, 0, 540);
    lv_obj_set_size(strip, width_, height_ - 540);   // 180
    lv_obj_set_style_bg_color(strip, lv_color_hex(XZ_TRACK), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_clear_flag(strip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // 当前消息标签 — 垂直居中于整条 (默认主题文字色, 深浅主题都可读)
    msg_lbl_ = lv_label_create(strip);
    lv_obj_set_style_text_font(msg_lbl_, zh_font(), 0);
    lv_obj_set_style_text_color(msg_lbl_, lv_color_hex(XZ_HI), 0);
    lv_obj_set_style_text_align(msg_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(msg_lbl_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_lbl_, width_ - 80);
    lv_label_set_text(msg_lbl_, "");
    lv_obj_align(msg_lbl_, LV_ALIGN_CENTER, 0, 0);
}

// Strip emoji / symbol code-points that the bundled cbin font (puhui_common)
// doesn't carry. cbin has NO emoji glyphs at all, so even whitelisted
// emoji would render as blank + spam 'glyph dsc. not found' warnings. To
// keep the chat readable and the warning log quiet, EVERY emoji / dingbat
// / misc-technical code-point is replaced with a space. CJK / Latin /
// punctuation pass through unchanged. The emoji_table.h whitelist is
// kept for future use (e.g. when an emoji font is added or bitmap
// assets are embedded) but the render path no longer consults it.
#include "emoji_table.h"

static std::string _strip_unsupported_glyphs(const char* in)
{
    if (!in) return "";
    std::string s;
    const unsigned char* p = (const unsigned char*)in;
    while (*p) {
        uint32_t cp = 0;
        int n = 0;
        if      ((*p & 0x80) == 0x00) { cp =  *p       & 0x7F;       n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp =  *p       & 0x1F;       n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp =  *p       & 0x0F;       n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp =  *p       & 0x07;       n = 4; }
        else                           { p++; continue; }
        bool valid = true;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }
        // Drop ALL emoji / dingbats / misc-technical code-points. The cbin
        // font carries no emoji glyphs, so rendering any of them only
        // produces 'glyph dsc. not found' warnings and risks stack
        // protection faults on long text (see 2026-06-19 logs).
        bool drop =
            (cp >= 0x1F000 && cp < 0x1FFFF) ||  // emoji blocks
            (cp >= 0x2700  && cp < 0x27BF)  ||  // dingbats
            (cp >= 0x2300  && cp < 0x24FF);     // misc technical / control pics
        if (drop) s += ' ';
        else      s.append((const char*)p, n);
        p += n;
    }
    return s;
}

// ─── 当前消息 (单标签, 无泡泡) ────────────────────────────────────────────────

// ─── LVGL timer: info bar refresh ─────────────────────────────────────────────

void Tab5BridgeLcdDisplay::UpdateInfoBar()
{
    // Clock
    if (time_lbl_) {
        time_t now;
        time(&now);
        struct tm t = {};
        localtime_r(&now, &t);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        lv_label_set_text(time_lbl_, buf);
    }

    // WiFi RSSI icon
    if (wifi_icon_) {
        wifi_ap_record_t ap = {};
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
        lv_label_set_text(wifi_icon_, rssi_to_icon(rssi));
        lv_color_t c = rssi ? lv_color_hex(XZ_ACCENT) : lv_color_hex(XZ_DANGER);
        lv_obj_set_style_text_color(wifi_icon_, c, 0);
    }

    // Battery
    if (batt_icon_ && batt_pct_lbl_) {
        int pct = xiaozhi_get_battery_percent();
        lv_label_set_text(batt_icon_, pct_to_batt_icon(pct));
        if (pct >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_label_set_text(batt_pct_lbl_, buf);
            lv_color_t c = pct < 20 ? lv_color_hex(XZ_DANGER)
                         : pct < 40 ? lv_color_hex(XZ_WARN)
                         :            lv_color_hex(XZ_ACCENT);
            lv_obj_set_style_text_color(batt_icon_, c, 0);
            lv_obj_set_style_text_color(batt_pct_lbl_, c, 0);
        } else {
            lv_label_set_text(batt_pct_lbl_, "--%");
        }
    }
}

// ─── LcdDisplay overrides ─────────────────────────────────────────────────────

void Tab5BridgeLcdDisplay::SetupUI()
{
    setup_ui_called_ = true; // suppress xiaozhi's heavy default UI
}

void Tab5BridgeLcdDisplay::SetStatus(const char* /*status*/)
{
    // 2026-11-10 定稿: 底部状态行(●聆听中/说话中)已移除 — 语音直聊无需状态提示,
    // 底部只显示当前消息。上游 application.cc 仍会调用本回调, 保留空实现。
}

void Tab5BridgeLcdDisplay::SetEmotion(const char* emotion)
{
    if (!emotion) return;
    DisplayLockGuard lock(this);
    if (emotion_img_) lv_image_set_src(emotion_img_, emotion_to_emoji(emotion));
}

void Tab5BridgeLcdDisplay::SetChatMessage(const char* role, const char* content)
{
    if (!content || content[0] == '\0') return;
    DisplayLockGuard lock(this);
    if (!msg_lbl_) return;

    // 只显示当前一条: 每条新消息直接替换标签文本。
    // 用户话=主题 accent, 小智=主题正文色 (2026-11-11 实底 track 条上,
    // 深浅主题都随 theme 快照保证可读)。
    bool is_user = (role && strcmp(role, "user") == 0);
    if (is_user) xiaozhi_note_user_stt();   // 服务端已回传识别文本 → 盖章给退出判定
    lv_obj_set_style_text_color(msg_lbl_, lv_color_hex(is_user ? XZ_ACCENT : XZ_HI), 0);

    // Drop emoji / symbol code-points the font doesn't carry (avoids LVGL
    // 'glyph dsc. not found' warning storms) — see _strip_unsupported_glyphs.
    std::string filtered = _strip_unsupported_glyphs(content);
    lv_label_set_text(msg_lbl_, filtered.c_str());
}

void Tab5BridgeLcdDisplay::ClearChatMessages()
{
    DisplayLockGuard lock(this);
    if (msg_lbl_) lv_label_set_text(msg_lbl_, "");
}

void Tab5BridgeLcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> /*image*/)
{
    // No preview support in the lightweight display.
}

void Tab5BridgeLcdDisplay::SetTheme(Theme* theme)
{
    Display::SetTheme(theme);
}
