#include "app_settings.h"
#include "../app_voice_input/app_voice_input.h"
#include "../utils/psram_json.h"
#include "../utils/theme/theme.h"
#include "../utils/assets/assets_partition.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <chrono>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_heap_caps.h>
#endif
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#elif defined(PLATFORM_BUILD_WEB)
#include "web_fonts.h"
#else
extern "C" const lv_font_t font_puhui_20_4;
#endif

static const std::string _tag = "app-tools";

// Xiaozhi's puhui common 30px cbin font — GB2312 ~3500 glyphs, XIP from flash.
static const lv_font_t* zh_font_30()
{
#if defined(PLATFORM_BUILD_WEB)
    return webui::font_30();
#elif !defined(PLATFORM_BUILD_DESKTOP)
    extern const uint8_t font_puhui_common_30_4_bin_start[] asm("_binary_font_puhui_common_30_4_bin_start");
    static lv_font_t* f = nullptr;
    if (!f) f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
#else
    return &font_puhui_20_4;
#endif
}

// 24px res 分区 cbin (font_zh_24, 主页仪表卡正文同款) — res 异常回退 30px
static const lv_font_t* zh_font_md()
{
#if defined(PLATFORM_BUILD_WEB)
    return webui::font_24();
#elif !defined(PLATFORM_BUILD_DESKTOP)
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f ? f : zh_font_30();
#else
    return &font_puhui_20_4;
#endif
}

// Chinese font defined in app_ha/view/font_zh_36.c (now includes 工具计算器).
extern lv_font_t font_zh_36;

// 工具页 tile 图标全部为 make_tile 图形分支现画 (线条取主题强调色, 随主题切换):
// 自选股/万年历 2026-10-27 弃用 stocks8_icon/calendar_icon, 计算器/汇率/单位换算/
// LoRa/RFID/邮件/Reachy 同日弃用 calc_icon/fx_icon/unit_icon/lora_logo/reachy_icon.
// 上述 .c 图片源文件仅存档, 不再被引用 (真机 gc-sections 自动剔除).

// 健康 tile 用预渲染字形 (app_health/health_icons.c, PIL 生成)
extern const lv_image_dsc_t hl_tile_health;

// 双色球 tile 图标: 已改图形化 kind==12 (票券+红六点+蓝一点, 随主题色,
// 2026-11-01); 原 lottery_icon.c 静态彩图无引用由 gc-sections 裁掉.

// 工具页图形分支共用: 画一块纯色小矩形 (图标线条/圆点), 自动冒泡点击到父 tile.
static lv_obj_t* tool_accent_rect(lv_obj_t* parent, uint32_t color,
                                  int x, int y, int w, int h, int radius)
{
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_shadow_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    return r;
}

// 工具页图形分支共用: 画一个纯色描边圆角框 (透明底; radius=边长一半即圆环),
// 自动冒泡点击到父 tile.
static lv_obj_t* tool_accent_frame(lv_obj_t* parent, uint32_t color,
                                   int x, int y, int w, int h, int radius, int border_w)
{
    lv_obj_t* f = lv_obj_create(parent);
    lv_obj_set_size(f, w, h);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(f, radius, 0);
    lv_obj_set_style_border_width(f, border_w, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(f, 0, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(f, LV_OBJ_FLAG_EVENT_BUBBLE);
    return f;
}

// 工具页图形分支共用: 用 lv_line 画折线 (pts 相对线对象原点, 线对象放在 (x,y)),
// 自动冒泡点击到父 tile.
static lv_obj_t* tool_accent_line(lv_obj_t* parent, uint32_t color, int x, int y,
                                  const lv_point_precise_t pts[], uint32_t n, int width)
{
    lv_obj_t* line = lv_line_create(parent);
    lv_line_set_points(line, pts, n);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(line, LV_OBJ_FLAG_EVENT_BUBBLE);
    return line;
}

// 工具页图形分支共用: 用 33 段折线近似画一段圆弧 (角度制, 0°=右, 顺时针增,
// cx/cy 为圆心在父 tile 里的绝对坐标). lv_line 的点数组必须跨帧存活 -> 存静态池;
// 图标几何固定, 页面重建时原槽位重算复用.
static lv_obj_t* tool_accent_arc(lv_obj_t* parent, uint32_t color, int cx, int cy,
                                 int radius, float a0_deg, float a1_deg, int width)
{
    static lv_point_precise_t pool[16][33];
    static int used = 0;
    lv_point_precise_t* pts = pool[used];
    used = (used + 1) % 16;
    for (int i = 0; i <= 32; i++) {
        const float a = (a0_deg + (a1_deg - a0_deg) * i / 32.0f) * 3.14159265f / 180.0f;
        pts[i].x = cx + radius * cosf(a);
        pts[i].y = cy + radius * sinf(a);
    }
    return tool_accent_line(parent, color, 0, 0, pts, 33, width);
}

// Calculator fonts (Arial Unicode subsets) defined in this app's font_calc_*.c.
extern const lv_font_t font_calc_big;   // 80px result line
extern const lv_font_t font_calc_op;    // 40px button labels (has × ÷)
extern const lv_font_t font_calc_expr;  // 30px expression line

// ── Palette ─────────────────────────────────────────────────────────────────
// 配色: 主题快照(onOpen 时从全局主题取, 见 utils/theme/theme.h);
// 计算器数字/功能键的中性灰保留(任何主题下都读作"中性")。
static uint32_t C_BG, C_ACCENT, C_CARD, C_CARD_PR, C_TEXT, C_DIM,
                CALC_BG, CALC_NUM, CALC_FUNC, CALC_OP, CALC_TXT, CALC_EXPR,
                CALC_DISP, C_SHADOW, C_WARN, C_DANGER, C_SOFT;

static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    C_BG = T.page;           // 屏底随主题(2026-11-09 加 page 字段)
    C_ACCENT = T.accent;
    C_CARD = T.card[1];     C_CARD_PR = T.card[0];
    C_TEXT = T.text_hi;     C_SHADOW = T.border;
    C_DIM = T.text_dim;
    C_WARN = T.warn;        // 邮件红旗操作 (语义琥珀)
    C_DANGER = T.danger;    // 红旗标记点
    C_SOFT = T.text_soft;   // 文件夹组头
    CALC_BG = T.page;
    CALC_NUM = 0x3A3A3C;    CALC_FUNC = 0x545456;
    CALC_OP = T.accent;     // 运算键随主题强调色
    CALC_TXT = 0xFFFFFF;    // 键帽文字(深灰键上恒白)
    CALC_DISP = T.text_hi;  // 显示区大字/FROM 金额(浅色主题随主题翻深)
    CALC_EXPR = T.text_soft; // 表达式小字(浅底上 dim 太淡, 2026-11-09)
}

// ── Currency table + live rate cache ─────────────────────────────────────────
namespace {
struct Ccy { const char* code; const char* cn; double per_usd; };
// Static fallback rates (units per 1 USD); overwritten at runtime by the HA
// server's /rate proxy (see _fxFetch). Order must match g_fx_rate below.
const Ccy FX[] = {
    {"USD", "美元",   1.00},
    {"CNY", "人民币", 7.25},
    {"EUR", "欧元",   0.92},
    {"JPY", "日元",   157.0},
    {"GBP", "英镑",   0.79},
    {"HKD", "港元",   7.81},
    {"KRW", "韩元",   1380.0},
    {"AUD", "澳元",   1.51},
};
constexpr int FX_N = (int)(sizeof(FX) / sizeof(FX[0]));

// Live per-USD rates (seeded from the static table, overwritten by /rate). The
// detached fetch worker writes under g_fx_mutex; the UI reads under it too.
std::mutex        g_fx_mutex;
double            g_fx_rate[FX_N] = {1.00, 7.25, 0.92, 157.0, 0.79, 7.81, 1380.0, 1.51};
std::atomic<bool> g_fx_updated{false};   // set by worker, consumed in onRunning
std::atomic<bool> g_fx_fetching{false};
std::string       g_fx_source = "static";

// ── Email cache (worker thread fills, UI thread reads) ──────────────────────
// Mirrors the g_fx_* pattern: detached fetch worker writes g_emails under
// g_email_mutex and flips g_email_updated; the UI loop detects the flag in
// onRunning() and rebuilds the lv_list via _emailRefresh().
}  // namespace

// Static-member definitions for the shared email cache (declared in
// app_settings.h). Mirrors the g_fx_* pattern above: detached fetch worker
// writes email_list under email_list_mutex and flips email_updated (badge
// data only since 2026-11-15 — the email sub-page is now the direct
// five-layer client on /api/mail/*). AppHome's status-bar icon polls
// fetchEmail() every 120s and reads email_unread_total.
std::atomic<int>        AppSettings::email_unread_total{0};  // total, from j["count"]
std::atomic<int>        AppSettings::email_flagged_total{0}; // 红旗总数
std::atomic<bool>       AppSettings::email_updated{false};
std::atomic<bool>       AppSettings::email_fetching{false};
std::atomic<bool>       AppSettings::email_error{false};
std::string             AppSettings::email_error_msg;
std::mutex              AppSettings::email_list_mutex;
std::vector<EmailItem>  AppSettings::email_list;
std::vector<EmailFolderInfo> AppSettings::email_folders;

// ── 邮件客户端 (五层直连, 2026-11-15) 静态 worker 结果 — worker 写, onRunning 读
std::mutex        AppSettings::mailc_mutex;
std::vector<AppSettings::MailAcct>   AppSettings::mailc_accts;
std::vector<AppSettings::MailFolder> AppSettings::mailc_folders;
std::vector<AppSettings::MailMsg>    AppSettings::mailc_msgs;
int      AppSettings::mailc_total = 0;
AppSettings::MailMsg AppSettings::mailc_cur_msg;
std::string AppSettings::mailc_body;
std::string AppSettings::mailc_body_uid, AppSettings::mailc_body_fld;
AppSettings::MailMeta AppSettings::mailc_meta;
std::string AppSettings::mailc_err;
std::string AppSettings::mailc_folders_email, AppSettings::mailc_list_folder;
std::string AppSettings::mailc_act_action, AppSettings::mailc_act_uid;
std::string AppSettings::mailc_act_fld, AppSettings::mailc_act_tip;
std::string AppSettings::mailc_act_err, AppSettings::mailc_send_err;
std::string AppSettings::mailc_login_email;
int      AppSettings::mailc_login_unread = 0;
std::atomic<bool> AppSettings::mailc_accts_done{false},   AppSettings::mailc_accts_ok{false};
std::atomic<bool> AppSettings::mailc_folders_done{false}, AppSettings::mailc_folders_ok{false};
std::atomic<bool> AppSettings::mailc_list_done{false},   AppSettings::mailc_list_ok{false};
std::atomic<bool> AppSettings::mailc_body_done{false},   AppSettings::mailc_body_ok{false};
std::atomic<bool> AppSettings::mailc_act_done{false},    AppSettings::mailc_act_ok{false};
std::atomic<bool> AppSettings::mailc_send_done{false},   AppSettings::mailc_send_ok{false};
std::atomic<bool> AppSettings::mailc_login_done{false},  AppSettings::mailc_login_ok{false};
std::atomic<bool> AppSettings::mailc_del_done{false},    AppSettings::mailc_del_ok{false};

AppSettings::AppSettings()
{
    setAppInfo().name = "工具";
}

void AppSettings::onCreate() {}

void AppSettings::onOpen()
{
    snap_theme();
    mclog::tagInfo(_tag, "open");
    // 防重入守卫: mooncake 的 openApp() 不会关前台 app (onClose 不触发),
    // 子 app 退出回到本页时 onOpen 会被再次调用——若无守卫, 这里 new 一个新屏
    // 并覆盖旧 _scr, 整屏对象全部泄漏; 工具↔子 app 每往返一次漏一屏,
    // 几轮后 LVGL OOM (lv_realloc failed → lv_obj_create assert), main 任务
    // 卡死 = "闪屏 + 触摸无响应" (2026-10-27 自选股实锤). 有屏直接复用.
    if (_scr) {
        lv_screen_load(_scr);
        _installSwipeGesture();
        return;
    }
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    _buildToolsPage();
    lv_screen_load(_scr);
    _installSwipeGesture();
    // 主页邮件图标在 app 未打开时点的: 屏建好了, 补开邮件子页
    if (_email_open_pending) {
        _email_open_pending = false;
        _openEmail();
    }
#ifdef PLATFORM_BUILD_DESKTOP
    if (const char* e = getenv("TAB5_OPEN_CALENDAR")) {
        if (e[0] == '1') {
            // 桌面调试: 直落万年历. onOpen 内直接 openApp 会撞 mooncake
            // 状态机 (本 app 还在 Opening), 延迟到下一拍再开.
            lv_timer_create([](lv_timer_t* tm) {
                auto* self = static_cast<AppSettings*>(lv_timer_get_user_data(tm));
                if (self) self->openCalendar();
                lv_timer_del(tm);
            }, 800, this);
        }
    }
#endif
}

void AppSettings::onRunning()
{
    // Apply freshly fetched live rates (worker thread only flips the flag; the
    // label/recompute must happen on the UI loop).
    if (g_fx_updated.exchange(false) && _fx_page) {
        _fxRecompute();
    }
    // Same pattern for the email client: worker fills statics + flips done
    // flags; we rebuild on the UI loop. 滑开的行正被操作时跳过列表重建
    // (flag 留给后续帧, 行合拢后立即重建)。
    if (_email_page) {
        // 事件回调记下的层切换/重渲染/关页 → 本帧统一执行 (见 _mail_pending 注释)
        if (_mail_pending_close) {
            _mail_pending_close = false;
            _closeEmail();
        } else if (_mail_pending) {
            _mail_pending = false;
            _mailSetLv(_mail_pending_lv);
        } else if (_mail_pending_render) {
            _mail_pending_render = false;
            if (!_mail_swipe_open) _mailRender();
        }
        if (!_email_page) return;   // pending_close 已把页关了
        if (mailc_accts_done.exchange(false) && _mail_lv == MailLv::Acct)
            _mailRender();
        if (mailc_folders_done.exchange(false) && _mail_lv == MailLv::List) {
            // 两列布局: 文件夹按钮在 List 层左列, 拉到即渲染 (含选中态高亮)
            bool match;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                match = (mailc_folders_email == _mail_acct);   // 快速换账号弃旧回执
            }
            if (match) _mailRender();
        }
        if (mailc_list_done.exchange(false) && _mail_lv == MailLv::List) {
            _mail_got = true;
            bool match;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                match = (mailc_list_folder == _mail_folder);   // 快速换夹弃旧回执
            }
            if (match && !_mail_swipe_open) _mailRender();
        }
        if (mailc_body_done.exchange(false) && _mail_body_lbl) {
            bool ok;
            std::string txt, buid, bfld;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                ok = mailc_body_ok.load();
                txt = mailc_body;
                buid = mailc_body_uid;
                bfld = mailc_body_fld;
            }
            // 回执对号: 快速换信时旧正文不作数 (oneapp mail_body 同款)
            if (buid == mailc_cur_msg.uid && bfld == mailc_cur_msg.fld) {
                if (ok) {
                    lv_label_set_text(_mail_body_lbl, txt.c_str());
                    if (_mail_content) lv_obj_scroll_to_y(_mail_content, 0, LV_ANIM_OFF);
                } else {
                    lv_label_set_text(_mail_body_lbl, ("获取失败: " + mailc_body).c_str());
                }
            }
        }
        if (mailc_act_done.exchange(false)) {
            bool ok;
            std::string tip, err, act, uid, fld;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                ok = mailc_act_ok.load();
                tip = mailc_act_tip;
                err = mailc_act_err;
                act = mailc_act_action;
                uid = mailc_act_uid;
                fld = mailc_act_fld;
            }
            if (ok) {
                _mailStatus(tip);
                // 乐观旗标已在点击时应用 + 下一帧重渲染 (onRunning 消费
                // _mail_pending_render); 这里只出反馈文案
            } else {
                mailcApplyFlags(act, uid, fld, false);   // 回滚 (oneapp mail_mark 失败分支)
                _mailStatus("标记失败: " + err);
                if (_mail_lv == MailLv::List && !_mail_swipe_open) _mailRender();
                if (_mail_lv == MailLv::Detail) _mailRender();
            }
        }
        if (mailc_send_done.exchange(false)) {
            bool ok;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                ok = mailc_send_ok.load();
                err = mailc_send_err;
            }
            _mail_sending = false;
            if (ok) {
                bool was_reply = _mail_reply;
                _mail_reply = false;
                _mail_quote.clear();
                _mailStatus("已发送 ✓");
                _mailSetLv(was_reply ? MailLv::Detail : MailLv::List);
            } else {
                _mailStatus("发送失败: " + err);   // 草稿保留在写信层 (oneapp 同款)
            }
        }
        if (mailc_login_done.exchange(false) && _mail_lv == MailLv::Login) {
            bool ok;
            std::string em, err;
            int un;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                ok = mailc_login_ok.load();
                em = mailc_login_email;
                err = mailc_err;
                un = mailc_login_unread;
            }
            if (ok) {
                _mail_acct = em;
                _mailStatus(std::string("已登录 · INBOX 未读 ") + std::to_string(un));
                _mailEnterList("@unread", "全部未读");   // 两列版: 登录直达列表页
                _mailSetLv(MailLv::List);                // onRunning 内同步切层安全
            } else {
                _mailStatus("登录失败: " + err);
            }
        }
        if (mailc_del_done.exchange(false) && _mail_lv == MailLv::Acct) {
            bool ok;
            std::string em, err;
            {
                std::lock_guard<std::mutex> lk(mailc_mutex);
                ok = mailc_del_ok.load();
                err = mailc_err;
            }
            if (ok) {
                _mailStatus("已删除账号");
                mailcFetchAccts();   // 重拉角标 → accts_done 后重渲染
            } else {
                _mailStatus("删除失败: " + err);
            }
        }
    }
    // (旧状态页的 email_updated/email_act onRunning 块已随五层客户端移除;
    //  fetchEmail 仍每 120s 由主页调用刷新信封角标 email_unread_total。)
}

void AppSettings::onClose()
{
    mclog::tagInfo(_tag, "close");
    _removeSwipeGesture();
    if (_scr) {
        // 删屏前先切临时屏: 工具页关屏常发生在活动屏上, 直接删会让
        // LVGL act_screen 悬空一帧 ("lv_obj_delete: the active screen
        // was deleted" 警告实录), 与所有 app onRunning 每帧钩子叠加是
        // UAF 面 (2026-11-09 player 同类 UAF 崩溃定案后补齐).
        lv_obj_t* tmp = lv_obj_create(NULL);
        lv_screen_load(tmp);
        lv_obj_delete(_scr);
        _scr = nullptr;
        lv_obj_delete_async(tmp);
    }
    _tools_page = _calc_page = _fx_page = _unit_page = nullptr;
    _calc_expr_lbl = _calc_res_lbl = nullptr;
    _fx_from_dd = _fx_to_dd = _fx_amt_lbl = _fx_res_lbl = _fx_rate_lbl = nullptr;
    _unit_cat_dd = _unit_from_dd = _unit_to_dd = _unit_amt_lbl = _unit_res_lbl = _unit_eq_lbl = nullptr;
    // 邮件子页对象随 _scr 整体删除, 只清指针 + 层状态 (静态缓存保留供下次秒开)
    AppVoiceInput::dismissMicButton();
    _email_page = _mail_title = _mail_status = _mail_actions = nullptr;
    _mail_content = _mail_hint = _mail_kb = nullptr;
    _mail_ta_email = _mail_ta_auth = _mail_ta_to = _mail_ta_subj = _mail_ta_body = nullptr;
    _mail_body_lbl = nullptr;
    _mail_swipe_open = nullptr;
    _mail_lv = MailLv::Acct;
    _mail_acct.clear();
    _mail_folder.clear();
    _mail_folder_disp.clear();
    _mail_got = false;
    _mail_reply = false;
    _mail_sending = false;
    _mail_quote.clear();
    _mail_del_arm.clear();
    // 不清零 email_unread_total: 本 app 只展示邮件列表, 没有"标记已读"功能,
    // 退出不代表邮件已读. 未读数是服务器真实状态 (由轮询刷新), 退出后状态栏
    // 信封图标 / 灯阵 NEW EMAIL 应继续按真实未读显示.
    email_updated.store(false);
    email_error.store(false);
    email_error_msg.clear();
    email_fetching.store(false);  // 防止重入保护卡死
    if (_close_cb) _close_cb();
}

// ─── Swipe-up gesture closes the whole app (back to home) ───────────────────
void AppSettings::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            static lv_point_t s_gpress{0, 0};   // PRESSED 起点 (底部起手判定)
            static int64_t s_last_close_ms = 0; // GESTURE 与 RELEASE 兜底去重
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSED) {
                    lv_point_t p; lv_indev_get_point(dev, &p);
                    s_gpress = p;
                    return;
                }
                // 底部起手上拨 → 退到主页。双轨判据 (与 app_stocks 同款):
                // GESTURE 承接快速甩动; RELEASE 位移兜底慢速/合成滑动
                // (LVGL GESTURE 依赖速度阈值, 网页版慢速拖拽常不触发,
                //  2026-11-03 工具页上拨失效实锤)。两轨 500ms 内只认第一次。
                const bool gesture_hit = (code == LV_EVENT_GESTURE &&
                    lv_indev_get_gesture_dir(dev) == LV_DIR_TOP);
                bool release_hit = false;
                if (code == LV_EVENT_RELEASED) {
                    lv_point_t p; lv_indev_get_point(dev, &p);
                    const int dx = p.x - s_gpress.x, dy = p.y - s_gpress.y;
                    release_hit = (s_gpress.y > 650 && dy < -120 &&
                                   abs(dy) > abs(dx) * 2);
#ifdef PLATFORM_BUILD_WEB
                    GetHAL()->uartMonitorSend("T-rel gp=" + std::to_string(s_gpress.y) +
                        " dy=" + std::to_string(dy) + " hit=" + (release_hit ? "1" : "0"));
#endif
                }
                if (!gesture_hit && !release_hit) return;
                if (gesture_hit && s_gpress.y <= 650) return;  // 位置守卫
                // steady_clock 纪元 (与 app_stocks mono_ms 同款); 桌面/网页
                // compat 的 esp_timer_get_time 是惰性纪元, 首调前恒 0, 会把
                // 首次手势困进 500ms 去重窗 (2026-11-03 工具页上拨失效真凶)
                const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (now_ms - s_last_close_ms < 500) return;
                s_last_close_ms = now_ms;
                lv_async_call([](void* udata) {
                    auto* app = static_cast<AppSettings*>(udata);
                    if (!app) return;
#ifdef PLATFORM_BUILD_WEB
                    GetHAL()->uartMonitorSend("T-async scr=" +
                        std::to_string(app->_scr ? 1 : 0) + " act=" +
                        std::to_string((int)(uintptr_t)lv_screen_active()));
#endif
                    // Only act when our screen is actually on display. This
                    // hook stays installed while another app opened from
                    // the tools page (e.g. the YRobot cockpit) owns the
                    // foreground — its joystick drags must not close us.
                    if (!app->_scr || lv_screen_active() != app->_scr) return;
                    // Back out of the deepest open sub-page first.
                    // 邮件子页 = 五层客户端: 上拨先逐层回退 (oneapp Esc 同款,
                    // lv_async 内执行 — 可同步删/切), 账号层再上拨才关整页回工具页。
                    if (app->_email_page) {
                        bool close = false;
                        MailLv nxt = app->_mailBackTarget(&close);
                        if (close) app->_closeEmail();
                        else app->_mailSetLv(nxt);
                    }
                    else if (app->_unit_page)   app->_closeUnit();
                    else if (app->_fx_page) app->_closeFx();
                    else if (app->_calc_page) app->_closeCalculator();
                    else                     app->close();
                }, lv_event_get_user_data(e));
            }, LV_EVENT_ALL, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppSettings::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Tools launcher page
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_buildToolsPage()
{
    _tools_page = lv_obj_create(_scr);
    lv_obj_set_size(_tools_page, 1280, 720);
    lv_obj_set_pos(_tools_page, 0, 0);
    lv_obj_set_style_bg_opa(_tools_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tools_page, 0, 0);
    lv_obj_set_style_pad_all(_tools_page, 0, 0);
    // 2026-11-05: 加计时器第 4 行后内容超一屏 → 页面可滚, 底缘上拨退出被
    // 滚动吞掉 (用户实锤「上拨变成拨动里面的 app」). 根治 = 4 行 tile 全部
    // 装进一屏 (TH 206→160, GAP_Y 16→12, 末行底 696 < 720), 页面不再滚动.
    lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_SCROLLABLE);

    // Tool card — same style as the HA "灯光" device cards: rounded card with a
    // soft shadow, icon on the left, name on the right. 3×3 网格铺满整屏
    // (2026-10-27 用户要求参考智能家居布局、少留白、上下对齐).
    // img==nullptr 走图形分支 (不依赖字体): kind==0 LED 点阵 (灯阵) /
    // kind==1 信封 (邮件) / kind==2 NFC 圆环 (RFID).
    constexpr int TW = 296, TH = 160, GAP_X = 16, GAP_Y = 12, MARGIN = 24;  // 4 列 × 4 行一屏装下 (2026-11-05 TH 206→160 防滚动吞上拨)
    auto make_tile = [&](int col, int row, const lv_image_dsc_t* img, int img_w,
                         const char* label, lv_event_cb_t cb,
                         int img_scale = 256, int icon_x = 12,
                         int img_scale_y = -1,
                         int kind = 0) {
        const int x = MARGIN + col * (TW + GAP_X);
        const int y = 20 + row * (TH + GAP_Y);
        lv_obj_t* tile = lv_obj_create(_tools_page);
        lv_obj_set_size(tile, TW, TH);
        lv_obj_align(tile, LV_ALIGN_TOP_LEFT, x, y);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_shadow_width(tile, 18, 0);
        lv_obj_set_style_shadow_color(tile, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_opa(tile, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(tile, 4, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
        // 按下"放大"效果: 同底色描边外扩 (LVGL 按住自动置 PRESSED 态, 松开消失;
        // 与每日一句/股票卡的 outline 闪烁同观感, 2026-10-28 用户要求)
        lv_obj_set_style_outline_width(tile, 6, LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(tile, lv_color_hex(C_CARD), LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, this);

        lv_obj_t* name = lv_label_create(tile);
        lv_label_set_text(name, label);
        lv_obj_set_style_text_font(name, zh_font_30(), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
        // 内容成组水平居中: 文字宽 + 图标宽 + 间距, 整组 (TW-总宽)/2 起
        const char* name_txt = lv_label_get_text(name);
        const int name_w = (int)lv_txt_get_width(name_txt, (uint32_t)strlen(name_txt),
                                                 zh_font_30(), 0);
        constexpr int ICON_GAP = 20;

        if (img) {
            lv_obj_t* icon = lv_image_create(tile);
            lv_image_set_src(icon, img);
            if (img_scale_y > 0) {
                // Non-uniform scale (e.g. stretch a portrait icon to a square box).
                lv_image_set_scale_x(icon, img_scale);
                lv_image_set_scale_y(icon, img_scale_y);
            } else if (img_scale != 256) {
                lv_image_set_scale(icon, img_scale);
            }
            const int start = (TW - img_w - ICON_GAP - name_w) / 2;
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, start, 0);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, start + img_w + ICON_GAP, 0);
        } else {
            // kind 分支: 0=点阵 (灯阵) / 1=信封 (邮件) / 2=嵌套方框标签 (RFID) /
            // 3=走势 (自选股) / 4=日历 (万年历) / 5=计算器 / 6=硬币双箭头 (汇率) /
            // 7=尺子 (单位换算) / 8=电波 (LoRa) / 9=机器人 (Reachy).
            // 线条全部随主题强调色 (2026-10-27 起工具页图标全图形化, 静态图全弃用).
            if (kind == 1) {
                // 信封 (邮件 tile) — 圆角轮廓 + V 形封盖, 参考 icons8-邮件 描边款.
                const int EW = 84, EH = 56;
                const int ex = (TW - EW - ICON_GAP - name_w) / 2;
                const int ey = (TH - EH) / 2;
                tool_accent_frame(tile, C_ACCENT, ex, ey, EW, EH, 8, 6);
                // V 形封盖: 点位相对线对象原点 (线对象放在信封左上角)
                static const lv_point_precise_t flap[] = {{7, 4}, {42, 30}, {77, 4}};
                tool_accent_line(tile, C_ACCENT, ex, ey, flap, 3, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, ex + EW + ICON_GAP, 0);
            } else if (kind == 2) {
                // RFID 标签 — 三层嵌套圆角方框 + 中心芯片, 参考 icons8-rfid标签.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 8, top + 8, 80, 80, 10, 6);
                tool_accent_frame(tile, C_ACCENT, start + 22, top + 22, 52, 52, 8, 5);
                tool_accent_frame(tile, C_ACCENT, start + 34, top + 34, 28, 28, 5, 4);
                tool_accent_rect(tile, C_ACCENT, start + 43, top + 43, 10, 10, 2);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 3) {
                // 自选股 (行情走势) — L 形坐标轴 + 上升折线 + 终点圆点,
                // 线条同灯阵圆点一样取主题强调色 C_ACCENT. 图标盒 96x96.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                // 纵轴 / 横轴 (6px 粗)
                tool_accent_rect(tile, C_ACCENT, start, top, 6, 84, 3);
                tool_accent_rect(tile, C_ACCENT, start, top + 78, 84, 6, 3);
                // 上升折线 (点位相对线对象原点, 线对象放在图标盒 (12,6) 处)
                static const lv_point_precise_t trend_pts[] = {
                    {0, 52}, {15, 34}, {27, 42}, {46, 18}, {58, 26}, {74, 2}
                };
                tool_accent_line(tile, C_ACCENT, start + 12, top + 6, trend_pts, 6, 6);
                // 折线终点圆点 (终点绝对坐标 start+86, top+8)
                tool_accent_rect(tile, C_ACCENT, start + 80, top + 2, 12, 12, 6);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 4) {
                // 万年历 — 圆角日历框 + 顶部两个装订环 + 表头分隔线 + 日期圆点,
                // 线条同灯阵圆点一样取主题强调色 C_ACCENT. 图标盒 96x96.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int fx = start + 6, fy = top + 18, fw = 84, fh = 72;
                // 装订环 (先画, 下半截被日历框边框盖住, 露出上半截)
                tool_accent_rect(tile, C_ACCENT, fx + 14, top + 8, 8, 20, 4);
                tool_accent_rect(tile, C_ACCENT, fx + fw - 22, top + 8, 8, 20, 4);
                // 日历框: 圆角边框, 背景透明
                tool_accent_frame(tile, C_ACCENT, fx, fy, fw, fh, 14, 6);
                // 表头分隔线
                tool_accent_rect(tile, C_ACCENT, fx + 6, fy + 22, fw - 12, 4, 2);
                // 日期圆点 3x2
                for (int r = 0; r < 2; r++)
                    for (int c = 0; c < 3; c++)
                        tool_accent_rect(tile, C_ACCENT,
                                         fx + 14 + c * 22, fy + 36 + r * 16, 10, 10, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 5) {
                // 计算器 — 圆角机身 + 显示屏条 + 4x3 按键阵, 参考 icons8-计算器.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int bx = start + 16, by = top + 6;   // 机身 64x84
                tool_accent_frame(tile, C_ACCENT, bx, by, 64, 84, 12, 6);
                tool_accent_rect(tile, C_ACCENT, bx + 10, by + 10, 44, 13, 3);  // 显示屏
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 4; c++)
                        tool_accent_rect(tile, C_ACCENT,
                                         bx + 10 + c * 12, by + 31 + r * 13, 8, 8, 2);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 6) {
                // 汇率 (外币兑换) — 对角两枚硬币 + 上下反向箭头, 参考 icons8-外币兑换.
                // 币面不带字: 96px 盒里 30px 的 ￥ 过挤, 表意交给 tile 文字「汇率」.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 6, top + 8, 34, 34, 17, 6);
                tool_accent_frame(tile, C_ACCENT, start + 56, top + 54, 34, 34, 17, 6);
                // 上箭头 (向左) / 下箭头 (向右)
                tool_accent_rect(tile, C_ACCENT, start + 52, top + 10, 38, 5, 2);
                tool_accent_rect(tile, C_ACCENT, start + 6, top + 81, 38, 5, 2);
                // 箭头头部 (V 形折线, 点位相对线对象原点)
                static const lv_point_precise_t head_l[] = {{6, 0}, {0, 6}, {6, 12}};
                static const lv_point_precise_t head_r[] = {{0, 0}, {6, 6}, {0, 12}};
                tool_accent_line(tile, C_ACCENT, start + 52, top + 7, head_l, 3, 5);
                tool_accent_line(tile, C_ACCENT, start + 38, top + 78, head_r, 3, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 7) {
                // 单位换算 (尺子) — 圆角尺身 + 5 根刻度, 参考 icons8-尺子 (横放).
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 4, top + 26, 88, 44, 8, 6);
                for (int i = 0; i < 5; i++)
                    tool_accent_rect(tile, C_ACCENT,
                                     start + 12 + i * 16, top + 32, 5, 16, 2);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 8) {
                // LoRa Chat (无线电波) — 中心圆环 + 左右各两道弧, 参考 icons8-无线电波.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int icx = start + IW / 2, icy = TH / 2;
                tool_accent_frame(tile, C_ACCENT, icx - 9, icy - 9, 18, 18, 9, 6);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 30, 145, 215, 5);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 44, 150, 210, 5);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 30, -35, 35, 5);
                tool_accent_arc(tile, C_ACCENT, icx, icy, 44, -30, 30, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 9) {
                // Reachy (机器人头) — 圆角头框 + 双耳 + 双天线 + 双眼 + 嘴,
                // 参考 icons8-机器人.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int icx = start + IW / 2, icy = TH / 2;
                tool_accent_frame(tile, C_ACCENT, icx - 30, icy - 24, 60, 48, 12, 6);
                tool_accent_rect(tile, C_ACCENT, icx - 38, icy - 7, 8, 14, 3);   // 左耳
                tool_accent_rect(tile, C_ACCENT, icx + 30, icy - 7, 8, 14, 3);   // 右耳
                tool_accent_rect(tile, C_ACCENT, icx - 16, icy - 38, 4, 14, 2);  // 左天线
                tool_accent_rect(tile, C_ACCENT, icx + 12, icy - 38, 4, 14, 2);  // 右天线
                tool_accent_rect(tile, C_ACCENT, icx - 18, icy - 46, 8, 8, 4);   // 天线顶球
                tool_accent_rect(tile, C_ACCENT, icx + 10, icy - 46, 8, 8, 4);
                tool_accent_rect(tile, C_ACCENT, icx - 18, icy - 12, 8, 8, 2);   // 双眼
                tool_accent_rect(tile, C_ACCENT, icx + 10, icy - 12, 8, 8, 2);
                tool_accent_rect(tile, C_ACCENT, icx - 8, icy + 4, 16, 5, 2);    // 嘴
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 10) {
                // SSH 终端 — 终端窗框 + 标题条圆点 + ">_" 提示符,
                // 参考 icons8-控制台. 图标盒 96x96.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int fx = start + 10, fy = top + 8, fw = 76, fh = 80;
                tool_accent_frame(tile, C_ACCENT, fx, fy, fw, fh, 12, 6);        // 窗框
                tool_accent_rect(tile, C_ACCENT, fx + 8, fy + 10, 8, 8, 4);      // 标题点x3
                tool_accent_rect(tile, C_ACCENT, fx + 22, fy + 10, 8, 8, 4);
                tool_accent_rect(tile, C_ACCENT, fx + 36, fy + 10, 8, 8, 4);
                tool_accent_rect(tile, C_ACCENT, fx + 8, fy + 24, fw - 16, 4, 2);// 分隔线
                // ">": 两段折线 (点位相对线对象原点)
                static const lv_point_precise_t chevron_pts[] = {
                    {0, 0}, {12, 9}, {0, 18}
                };
                tool_accent_line(tile, C_ACCENT, fx + 14, fy + 36, chevron_pts, 3, 5);
                tool_accent_rect(tile, C_ACCENT, fx + 32, fy + 50, 22, 5, 2);    // "_"
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 11) {
                // 健康 — 预渲染实心心形 + ECG 镂空 (gen_health_icons.py, 96x96 A8,
                // 2026-10-31 折线版被用户嫌粗糙后重制), recolor 取主题强调色.
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                lv_obj_t* ic = lv_image_create(tile);
                lv_image_set_src(ic, &hl_tile_health);
                lv_obj_set_pos(ic, start, TH / 2 - IW / 2);
                lv_obj_set_style_image_recolor(ic, lv_color_hex(C_ACCENT), 0);
                lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
                lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 12) {
                // 双色球 — 票券圆角框 + 左区 3×2 六点 (红) + 中缝 + 右区 1 大点
                // (蓝), 参考 icons8-彩票. 2026-11-01 弃静态彩图改图形化随主题色
                // (与其他工具 tile 同风格).
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int fx = start + 4, fy = top + 16, fw = 88, fh = 64;
                tool_accent_frame(tile, C_ACCENT, fx, fy, fw, fh, 12, 6);   // 票券
                for (int r = 0; r < 2; r++)
                    for (int c = 0; c < 3; c++)
                        tool_accent_rect(tile, C_ACCENT,
                                         fx + 12 + c * 17, fy + 12 + r * 22, 12, 12, 6);
                tool_accent_rect(tile, C_ACCENT, fx + 65, fy + 10, 4, fh - 20, 2);  // 中缝
                tool_accent_rect(tile, C_ACCENT, fx + 72, fy + 24, 16, 16, 8);      // 蓝球
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 13) {
                // 图库 — 相框 + 双山 + 右上太阳 (参考通用"照片"意象, icons8 相框).
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int fx = start + 4, fy = top + 14, fw = 88, fh = 66;
                tool_accent_frame(tile, C_ACCENT, fx, fy, fw, fh, 10, 6);   // 相框
                // 太阳: 整圆环 (arc 0→360)
                tool_accent_arc(tile, C_ACCENT, fx + fw - 20, fy + 16, 8, 0, 360, 4);
                // 双山折线: 左小山起于框底, 峰接框右; 右大山更高的峰
                static const lv_point_precise_t hills[] = {
                    {0, 34}, {14, 20}, {28, 34}, {46, 12}, {70, 34}, {88, 34}
                };
                tool_accent_line(tile, C_ACCENT, fx, fy + 32, hills, 6, 5);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 14) {
                // 计时器 — 秒表: 顶部按钮 + 圆环表身 + 45° 指针 + 中心点
                // (参考 icons8-秒表; 2026-11-05 计时器入口从主页迁入工具页).
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                const int cx = start + 48, cy = top + 54;
                tool_accent_rect(tile, C_ACCENT, cx - 14, cy - 56, 28, 6, 3);  // 顶帽
                tool_accent_rect(tile, C_ACCENT, cx - 6, cy - 50, 12, 14, 4);  // 按钮颈
                tool_accent_frame(tile, C_ACCENT, cx - 36, cy - 36, 72, 72, 36, 6);  // 表身圆环
                // 指针: 中心 → 右上 45° (点位相对线对象原点, 全正坐标)
                static const lv_point_precise_t hand[] = {{0, 20}, {20, 0}};
                tool_accent_line(tile, C_ACCENT, cx - 2, cy - 20, hand, 2, 5);
                tool_accent_rect(tile, C_ACCENT, cx - 4, cy - 4, 8, 8, 4);     // 中心轴点
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 15) {
                // 游戏 — 手柄: 圆角横身 + 左十字键 + 右双钮
                // (2026-11-06 游戏合集 tile: 翻牌/星星/扫雷/数独)
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 4, top + 24, 88, 48, 20, 6);   // 身
                // 左十字 (中心 start+30, top+48)
                tool_accent_rect(tile, C_ACCENT, start + 22, top + 45, 16, 6, 3);   // 横
                tool_accent_rect(tile, C_ACCENT, start + 27, top + 40, 6, 16, 3);   // 竖
                // 右双钮
                tool_accent_rect(tile, C_ACCENT, start + 58, top + 38, 12, 12, 6);
                tool_accent_rect(tile, C_ACCENT, start + 74, top + 50, 12, 12, 6);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else if (kind == 16) {
                // 油管 — 圆角横框 + 播放三角 (2026-11-06 油管 tile: Pi 中继音频版)
                const int IW = 96;
                const int start = (TW - IW - ICON_GAP - name_w) / 2;
                const int top = TH / 2 - IW / 2;
                tool_accent_frame(tile, C_ACCENT, start + 4, top + 24, 88, 52, 14, 6);   // 框
                static const lv_point_precise_t tri[] = {
                    {0, 0}, {0, 20}, {17, 10}, {0, 0}
                };
                tool_accent_line(tile, C_ACCENT, start + 40, top + 40, tri, 4, 5);   // ▷
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start + IW + ICON_GAP, 0);
            } else {
                // 3x3 实心圆点网格, 象征 8x8 LED 矩阵缩略图.
                // 不用 LV_SYMBOL_BULLET (依赖字体覆盖, 16px montserrat 渲染可能出方框),
                // 直接画 9 个圆角矩形, 完全脱离字体. 9 个圆点既省资源也清晰.
                const int CELL = 24, GAP = 6, COLS = 3, ROWS = 3;
                const int GRID_W = COLS * CELL + (COLS - 1) * GAP;
                const int start_x = (TW - GRID_W - ICON_GAP - name_w) / 2;
                const int start_y = (TH - ROWS * CELL - (ROWS - 1) * GAP) / 2;
                for (int r = 0; r < ROWS; r++) {
                    for (int c = 0; c < COLS; c++) {
                        lv_obj_t* dot = lv_obj_create(tile);
                        lv_obj_set_size(dot, CELL, CELL);
                        lv_obj_set_pos(dot,
                            start_x + c * (CELL + GAP),
                            start_y + r * (CELL + GAP));
                        lv_obj_set_style_bg_color(dot, lv_color_hex(C_ACCENT), 0);
                        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                        lv_obj_set_style_radius(dot, CELL / 2, 0);  // 圆形
                        lv_obj_set_style_border_width(dot, 0, 0);
                        lv_obj_set_style_shadow_width(dot, 0, 0);
                        lv_obj_set_style_pad_all(dot, 0, 0);
                        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
                        // 关键: 9 个 dot 不吞 click, 冒泡到父 tile
                        // (灯阵 tile 也用同一份 make_tile, 之前不点就是这问题)
                        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);
                    }
                }
                lv_obj_align(name, LV_ALIGN_LEFT_MID, start_x + GRID_W + ICON_GAP, 0);
            }
        }
    };

#ifdef PLATFORM_BUILD_WEB
    // web 版工具页: 不移植 LoRa Chat / RFID / Reachy (PORT.A 外设与机器人),
    // 其余 9 个入口重排为 4×3 网格前 9 格
    make_tile(0, 0, nullptr, 130, "计算器",  _toolBtn_cb, 256, 12, -1, 5);
    make_tile(1, 0, nullptr, 130, "汇率",    _toolFx_cb, 256, 12, -1, 6);
    make_tile(2, 0, nullptr, 130, "单位换算", _toolUnit_cb, 256, 12, -1, 7);
    make_tile(3, 0, nullptr, 130, "健康", _toolHealth_cb, 256, 12, -1, 11);
    make_tile(0, 1, nullptr, 130, "邮  件", _toolMail_cb, 256, 12, -1, 1);
    make_tile(1, 1, nullptr, 130, "万年历", _toolCalendar_cb, 256, 12, -1, 4);
    make_tile(2, 1, nullptr, 130, "SSH", _toolSsh_cb, 256, 12, -1, 10);
    make_tile(3, 1, nullptr, 0, "双色球", _toolLottery_cb, 256, 12, -1, 12);
    make_tile(0, 2, nullptr, 130, "图  库", _toolGallery_cb, 256, 12, -1, 13);
    // (2026-11-05: 计时器入口自主页迁入工具页, web 版主页按钮位让给「想法」)
    make_tile(1, 2, nullptr, 130, "计时器", _toolTimer_cb, 256, 12, -1, 14);
#else
    // 行 0: 计算器 / 汇率 / 单位换算 — 图标全部走 make_tile 图形分支
    // (kind 5/6/7), 线条随主题强调色, 原静态图 calc_icon/fx_icon/unit_icon 已弃用.
    // (2026-10-30: 灯阵 tile 已随 app_unit_puzzle 整体移除)
    make_tile(0, 0, nullptr, 130, "计算器",  _toolBtn_cb, 256, 12, -1, 5);
    make_tile(1, 0, nullptr, 130, "汇率",    _toolFx_cb, 256, 12, -1, 6);
    make_tile(2, 0, nullptr, 130, "单位换算", _toolUnit_cb, 256, 12, -1, 7);
    // (2026-10-31: 健康 app — hermes /api/health iPhone 快捷指令推送数据,
    //  kind=11 心形+脉搏分支, 补 (3,0) 空位)
    make_tile(3, 0, nullptr, 130, "健康", _toolHealth_cb, 256, 12, -1, 11);

    // (2026-11-01: 双色球 app — hermes /api/ssq/history 开奖/走势/对奖,
    //  用户指定 icons8 彩票图标 (彩色票券+红蓝球, 保留原色不 recolor))
    make_tile(2, 2, nullptr, 0, "双色球", _toolLottery_cb, 256, 12, -1, 12);

    // 第 2 行第 1 列: LoRa 聊天 (Unit C6L SX1262, PORT.A UART) — kind=8 电波分支,
    // 线条随主题强调色 (原 lora_logo 静态图 + 手写 tile 块已弃用).
    make_tile(0, 1, nullptr, 130, "LoRa Chat", _toolLoraChat_cb, 256, 12, -1, 8);

    // 第 3 行: 邮件 (调 hermes :8766/unread_emails 显示未读列表), kind=1 信封分支
    make_tile(2, 1, nullptr, 130, "邮  件", _toolMail_cb, 256, 12, -1, 1);

    // (2026-10-28: 金融入口移到主页底部按钮, 工具页不再放)

    // 第 3 行第 1 列: Reachy 仪表盘 (调 Reachy :8042/api/* 等接口, 5 tab 只读)
    //   kind=9 机器人分支; 2026-10-27 用户要求由「YRobot 控制台」改名「Reachy」,
    //   原 reachy_icon 静态图弃用.
    make_tile(0, 2, nullptr, 130, "Reachy", _toolReachy_cb, 256, 12, -1, 9);
    //   kind=4 日历分支, 线条随主题强调色 (原 calendar_icon 静态图已弃用)
    //   2026-10-28: 移到 (3,1) 补金融入口移除后的空位
    make_tile(3, 1, nullptr, 130, "万年历", _toolCalendar_cb, 256, 12, -1, 4);

    // 第 2 行第 3 列: RFID 读写器 (Unit NFC / MFRC522, PORT A I2C 0x28)
    //   图标走 kind=2 分支: 3 个同心圆环 + 中心芯片, 象征 NFC 感应
    make_tile(1, 1, nullptr, 130, "RFID", _toolRfid_cb, 256, 12, -1, 2);

    // 第 3 行第 2 列: SSH 终端 (libssh 0.11.5 + mbedtls, 移植 Tab5_SSH_Client)
    //   图标走 kind=10 分支: 终端窗框 + ">_" 提示符
    make_tile(1, 2, nullptr, 130, "SSH", _toolSsh_cb, 256, 12, -1, 10);

    // 第 3 行第 4 列: 图库 (共享相簿「壁纸」浏览, hermes :8766/gallery/*)
    //   图标走 kind=13 分支: 相框 + 双山 + 太阳; 2026-11-02 补最后空位
    make_tile(3, 2, nullptr, 130, "照  片", _toolGallery_cb, 256, 12, -1, 13);

    // 第 4 行第 1 列: 计时器 (2026-11-05 入口从主页迁入, 主页按钮位让给「想法」;
    //   页面纵向可滚动, 第 4 行直接滚出来) 图标走 kind=14 秒表分支
    make_tile(0, 3, nullptr, 130, "计时器", _toolTimer_cb, 256, 12, -1, 14);

    // 第 4 行第 2 列: 游戏合集 (car-dash 4 个触屏小游戏移植: 翻牌/星星/扫雷/数独,
    //   2026-11-06) 图标走 kind=15 手柄分支
    make_tile(1, 3, nullptr, 130, "游 戏", _toolGames_cb, 256, 12, -1, 15);

    // 第 4 行第 3 列: 油管 (car-dash 油管面板移植, Pi 中继音频版, 2026-11-06)
    //   图标走 kind=16 播放键分支
    make_tile(2, 3, nullptr, 130, "油 管", _toolYt_cb, 256, 12, -1, 16);
#endif  // !PLATFORM_BUILD_WEB
}

void AppSettings::_toolBtn_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openCalculator();
}

void AppSettings::_toolLoraChat_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (self->_lora_chat_id > 0) {
        mooncake::GetMooncake().openApp(self->_lora_chat_id);
    }
}

void AppSettings::_toolFx_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openFx();
}

void AppSettings::_toolUnit_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openUnit();
}

void AppSettings::_toolMail_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openEmail();
}

void AppSettings::_toolStocks_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openStocks();
}

void AppSettings::openCalendar()
{
    if (_calendar_id > 0) {
        _tools_page = nullptr;   // 子 app 打开后本页随 _scr 一起由 onClose 清
        mooncake::GetMooncake().openApp(_calendar_id);
    }
}

void AppSettings::_toolCalendar_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openCalendar();
}

void AppSettings::openHealth()
{
    if (_health_id > 0) {
        _tools_page = nullptr;   // 子 app 打开后本页随 _scr 一起由 onClose 清
        mooncake::GetMooncake().openApp(_health_id);
    }
}

void AppSettings::_toolHealth_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openHealth();
}

void AppSettings::openLottery()
{
    if (_lottery_id > 0) {
        mooncake::GetMooncake().openApp(_lottery_id);
    }
}

void AppSettings::_toolLottery_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openLottery();
}

void AppSettings::openGallery()
{
    if (_gallery_id > 0) {
        mooncake::GetMooncake().openApp(_gallery_id);
    }
}

void AppSettings::_toolGallery_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openGallery();
}

void AppSettings::openTimer()
{
    if (_timer_id > 0) {
        _tools_page = nullptr;   // 子 app 打开后本页随 _scr 一起由 onClose 清
        mooncake::GetMooncake().openApp(_timer_id);
    }
}

void AppSettings::_toolTimer_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openTimer();
}

void AppSettings::openGames()
{
    if (_games_id > 0) {
        _tools_page = nullptr;   // 子 app 打开后本页随 _scr 一起由 onClose 清
        mooncake::GetMooncake().openApp(_games_id);
    }
}

void AppSettings::_toolGames_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openGames();
}

void AppSettings::openYt()
{
    if (_yt_id > 0) {
        _tools_page = nullptr;   // 子 app 打开后本页随 _scr 一起由 onClose 清
        mooncake::GetMooncake().openApp(_yt_id);
    }
}

void AppSettings::_toolYt_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openYt();
}

void AppSettings::openStocks()
{
    if (_stocks_id > 0) {
        mooncake::GetMooncake().openApp(_stocks_id);
    }
}

void AppSettings::_toolReachy_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->openReachy();
}

void AppSettings::openReachy()
{
    if (_reachy_id > 0) {
        mooncake::GetMooncake().openApp(_reachy_id);
    }
}

void AppSettings::_toolRfid_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (self->_rfid_id > 0) {
        mooncake::GetMooncake().openApp(self->_rfid_id);
    }
}

void AppSettings::_toolSsh_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (self->_ssh_id > 0) {
        mooncake::GetMooncake().openApp(self->_ssh_id);
    }
}


// ════════════════════════════════════════════════════════════════════════════
//  Calculator
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openCalculator()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _calc_page = lv_obj_create(_scr);
    lv_obj_set_size(_calc_page, 1280, 720);
    lv_obj_set_pos(_calc_page, 0, 0);
    lv_obj_set_style_bg_color(_calc_page, lv_color_hex(CALC_BG), 0);
    lv_obj_set_style_border_width(_calc_page, 0, 0);
    lv_obj_set_style_radius(_calc_page, 0, 0);
    lv_obj_set_style_pad_all(_calc_page, 0, 0);
    lv_obj_clear_flag(_calc_page, LV_OBJ_FLAG_SCROLLABLE);

    // ── Display: expression (small, grey) over result (big, white), right-aligned.
    _calc_expr_lbl = lv_label_create(_calc_page);
    lv_label_set_text(_calc_expr_lbl, "");
    lv_obj_set_style_text_font(_calc_expr_lbl, &font_calc_expr, 0);
    lv_obj_set_style_text_color(_calc_expr_lbl, lv_color_hex(CALC_EXPR), 0);
    lv_obj_align(_calc_expr_lbl, LV_ALIGN_TOP_RIGHT, -36, 86);

    _calc_res_lbl = lv_label_create(_calc_page);
    lv_label_set_text(_calc_res_lbl, "0");
    lv_obj_set_style_text_font(_calc_res_lbl, &font_calc_big, 0);
    lv_obj_set_style_text_color(_calc_res_lbl, lv_color_hex(CALC_DISP), 0);
    lv_obj_align(_calc_res_lbl, LV_ALIGN_TOP_RIGHT, -36, 130);

    // ── Button grid: 5 columns × 4 rows ──────────────────────────────────────
    struct Key { const char* label; const char* code; uint32_t color; bool sym; };
    // code is what _calcInput receives. "DEL"/"AC"/"PCT"/"NEG" are special.
    static const Key KEYS[4][5] = {
        {{"7","7",CALC_NUM,false},{"8","8",CALC_NUM,false},{"9","9",CALC_NUM,false},
         {LV_SYMBOL_BACKSPACE,"DEL",CALC_FUNC,true},{"\xC3\xB7","/",CALC_OP,false}},
        {{"4","4",CALC_NUM,false},{"5","5",CALC_NUM,false},{"6","6",CALC_NUM,false},
         {"AC","AC",CALC_FUNC,false},{"\xC3\x97","*",CALC_OP,false}},
        {{"1","1",CALC_NUM,false},{"2","2",CALC_NUM,false},{"3","3",CALC_NUM,false},
         {"%","PCT",CALC_FUNC,false},{"-","-",CALC_OP,false}},
        {{"+/-","NEG",CALC_NUM,false},{"0","0",CALC_NUM,false},{".",".",CALC_NUM,false},
         {"=","=",CALC_OP,false},{"+","+",CALC_OP,false}},
    };

    const int SIDE = 24, GAP = 16, COLS = 5, ROWS = 4;
    const int grid_top = 270;
    const int bw = (1280 - 2 * SIDE - (COLS - 1) * GAP) / COLS;   // ~233
    const int bh = (720 - grid_top - SIDE - (ROWS - 1) * GAP) / ROWS;  // ~95

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const Key& k = KEYS[r][c];
            lv_obj_t* b = lv_obj_create(_calc_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, SIDE + c * (bw + GAP), grid_top + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(k.color), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);   // pill
            lv_obj_set_style_border_width(b, 0, 0);
            // Pressed: lighten.
            lv_obj_set_style_bg_color(b,
                lv_color_hex(k.color == CALC_OP ? C_TEXT
                           : k.color == CALC_NUM ? 0x5A5A5C : 0x6E6E70),
                LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _calcKey_cb, LV_EVENT_CLICKED, this);

            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    _calcReset();
}

void AppSettings::_back_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeCalculator();
}

void AppSettings::_closeCalculator()
{
    if (_calc_page) {
        lv_obj_delete(_calc_page);
        _calc_page = nullptr;
        _calc_expr_lbl = _calc_res_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

// ── Calculator logic ─────────────────────────────────────────────────────────
namespace {
// Group the integer part of a numeric string with thousands separators,
// preserving sign and any fractional part / trailing dot as typed.
std::string group(const std::string& s)
{
    std::string sign, num = s;
    if (!num.empty() && (num[0] == '-' || num[0] == '+')) { sign = num.substr(0, 1); num = num.substr(1); }
    size_t dot = num.find('.');
    std::string ip = (dot == std::string::npos) ? num : num.substr(0, dot);
    std::string fp = (dot == std::string::npos) ? ""  : num.substr(dot);  // includes '.'
    std::string out;
    int cnt = 0;
    for (int i = (int)ip.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), ip[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    if (ip.empty()) out = "0";
    return sign + out + fp;
}

// Format a double for display: up to 10 significant digits, trailing zeros
// trimmed, then thousands-grouped.
std::string fmtNum(double v)
{
    if (std::isnan(v) || std::isinf(v)) return "Error";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.10g", v);
    return group(buf);
}

// Currency-converter formatter: at most 3 decimal places, trailing zeros
// trimmed, then thousands-grouped. Keeps the result line short (full-precision
// values like 0.1476688479 otherwise overflow the card).
std::string fxFmt(double v)
{
    if (std::isnan(v) || std::isinf(v)) return "Error";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        size_t e = s.size();
        while (e > 0 && s[e - 1] == '0') e--;
        if (e > 0 && s[e - 1] == '.') e--;
        s = s.substr(0, e);
    }
    return group(s);
}

double applyOp(double a, double b, char op)
{
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0.0 ? a / b : NAN;
    }
    return b;
}

const char* opSym(char op)
{
    switch (op) {
        case '+': return "+";
        case '-': return "-";
        case '*': return "\xC3\x97";  // ×
        case '/': return "\xC3\xB7";  // ÷
    }
    return "";
}
}  // namespace

void AppSettings::_calcReset()
{
    _entry = "0";
    _acc   = 0.0;
    _op    = 0;
    _fresh = true;
    _lhs_disp.clear();
    _calcRefresh();
}

void AppSettings::_calcRefresh()
{
    if (_calc_res_lbl)  lv_label_set_text(_calc_res_lbl, group(_entry).c_str());
    if (_calc_expr_lbl) {
        std::string ex;
        if (_op) ex = _lhs_disp + opSym(_op) + (_fresh ? "" : group(_entry));
        lv_label_set_text(_calc_expr_lbl, ex.c_str());
    }
}

void AppSettings::_calcInput(const char* key)
{
    std::string k = key;

    if (k == "AC") { _calcReset(); return; }

    if (k == "DEL") {
        if (!_fresh && _entry != "0") {
            _entry.pop_back();
            if (_entry.empty() || _entry == "-") _entry = "0";
        }
        _calcRefresh();
        return;
    }

    if (k == "NEG") {
        if (_entry != "0") {
            if (_entry[0] == '-') _entry = _entry.substr(1);
            else                  _entry = "-" + _entry;
        }
        _calcRefresh();
        return;
    }

    if (k == "PCT") {
        double v = atof(_entry.c_str()) / 100.0;
        _entry = fmtNum(v);
        // strip grouping commas back out so further edits stay numeric
        std::string t; for (char ch : _entry) if (ch != ',') t += ch;
        _entry = t;
        _fresh = true;
        _calcRefresh();
        return;
    }

    if (k == ".") {
        if (_fresh) { _entry = "0."; _fresh = false; }
        else if (_entry.find('.') == std::string::npos) _entry += ".";
        _calcRefresh();
        return;
    }

    // Digit 0-9
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') {
        if (_fresh) { _entry = k; _fresh = false; }
        else if (_entry == "0") _entry = k;
        else if (_entry == "-0") _entry = "-" + k;
        else _entry += k;
        _calcRefresh();
        return;
    }

    // Operators + - * /
    if (k == "+" || k == "-" || k == "*" || k == "/") {
        if (_op && !_fresh) {
            _acc = applyOp(_acc, atof(_entry.c_str()), _op);
        } else if (!_op) {
            _acc = atof(_entry.c_str());
        }
        _op = k[0];
        _lhs_disp = fmtNum(_acc);
        _entry = fmtNum(_acc);
        { std::string t; for (char ch : _entry) if (ch != ',') t += ch; _entry = t; }
        _fresh = true;
        _calcRefresh();
        return;
    }

    // Equals
    if (k == "=") {
        if (_op) {
            double rhs = atof(_entry.c_str());
            std::string rhs_disp = group(_entry);
            double res = applyOp(_acc, rhs, _op);
            // Show full "lhs op rhs" expression, result below.
            if (_calc_expr_lbl)
                lv_label_set_text(_calc_expr_lbl, (_lhs_disp + opSym(_op) + rhs_disp).c_str());
            _entry = fmtNum(res);
            { std::string t; for (char ch : _entry) if (ch != ',') t += ch; _entry = t; }
            if (_calc_res_lbl) lv_label_set_text(_calc_res_lbl, group(_entry).c_str());
            _acc = res;
            _op = 0;
            _fresh = true;
        }
        return;
    }
}

void AppSettings::_calcKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_calcInput(code);
}

// ════════════════════════════════════════════════════════════════════════════
//  Currency converter (汇率)
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openFx()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _fx_page = lv_obj_create(_scr);
    lv_obj_set_size(_fx_page, 1280, 720);
    lv_obj_set_pos(_fx_page, 0, 0);
    lv_obj_set_style_bg_color(_fx_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_fx_page, 0, 0);
    lv_obj_set_style_radius(_fx_page, 0, 0);
    lv_obj_set_style_pad_all(_fx_page, 0, 0);
    lv_obj_clear_flag(_fx_page, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部标题已去掉 (2026-10-28 用户要求, 同工具页去「工具」标题), 内容整体上移.

    // Currency code options for the dropdowns (ASCII → renders in montserrat).
    std::string opts;
    for (int i = 0; i < FX_N; i++) { if (i) opts += "\n"; opts += FX[i].code; }

    // ── Conversion panel (left) ──────────────────────────────────────────────
    const int CARD_X = 40, CARD_W = 600, CARD_H = 120;

    auto make_conv_card = [&](int y, bool is_from) -> lv_obj_t* {
        lv_obj_t* c = lv_obj_create(_fx_page);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, CARD_X, y);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_shadow_width(c, 18, 0);
        lv_obj_set_style_shadow_color(c, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dd = lv_dropdown_create(c);
        lv_obj_set_size(dd, 130, 60);
        lv_obj_align(dd, LV_ALIGN_LEFT_MID, 16, 0);
        lv_dropdown_set_options(dd, opts.c_str());
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_30, 0);
        lv_obj_t* dd_list = lv_dropdown_get_list(dd);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_30, 0);
        lv_obj_add_event_cb(dd, _fxDd_cb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t* val = lv_label_create(c);
        lv_label_set_text(val, "0");
        lv_obj_set_style_text_font(val, &font_calc_op, 0);
        lv_obj_set_style_text_color(val,
            lv_color_hex(is_from ? CALC_DISP : C_ACCENT), 0);   // FROM 金额在浅卡上随主题
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0);

        if (is_from) { _fx_from_dd = dd; _fx_amt_lbl = val; }
        else         { _fx_to_dd   = dd; _fx_res_lbl = val; }
        return c;
    };

    make_conv_card(90, true);
    make_conv_card(330, false);

    // Swap button centered in the gap between the two cards.
    // FROM card: 90..(90+CARD_H); TO card starts at 330 → gap centre = 270,
    // 上下各留 16px (2026-10-28 用户: 不要紧挨着).
    lv_obj_t* swap = lv_obj_create(_fx_page);
    lv_obj_set_size(swap, 88, 88);   // 64→88: 圆和图标都放大 (2026-10-28 用户)
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 44, 270 - 44);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _fxSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(swap_lbl, &lv_font_montserrat_38, 0);
    lv_obj_center(swap_lbl);
    lv_obj_set_style_text_color(swap_lbl, lv_color_hex(0xFFFFFF), 0);

    // Rate line.
    _fx_rate_lbl = lv_label_create(_fx_page);
    lv_label_set_text(_fx_rate_lbl, "");
    lv_obj_set_pos(_fx_rate_lbl, CARD_X + 6, 470);
    lv_obj_set_style_text_color(_fx_rate_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_fx_rate_lbl, zh_font_30(), 0);

    // ── Keypad (right) ───────────────────────────────────────────────────────
    const int KX = 700, KW = 540, GAP = 16, COLS = 3, ROWS = 4;

    // AC bar above the keypad.
    lv_obj_t* ac = lv_obj_create(_fx_page);
    lv_obj_set_size(ac, KW, 60);
    lv_obj_set_pos(ac, KX, 40);
    lv_obj_set_style_bg_color(ac, lv_color_hex(CALC_FUNC), 0);
    lv_obj_set_style_radius(ac, 16, 0);
    lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_bg_color(ac, lv_color_hex(0x6E6E70), LV_STATE_PRESSED);
    lv_obj_clear_flag(ac, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ac, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ac, (void*)"AC");
    lv_obj_add_event_cb(ac, _fxKey_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* ac_lbl = lv_label_create(ac);
    lv_label_set_text(ac_lbl, "AC");
    lv_obj_center(ac_lbl);
    lv_obj_set_style_text_color(ac_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ac_lbl, &font_calc_op, 0);

    struct FKey { const char* label; const char* code; bool sym; };
    static const FKey FKEYS[4][3] = {
        {{"7","7",false},{"8","8",false},{"9","9",false}},
        {{"4","4",false},{"5","5",false},{"6","6",false}},
        {{"1","1",false},{"2","2",false},{"3","3",false}},
        {{".",".",false},{"0","0",false},{LV_SYMBOL_BACKSPACE,"DEL",true}},
    };
    const int ktop = 120;
    const int bw = (KW - (COLS - 1) * GAP) / COLS;
    const int bh = (720 - ktop - 30 - (ROWS - 1) * GAP) / ROWS;
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            const FKey& k = FKEYS[r][col];
            lv_obj_t* b = lv_obj_create(_fx_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, KX + col * (bw + GAP), ktop + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(CALC_NUM), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5C), LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _fxKey_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    // Defaults: 1 USD → CNY.
    _fx_entry = "1";
    lv_dropdown_set_selected(_fx_from_dd, 0);
    lv_dropdown_set_selected(_fx_to_dd, 1);
    _fxRecompute();

    // Pull live rates from the HA server in the background (falls back to the
    // static table if the server is unreachable).
    _fxFetch();
}

void AppSettings::_fxFetch()
{
    if (g_fx_fetching.exchange(true)) return;  // already in flight

    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.200");   // 默认与主页一致 (模拟器无 NVS 配置)
    std::string url  = "http://" + host + ":8766/rate";

    bool spawned = GetHAL()->tryRunDetached([url]() {
        auto resp = GetHAL()->httpGet(url);
        if (resp.ok) {
            try {
                auto j = PsramJson::parse(resp.body);
                if (j.value("ok", false) && j.contains("rates")) {
                    const auto& rates = j["rates"];
                    std::lock_guard<std::mutex> lk(g_fx_mutex);
                    for (int i = 0; i < FX_N; i++) {
                        auto it = rates.find(FX[i].code);
                        if (it != rates.end() && it->is_number()) {
                            double v = it->get<double>();
                            if (v > 0) g_fx_rate[i] = v;
                        }
                    }
                    g_fx_source = j.value("source", std::string("?"));
                    g_fx_updated.store(true);  // UI loop will recompute
                    mclog::tagInfo(_tag, "fx rates updated ({})", g_fx_source.c_str());
                }
            } catch (...) {
                mclog::tagWarn(_tag, "fx parse failed");
            }
        } else {
            mclog::tagWarn(_tag, "fx fetch failed (keeping static rates)");
        }
        g_fx_fetching.store(false);
    });
    if (!spawned) g_fx_fetching.store(false);
}

void AppSettings::_fxBack_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeFx();
}

void AppSettings::_closeFx()
{
    if (_fx_page) {
        lv_obj_delete(_fx_page);
        _fx_page = nullptr;
        _fx_from_dd = _fx_to_dd = _fx_amt_lbl = _fx_res_lbl = _fx_rate_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

void AppSettings::_fxRecompute()
{
    if (!_fx_from_dd || !_fx_to_dd) return;
    int fi = lv_dropdown_get_selected(_fx_from_dd);
    int ti = lv_dropdown_get_selected(_fx_to_dd);
    if (fi < 0 || fi >= FX_N) fi = 0;
    if (ti < 0 || ti >= FX_N) ti = 0;

    double rf, rt;
    {
        std::lock_guard<std::mutex> lk(g_fx_mutex);
        rf = g_fx_rate[fi];
        rt = g_fx_rate[ti];
    }
    if (rf <= 0) rf = 1;
    double amt = atof(_fx_entry.c_str());
    double res = amt / rf * rt;

    if (_fx_amt_lbl) lv_label_set_text(_fx_amt_lbl, group(_fx_entry).c_str());
    if (_fx_res_lbl) lv_label_set_text(_fx_res_lbl, fxFmt(res).c_str());
    if (_fx_rate_lbl) {
        double one = rt / rf;  // 1 FROM = one TO
        char buf[96];
        snprintf(buf, sizeof(buf), "1 %s = %s %s", FX[fi].cn, fxFmt(one).c_str(), FX[ti].cn);
        lv_label_set_text(_fx_rate_lbl, buf);
    }
}

void AppSettings::_fxInput(const char* key)
{
    std::string k = key;
    if (k == "AC") {
        _fx_entry = "0";
    } else if (k == "DEL") {
        if (!_fx_entry.empty()) _fx_entry.pop_back();
        if (_fx_entry.empty()) _fx_entry = "0";
    } else if (k == ".") {
        if (_fx_entry.find('.') == std::string::npos) _fx_entry += ".";
    } else {  // digit
        if (_fx_entry == "0") _fx_entry = k;
        else if (_fx_entry.size() < 12) _fx_entry += k;
    }
    _fxRecompute();
}

void AppSettings::_fxKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_fxInput(code);
}

void AppSettings::_fxDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_fxRecompute();
}

void AppSettings::_fxSwap_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self->_fx_from_dd || !self->_fx_to_dd) return;
    int fi = lv_dropdown_get_selected(self->_fx_from_dd);
    int ti = lv_dropdown_get_selected(self->_fx_to_dd);
    lv_dropdown_set_selected(self->_fx_from_dd, ti);
    lv_dropdown_set_selected(self->_fx_to_dd, fi);
    self->_fxRecompute();
}

// ════════════════════════════════════════════════════════════════════════════
//  Unit converter (单位换算)
// ════════════════════════════════════════════════════════════════════════════

struct UnitCat {
    const char* name;
    struct Unit {
        const char* code;       // abbreviation shown in dropdown
        const char* label;      // full name for the equation line
        double to_base;         // multiply to get base-unit value
    };
    const Unit* units;
    int n_units;
};

// Conversion factors relative to the base unit of each category.
// Temperature is handled separately (not a linear factor).

static const UnitCat::Unit UNITS_LENGTH[] = {
    {"mm",  "毫米",  0.001},
    {"cm",  "厘米",  0.01},
    {"m",   "米",    1.0},
    {"km",  "千米",  1000.0},
    {"in",  "英寸",  0.0254},
    {"ft",  "英尺",  0.3048},
    {"yd",  "码",    0.9144},
    {"mi",  "英里",  1609.344},
};
static const UnitCat::Unit UNITS_WEIGHT[] = {
    {"mg",  "毫克",  0.001},
    {"g",   "克",    1.0},
    {"kg",  "千克",  1000.0},
    {"t",   "吨",    1e6},
    {"oz",  "盎司",  28.3495},
    {"lb",  "磅",    453.592},
};
static const UnitCat::Unit UNITS_TEMP[] = {
    {"°C",  "摄氏度", 0},
    {"°F",  "华氏度", 0},
    {"K",   "开尔文", 0},
};
static const UnitCat::Unit UNITS_AREA[] = {
    {"mm²", "平方毫米", 1e-6},
    {"cm²", "平方厘米", 0.0001},
    {"m²",  "平方米",   1.0},
    {"km²", "平方公里",  1e6},
    {"ha",  "公顷",     10000.0},
    {"亩",  "亩",       666.6667},
    {"acre","英亩",     4046.86},
    {"ft²", "平方英尺", 0.092903},
};
static const UnitCat::Unit UNITS_VOLUME[] = {
    {"mL",  "毫升",   0.001},
    {"L",   "升",     1.0},
    {"m³",  "立方米", 1000.0},
    {"gal", "加仑",   3.78541},
    {"cup", "杯",     0.236588},
};
static const UnitCat::Unit UNITS_SPEED[] = {
    {"m/s",  "米/秒",    1.0},
    {"km/h", "千米/时",  0.277778},
    {"mph",  "英里/时",  0.44704},
    {"kn",   "节",       0.514444},
};
static const UnitCat::Unit UNITS_DATA[] = {
    {"B",   "字节", 1.0},
    {"KB",  "千字节", 1024.0},
    {"MB",  "兆字节", 1048576.0},
    {"GB",  "吉字节", 1073741824.0},
    {"TB",  "太字节", 1099511627776.0},
};

static const UnitCat UNIT_CATS[] = {
    {"长度", UNITS_LENGTH, 8},
    {"重量", UNITS_WEIGHT, 6},
    {"温度", UNITS_TEMP,   3},
    {"面积", UNITS_AREA,   8},
    {"体积", UNITS_VOLUME, 5},
    {"速度", UNITS_SPEED,  4},
    {"数据", UNITS_DATA,   5},
};
static constexpr int UNIT_CAT_N = sizeof(UNIT_CATS) / sizeof(UNIT_CATS[0]);

static std::string unitFmt(double v)
{
    if (fabs(v) < 1e-12) return "0";
    char buf[32];
    if (fabs(v) >= 1e9)
        snprintf(buf, sizeof(buf), "%.8g", v);
    else if (fabs(v) < 0.001 && v != 0)
        snprintf(buf, sizeof(buf), "%.8g", v);
    else
        snprintf(buf, sizeof(buf), "%.3f", v);
    // Trim trailing zeros after decimal (but keep at least one decimal)
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.size() > 2 && s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

void AppSettings::_openUnit()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _unit_page = lv_obj_create(_scr);
    lv_obj_set_size(_unit_page, 1280, 720);
    lv_obj_set_pos(_unit_page, 0, 0);
    lv_obj_set_style_bg_color(_unit_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_unit_page, 0, 0);
    lv_obj_set_style_radius(_unit_page, 0, 0);
    lv_obj_set_style_pad_all(_unit_page, 0, 0);
    lv_obj_clear_flag(_unit_page, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部标题已去掉 (2026-10-28 用户要求, 同工具页), 内容整体上移.

    // Category dropdown — left-aligned so it doesn't overlap the AC bar.
    std::string cat_opts;
    for (int i = 0; i < UNIT_CAT_N; i++) {
        if (i) cat_opts += "\n";
        cat_opts += UNIT_CATS[i].name;
    }
    _unit_cat_dd = lv_dropdown_create(_unit_page);
    lv_obj_set_size(_unit_cat_dd, 420, 60);
    lv_obj_set_pos(_unit_cat_dd, 40, 46);
    lv_dropdown_set_options(_unit_cat_dd, cat_opts.c_str());
    lv_dropdown_set_symbol(_unit_cat_dd, nullptr);   // 默认 FA 箭头 zh 字体无字形 (万年历同款坑)
    lv_obj_set_style_text_font(_unit_cat_dd, zh_font_30(), 0);
    lv_dropdown_set_selected(_unit_cat_dd, 0);
    _unit_cat_idx = 0;
    // The dropdown list is a separate object — must set its font too.
    lv_obj_t* cat_list = lv_dropdown_get_list(_unit_cat_dd);
    lv_obj_set_style_text_font(cat_list, zh_font_30(), 0);
    lv_obj_add_event_cb(_unit_cat_dd, _unitCatDd_cb, LV_EVENT_VALUE_CHANGED, this);

    // ── Conversion panel (left) ──────────────────────────────────────────────
    const int CARD_X = 40, CARD_W = 580, CARD_H = 120;
    const int KEY_X = 660, KEY_W = 580;

    _unit_from_dd = nullptr;
    _unit_to_dd   = nullptr;
    _unit_amt_lbl = nullptr;
    _unit_res_lbl = nullptr;

    auto make_card = [&](int y, bool is_from) -> lv_obj_t* {
        lv_obj_t* c = lv_obj_create(_unit_page);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, CARD_X, y);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_shadow_width(c, 18, 0);
        lv_obj_set_style_shadow_color(c, lv_color_hex(C_SHADOW), 0);
        lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dd = lv_dropdown_create(c);
        lv_obj_set_size(dd, 140, 60);
        lv_obj_align(dd, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_30, 0);
        // Dropdown list needs its own font (otherwise defaults to montserrat).
        lv_obj_t* dd_list = lv_dropdown_get_list(dd);
        lv_obj_set_style_text_font(dd_list, &lv_font_montserrat_30, 0);

        lv_obj_t* val = lv_label_create(c);
        lv_label_set_text(val, "0");
        lv_obj_set_style_text_font(val, &font_calc_op, 0);
        lv_obj_set_style_text_color(val,
            lv_color_hex(is_from ? CALC_DISP : C_ACCENT), 0);   // FROM 金额在浅卡上随主题
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0);

        if (is_from) {
            _unit_from_dd = dd;
            _unit_amt_lbl = val;
            lv_obj_add_event_cb(dd, _unitFromDd_cb, LV_EVENT_VALUE_CHANGED, this);
        } else {
            _unit_to_dd = dd;
            _unit_res_lbl = val;
            lv_obj_add_event_cb(dd, _unitToDd_cb, LV_EVENT_VALUE_CHANGED, this);
        }
        return c;
    };

    make_card(150, true);
    make_card(390, false);

    // Swap button. FROM card bottom = 180+120=300, TO card top = 400.
    // Gap centre = (300+400)/2 = 350.  64 px button centred at y=318.
    lv_obj_t* swap = lv_obj_create(_unit_page);
    lv_obj_set_size(swap, 88, 88);
    // Swap 上下各留 16px (FROM 卡底 270 / TO 卡顶 390, 间隙中点 330).
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 44, 286);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _unitSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_font(swap_lbl, &lv_font_montserrat_38, 0);
    lv_obj_center(swap_lbl);
    lv_obj_set_style_text_color(swap_lbl, lv_color_hex(0xFFFFFF), 0);

    // Equivalence line.
    _unit_eq_lbl = lv_label_create(_unit_page);
    lv_label_set_text(_unit_eq_lbl, "");
    lv_obj_set_pos(_unit_eq_lbl, CARD_X + 6, 545);
    lv_obj_set_style_text_color(_unit_eq_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_unit_eq_lbl, zh_font_30(), 0);

    // ── Keypad (right) ───────────────────────────────────────────────────────
    const int GAP = 16, COLS = 3, ROWS = 4;

    lv_obj_t* ac = lv_obj_create(_unit_page);
    lv_obj_set_size(ac, KEY_W, 60);
    lv_obj_set_pos(ac, KEY_X, 46);
    lv_obj_set_style_bg_color(ac, lv_color_hex(CALC_FUNC), 0);
    lv_obj_set_style_radius(ac, 16, 0);
    lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_bg_color(ac, lv_color_hex(0x6E6E70), LV_STATE_PRESSED);
    lv_obj_clear_flag(ac, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ac, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ac, (void*)"AC");
    lv_obj_add_event_cb(ac, _unitKey_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* ac_lbl = lv_label_create(ac);
    lv_label_set_text(ac_lbl, "AC");
    lv_obj_center(ac_lbl);
    lv_obj_set_style_text_color(ac_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ac_lbl, &font_calc_op, 0);

    struct UKey { const char* label; const char* code; bool sym; };
    static const UKey UKEYS[4][3] = {
        {{"7","7",false},{"8","8",false},{"9","9",false}},
        {{"4","4",false},{"5","5",false},{"6","6",false}},
        {{"1","1",false},{"2","2",false},{"3","3",false}},
        {{".",".",false},{"0","0",false},{LV_SYMBOL_BACKSPACE,"DEL",true}},
    };
    const int ktop = 120;
    const int bw = (KEY_W - (COLS - 1) * GAP) / COLS;
    const int bh = (720 - ktop - 30 - (ROWS - 1) * GAP) / ROWS;
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            const UKey& k = UKEYS[r][col];
            lv_obj_t* b = lv_obj_create(_unit_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, KEY_X + col * (bw + GAP), ktop + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(CALC_NUM), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5C), LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _unitKey_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    // Populate unit dropdowns for the initial category.
    _unitPopulateUnits();
    _unit_entry = "1";
    _unitRecompute();
}

void AppSettings::_unitPopulateUnits()
{
    int ci = _unit_cat_idx;
    if (ci < 0 || ci >= UNIT_CAT_N) ci = 0;
    const UnitCat& cat = UNIT_CATS[ci];

    std::string opts;
    for (int i = 0; i < cat.n_units; i++) {
        if (i) opts += "\n";
        opts += cat.units[i].code;
    }

    if (_unit_from_dd) {
        lv_dropdown_set_options(_unit_from_dd, opts.c_str());
        lv_dropdown_set_selected(_unit_from_dd, 0);
    }
    if (_unit_to_dd) {
        lv_dropdown_set_options(_unit_to_dd, opts.c_str());
        lv_dropdown_set_selected(_unit_to_dd, cat.n_units > 1 ? 1 : 0);
    }
}

void AppSettings::_unitRecompute()
{
    if (!_unit_from_dd || !_unit_to_dd) return;
    int ci = _unit_cat_idx;
    if (ci < 0 || ci >= UNIT_CAT_N) ci = 0;
    const UnitCat& cat = UNIT_CATS[ci];

    int fi = lv_dropdown_get_selected(_unit_from_dd);
    int ti = lv_dropdown_get_selected(_unit_to_dd);
    if (fi < 0 || fi >= cat.n_units) fi = 0;
    if (ti < 0 || ti >= cat.n_units) ti = 0;

    double amt = atof(_unit_entry.c_str());

    double res;
    if (ci == 2) {
        // Temperature — special handling
        // Convert from unit fi to °C, then to unit ti.
        double celsius;
        if (fi == 0)      celsius = amt;              // °C → °C
        else if (fi == 1) celsius = (amt - 32) / 1.8; // °F → °C
        else              celsius = amt - 273.15;       // K → °C

        if (ti == 0)      res = celsius;
        else if (ti == 1) res = celsius * 1.8 + 32;
        else              res = celsius + 273.15;
    } else {
        // Linear conversion: value × to_base / from_to_base
        double f_factor = cat.units[fi].to_base;
        double t_factor = cat.units[ti].to_base;
        if (f_factor <= 0) f_factor = 1;
        res = amt * f_factor / t_factor;
    }

    if (_unit_amt_lbl) lv_label_set_text(_unit_amt_lbl, group(_unit_entry).c_str());
    if (_unit_res_lbl) lv_label_set_text(_unit_res_lbl, unitFmt(res).c_str());
    if (_unit_eq_lbl) {
        if (ci == 2) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s = %s %s",
                group(_unit_entry).c_str(), cat.units[fi].label,
                unitFmt(res).c_str(), cat.units[ti].label);
            lv_label_set_text(_unit_eq_lbl, buf);
        } else {
            double f_factor = cat.units[fi].to_base;
            double t_factor = cat.units[ti].to_base;
            if (f_factor <= 0) f_factor = 1;
            double one = f_factor / t_factor;
            char buf[96];
            snprintf(buf, sizeof(buf), "1 %s = %s %s",
                cat.units[fi].label, unitFmt(one).c_str(), cat.units[ti].label);
            lv_label_set_text(_unit_eq_lbl, buf);
        }
    }
}

void AppSettings::_unitInput(const char* key)
{
    std::string k = key;
    if (k == "AC") {
        _unit_entry = "0";
    } else if (k == "DEL") {
        if (!_unit_entry.empty()) _unit_entry.pop_back();
        if (_unit_entry.empty()) _unit_entry = "0";
    } else if (k == ".") {
        if (_unit_entry.find('.') == std::string::npos) _unit_entry += ".";
    } else {  // digit
        if (_unit_entry == "0") _unit_entry = k;
        else if (_unit_entry.size() < 14) _unit_entry += k;
    }
    _unitRecompute();
}

void AppSettings::_closeUnit()
{
    if (_unit_page) {
        lv_obj_delete(_unit_page);
        _unit_page = nullptr;
        _unit_cat_dd = _unit_from_dd = _unit_to_dd = nullptr;
        _unit_amt_lbl = _unit_res_lbl = _unit_eq_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

// ── Static callbacks ───────────────────────────────────────────────────────

void AppSettings::_unitKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_unitInput(code);
}

void AppSettings::_unitCatDd_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    self->_unit_cat_idx = lv_dropdown_get_selected(self->_unit_cat_dd);
    self->_unitPopulateUnits();
    self->_unit_entry = "1";
    self->_unitRecompute();
}

void AppSettings::_unitFromDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_unitRecompute();
}

void AppSettings::_unitToDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_unitRecompute();
}

void AppSettings::_unitSwap_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self->_unit_from_dd || !self->_unit_to_dd) return;
    int fi = lv_dropdown_get_selected(self->_unit_from_dd);
    int ti = lv_dropdown_get_selected(self->_unit_to_dd);
    lv_dropdown_set_selected(self->_unit_from_dd, ti);
    lv_dropdown_set_selected(self->_unit_to_dd, fi);
    self->_unitRecompute();
}

void AppSettings::_unitBack_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeUnit();
}

// ════════════════════════════════════════════════════════════════════════════
//  Email client (邮件) — 2026-11-15 oneapp(CardputerZero) 同款五层客户端:
//  账号(多账号+未读角标+二段确认删除) / 登录(邮箱+授权码) / 文件夹(聚合虚拟夹
//  置顶+真实夹) / 列表(滑动行 红旗/已读 + 点面卡进详情) / 详情(正文滚动+
//  标已读/旗标+回复) / 写信(收件人/主题/正文 + 回复引用块, SMTP 直发)。
//  协议层 = Pi 8768 /api/mail/* (mail_client.py, oneapp mail.py 移植:
//  授权码 IMAP 收 + SMTP 发, 会话复用, GBK/RFC2047/UTF-7 服务端全解)。
//  主页信封角标仍走 fetchEmail() (/api/email/status) 不变。
//  层间导航: 返回钮/上拨逐层回退 (oneapp mail_esc 同款), 账号层回工具页;
//  事件回调里不同步删事件目标祖先 — 层切换记 _mail_pending 下一帧执行。
// ════════════════════════════════════════════════════════════════════════════

namespace {

// 通用点击上下文 (heap 分配, 挂 user_data, DELETE 事件释放):
//   账号行 a=email / 面卡 a=uid b=fld / 文件夹行 a=raw b=disp /
//   动作钮 a=action b=folder(raw) c=uid / 删除 chip a=email
struct MailCtx {
    AppSettings* self;
    std::string a, b, c;
};

void mail_ctx_del_cb(lv_event_t* e)
{
    delete static_cast<MailCtx*>(lv_event_get_user_data(e));
}

// PsramString → std::string (worker 解析取值; 桌面/网页回退标准 json)
std::string mc_s(const PsramString& s) { return std::string(s.data(), s.size()); }

// worker 通用 POST /api/mail/*: HTTP/JSON/业务失败统一归 false + 用户可读文案
struct McResp {
    bool ok = false;
    std::string err;
    PsramJson j;
};

McResp mc_post(const char* path, const std::string& body)
{
    McResp r;
    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.200");
    std::string url = "http://" + host + ":8768" + path;
    auto resp = GetHAL()->httpPost(url, body, {{"Content-Type", "application/json"}});
    if (!resp.ok) {
        r.err = resp.status == 0 ? "网络/timeout (检查 8768)"
                                : ("HTTP " + std::to_string(resp.status));
        return r;
    }
    try {
        r.j = PsramJson::parse(resp.body);
    } catch (const std::exception& ex) {
        r.err = std::string("JSON 解析失败: ") + ex.what();
        return r;
    }
    r.ok = r.j.value("ok", false);
    if (!r.ok) r.err = mc_s(r.j.value("err", PsramString("操作失败")));
    return r;
}

// 乐观旗标应用/回滚 (单条; forward=true 应用动作语义, false 回滚;
// oneapp mail_mark 口径: mark_read→seen=true / flag→flagged=true …)
void mailc_apply_flag(const std::string& action, bool forward, AppSettings::MailMsg& m)
{
    if (action == "mark_read")        m.seen = forward;
    else if (action == "mark_unread") m.seen = !forward;
    else if (action == "flag")        m.flagged = forward;
    else if (action == "unflag")      m.flagged = !forward;
}

// ── UI 小件 (cb/ud 由成员渲染函数传入 — 匿名空间拿不到私有静态 cb) ───────────
lv_obj_t* mail_card(lv_obj_t* parent, lv_coord_t w, int h, uint32_t bg,
                    lv_event_cb_t cb, void* ud)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    return c;
}

lv_obj_t* mail_label(lv_obj_t* parent, const char* txt, uint32_t color,
                     const lv_font_t* font, int x, int y, int w,
                     lv_label_long_mode_t mode)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, mode);
    return l;
}

lv_obj_t* mail_pill(lv_obj_t* parent, int x, int w, const char* txt, uint32_t bg,
                    uint32_t fg, lv_event_cb_t cb, void* ud)
{
    lv_obj_t* b = mail_card(parent, w, 58, bg, cb, ud);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, zh_font_30(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t* mail_ta(lv_obj_t* parent, int x, int y, int w, int h, bool one_line,
                  bool pwd, lv_event_cb_t cb, void* ud)
{
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, h);
    lv_textarea_set_one_line(ta, one_line);
    if (pwd) lv_textarea_set_password_mode(ta, true);
    lv_obj_set_style_text_font(ta, zh_font_30(), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_pad_all(ta, 0, 0);
    lv_obj_add_event_cb(ta, cb, LV_EVENT_ALL, ud);
    return ta;
}

// 回复主题去叠加前缀 (oneapp mail_compose_open 同款语义: Re:/FW:/回复:/转发:…)
std::string mail_strip_reply_prefix(std::string s)
{
    for (;;) {
        size_t p = s.find_first_not_of(" \t");
        s = (p == std::string::npos) ? "" : s.substr(p);
        if (s.empty()) break;
        std::string low;
        for (char c : s.substr(0, 12)) low += (char)std::tolower((unsigned char)c);
        if (low.rfind("re:", 0) == 0)        s = s.substr(3);
        else if (low.rfind("fw:", 0) == 0)   s = s.substr(3);
        else if (low.rfind("aw:", 0) == 0)   s = s.substr(3);
        else if (low.rfind("sv:", 0) == 0)   s = s.substr(3);
        else if (s.rfind("回复：", 0) == 0)  s = s.substr(9);
        else if (s.rfind("回复:", 0) == 0)   s = s.substr(7);
        else if (s.rfind("转发：", 0) == 0)  s = s.substr(9);
        else if (s.rfind("转发:", 0) == 0)   s = s.substr(7);
        else break;
    }
    return s;
}

}  // namespace

// 乐观旗标应用到 cur_msg + msgs (worker 失败回滚共用)。action 支持逗号分隔
// 多动作 ("mark_read,flag" = flagread 两连发), 逐一应用/回滚。
void AppSettings::mailcApplyFlags(const std::string& actions_csv, const std::string& uid,
                                  const std::string& fld, bool forward)
{
    std::lock_guard<std::mutex> lk(mailc_mutex);
    std::string act = actions_csv;
    size_t pos;
    while ((pos = act.find(',')) != std::string::npos) {
        std::string one = act.substr(0, pos);
        act = act.substr(pos + 1);
        if (!one.empty()) mailc_apply_one(one, uid, fld, forward);
    }
    if (!act.empty()) mailc_apply_one(act, uid, fld, forward);
}

void AppSettings::mailc_apply_one(const std::string& action, const std::string& uid,
                                  const std::string& fld, bool forward)
{
    if (mailc_cur_msg.uid == uid && mailc_cur_msg.fld == fld)
        mailc_apply_flag(action, forward, mailc_cur_msg);
    for (auto& m : mailc_msgs)
        if (m.uid == uid && m.fld == fld) mailc_apply_flag(action, forward, m);
}

// 旗标动作统一入口 (详情钮 + 列表滑动钮): 乐观应用 → 发 worker; spawn 失败
// 立即回滚。重渲染走 _mail_pending_render 下一帧 (不同步删事件目标祖先)。
void AppSettings::mail_do_action(const std::string& action,
                                 const std::string& uid, const std::string& fld,
                                 const char* tip)
{
    if (uid.empty()) return;
    mailcApplyFlags(action, uid, fld, true);
    if (!mailcAction(_mail_acct, action, fld, uid, tip)) {
        mailcApplyFlags(action, uid, fld, false);
        _mailStatus("操作失败 (线程忙/创建失败)");
        return;
    }
    _mailStatus("操作中…");
    _mail_pending_render = true;
}

// ── 旧状态页数据源 (主页信封角标 + 潜在回退路径): 保留 fetchEmail 管线不动 ──
// 五层客户端本体走 /api/mail/*, 这套 /api/email/status 缓存只喂状态栏图标。
void AppSettings::fetchEmail()
{
    // spawn 失败退避闸 (!spawned 分支设置): 退避期内静默跳过, 既能自愈又不
    // 形成 16ms 一发的紧循环. (int32_t) 差值比较兼容 lv_tick 回绕.
    static uint32_t s_retry_not_before = 0;
    {
        uint32_t now_tick = lv_tick_get();
        if (s_retry_not_before != 0 && (int32_t)(now_tick - s_retry_not_before) < 0) return;
    }
    // 多轮询去重: 主页状态栏 120s 一轮, 45s 内完成过一次就复用上次结果。
    static uint32_t s_last_done_tick = 0;
    {
        uint32_t now_tick = lv_tick_get();
        if (s_last_done_tick != 0 && (int32_t)(now_tick - s_last_done_tick) < 45000) return;
    }
    if (email_fetching.exchange(true)) {
        mclog::tagWarn(_tag, "fetchEmail: already in flight, skip");
        return;
    }

    // email-status-server.py 跑在 8768 端口, 直接读 /tmp/email-status.json
    // 缓存, <10ms 返回, 不需要 IMAP 同步。用 svc_host 而不是 ha_host:
    // ha_host 指向 HA 主机, svc_host 才是 hermes + email-status-server 跑的机器.
    std::string host = GetHAL()->getConfig("svc_host", "192.168.1.200");   // 默认与主页一致 (模拟器无 NVS 配置)
    // 申请端按自身内存决定拿多少: Tab5 小内存 → 每夹最多 30 封 + 预览截 100 字
    std::string url  = "http://" + host + ":8768/api/email/status?limit=30&preview=100";

    // ETag 协商: 内容没变服务端回 304 空 body, 下载+解析整个跳过
    // (轮询 99% 走这条路, 内部堆 churn 归零)。仅 worker 线程读写,
    // 由 email_fetching 闸串行化。
    static std::string s_etag;
    std::vector<std::pair<std::string, std::string>> req_headers;
    if (!s_etag.empty()) req_headers.emplace_back("If-None-Match", s_etag);

    bool spawned = GetHAL()->tryRunDetached([url, req_headers]() {
        auto resp = GetHAL()->httpGet(url, req_headers);
        if (resp.status == 304) {
            // 304 早退也必须清闸, 否则 in-flight 旗标永久卡死 (2026-11-09 实锤).
            s_last_done_tick = lv_tick_get();
            email_fetching.store(false);
            return;
        }
        if (resp.ok) {
            for (auto& [k, v] : resp.resp_headers) {
                std::string lk = k;
                for (auto& c : lk) c = (char)std::tolower((unsigned char)c);
                if (lk == "etag" && !v.empty()) { s_etag = v; break; }
            }
            try {
                auto j = PsramJson::parse(resp.body);
                auto to_std = [](const PsramString& s) {
                    return std::string(s.data(), s.size());
                };
                std::lock_guard<std::mutex> lk(email_list_mutex);
                email_list.clear();
                email_folders.clear();
                if (j.contains("folders") && j["folders"].is_array()) {
                    int total = 0;
                    for (const auto& f : j["folders"]) {
                        if (!f.is_object()) continue;
                        std::string name = to_std(f.value("name", PsramString("?")));
                        EmailFolderInfo fi;
                        fi.name    = name;
                        fi.unread  = f.value("unread", 0);
                        fi.flagged = f.value("flagged", 0);
                        total += fi.unread;
                        email_folders.push_back(fi);
                        if (f.contains("messages") && f["messages"].is_array()) {
                            for (const auto& m : f["messages"]) {
                                if (!m.is_object()) continue;
                                EmailItem it;
                                it.uid     = to_std(m.value("uid", PsramString()));
                                it.from    = to_std(m.value("from", PsramString("?")));
                                it.subject = to_std(m.value("subject", PsramString("(无主题)")));
                                it.date    = to_std(m.value("date", PsramString()));
                                it.preview = to_std(m.value("preview", PsramString()));
                                it.folder  = name;
                                it.seen    = m.value("seen", false);
                                it.flagged = m.value("flagged", false);
                                email_list.push_back(std::move(it));
                            }
                        }
                    }
                    email_unread_total.store(total);
                    email_flagged_total.store(j.value("flagged_total", 0));
                    email_updated.store(true);
                    mclog::tagInfo(_tag, "email badge list updated ({} folders, {} mails, {} unread)",
                                   email_folders.size(), email_list.size(),
                                   email_unread_total.load());
                } else {
                    email_error.store(true);
                    email_error_msg = to_std(j.value("error", PsramString("未知错误")));
                }
            } catch (const std::exception& ex) {
                mclog::tagWarn(_tag, "email parse failed: {}", ex.what());
                email_error.store(true);
                email_error_msg = std::string("JSON 解析失败: ") + ex.what();
            }
        } else {
            mclog::tagWarn(_tag, "email fetch failed (status={})", resp.status);
            email_error.store(true);
            email_error_msg = resp.status == 0 ? "网络/timeout (status=0)"
                                               : ("HTTP " + std::to_string(resp.status));
        }
        s_last_done_tick = lv_tick_get();
        email_fetching.store(false);
    });
    if (!spawned) {
        mclog::tagWarn(_tag, "emailFetch: tryRunDetached failed (pthread_create?)");
        email_error.store(true);
        email_error_msg = "本地线程创建失败 (内存不足?)";
        email_fetching.store(false);
        s_retry_not_before = lv_tick_get() + 60 * 1000;
    }
}

void AppSettings::openEmailPage()
{
    // Public entry used by the home status-bar mail icon. Same as the user
    // tapping the "邮件" tool tile — build the sub-page and kick off a fetch.
    // 首次点击时 app 尚未打开, openApp() 异步切换后 onOpen 才建屏 (_scr 为空,
    // 直接 _openEmail 会空跑落在工具页) — 挂 pending, onOpen 建屏后补开.
    if (!_scr) {
        _email_open_pending = true;
        return;
    }
    _openEmail();
}

void AppSettings::_openEmail()
{
    mclog::tagInfo(_tag, "_openEmail: entered");
    if (_email_page) return;   // 防重入: 双开 = 叠屏泄漏 (onOpen 防重入铁律同源)
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _email_page = lv_obj_create(_scr);
    lv_obj_set_size(_email_page, 1280, 720);
    lv_obj_set_pos(_email_page, 0, 0);
    lv_obj_set_style_bg_color(_email_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_email_page, 0, 0);
    lv_obj_set_style_radius(_email_page, 0, 0);
    lv_obj_set_style_pad_all(_email_page, 0, 0);
    lv_obj_clear_flag(_email_page, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏: 返回钮 (逐层回退, 账号层=关页回工具页) + 标题 + 状态行 + 动作区
    // (动作区每层重建: List=刷新+写邮件 / Login=登录 / Compose=发送)
    mail_pill(_email_page, 20, 120, "返 回", C_CARD, C_TEXT, _mailBack_cb, this);
    _mail_title = mail_label(_email_page, "邮件", C_TEXT, zh_font_30(), 156, 26, 350,
                             LV_LABEL_LONG_DOT);
    _mail_status = mail_label(_email_page, "", C_DIM, zh_font_md(), 520, 34, 330,
                              LV_LABEL_LONG_DOT);
    _mail_actions = lv_obj_create(_email_page);
    lv_obj_set_size(_mail_actions, 400, 58);
    lv_obj_set_pos(_mail_actions, 860, 13);
    lv_obj_set_style_bg_opa(_mail_actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_mail_actions, 0, 0);
    lv_obj_set_style_pad_all(_mail_actions, 0, 0);
    lv_obj_clear_flag(_mail_actions, LV_OBJ_FLAG_SCROLLABLE);

    // 层内容容器 (每层 lv_obj_clean 重建; 不用 lv_list — 其 clip layer 会反复
    // 分配 1200x16 RGBA8888 缓冲把渲染管线堵死, 2026-11-07 实锤)
    _mail_content = lv_obj_create(_email_page);
    lv_obj_set_size(_mail_content, 1240, 608);
    lv_obj_set_pos(_mail_content, 20, 96);
    lv_obj_set_style_bg_color(_mail_content, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(_mail_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_mail_content, 0, 0);
    lv_obj_set_style_radius(_mail_content, 0, 0);
    lv_obj_set_style_pad_all(_mail_content, 0, 0);
    lv_obj_set_scroll_dir(_mail_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_mail_content, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(_mail_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(_mail_content, 12, 0);

    // 左列文件夹按钮 (两列布局, 仅 List 层显示 — _mailRender 按层切位置/可见性)
    _mail_sidebar = lv_obj_create(_email_page);
    lv_obj_set_size(_mail_sidebar, 300, 608);
    lv_obj_set_pos(_mail_sidebar, 20, 96);
    lv_obj_set_style_bg_color(_mail_sidebar, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(_mail_sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_mail_sidebar, 0, 0);
    lv_obj_set_style_radius(_mail_sidebar, 0, 0);
    lv_obj_set_style_pad_all(_mail_sidebar, 0, 0);
    lv_obj_set_scroll_dir(_mail_sidebar, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_mail_sidebar, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_flex_flow(_mail_sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(_mail_sidebar, 10, 0);
    lv_obj_add_flag(_mail_sidebar, LV_OBJ_FLAG_HIDDEN);

    // 内容区居中提示 (加载中/失败/空; 挂 page 防被 flex 顶走)
    _mail_hint = mail_label(_email_page, "加载中…", C_DIM, zh_font_30(), 0, 370, 1280,
                            LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_mail_hint, LV_TEXT_ALIGN_CENTER, 0);

    // 滑开行 5s 无操作自动合拢 (沿用旧机制)
    _mail_swipe_timer = lv_timer_create(_mailSwipeTimer_cb, 500, this);

    // 层状态复位 (静态缓存保留 — 下次打开秒显上次数据)
    _mail_lv = MailLv::Acct;
    _mail_acct.clear();
    _mail_folder.clear();
    _mail_folder_disp.clear();
    _mail_got = false;
    _mail_reply = false;
    _mail_sending = false;
    _mail_quote.clear();
    _mail_del_arm.clear();
    _mail_pending = _mail_pending_render = _mail_pending_close = false;

    _mailRender();
    mclog::tagInfo(_tag, "_openEmail: dispatching mailcFetchAccts");
    mailcFetchAccts();
}

void AppSettings::_closeEmail()
{
    _mailKbHide();
    if (_mail_swipe_timer) {
        lv_timer_del(_mail_swipe_timer);
        _mail_swipe_timer = nullptr;
    }
    _mail_swipe_open = nullptr;   // 行随 page 删除, 指针一并清
    if (_email_page) {
        lv_obj_delete(_email_page);
        _email_page = nullptr;
    }
    _mail_kb = nullptr;           // 键盘是 page 子对象, 已随删
    _mail_title = _mail_status = _mail_actions = nullptr;
    _mail_content = _mail_hint = _mail_sidebar = nullptr;
    _mail_ta_email = _mail_ta_auth = _mail_ta_to = _mail_ta_subj = _mail_ta_body = nullptr;
    _mail_body_lbl = nullptr;
    // 层状态复位 (静态缓存保留供下次秒开)
    _mail_lv = MailLv::Acct;
    _mail_acct.clear();
    _mail_folder.clear();
    _mail_folder_disp.clear();
    _mail_got = false;
    _mail_reply = false;
    _mail_sending = false;
    _mail_quote.clear();
    _mail_del_arm.clear();
    _mail_pending = _mail_pending_render = _mail_pending_close = false;
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

void AppSettings::_mailStatus(const std::string& s)
{
    if (_mail_status) lv_label_set_text(_mail_status, s.c_str());
}

void AppSettings::_mailKbShow(lv_obj_t* ta, bool voice)
{
    if (GetHAL()->usbKeyboardDetect()) return;   // 实体键盘在线不开软键盘 (app_ssh 同款)
    if (!_mail_kb) {
        _mail_kb = lv_keyboard_create(_email_page);
        lv_obj_set_size(_mail_kb, 1280, 260);
        lv_obj_align(_mail_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(_mail_kb, _mailKb_cb, LV_EVENT_ALL, this);
    }
    lv_keyboard_set_textarea(_mail_kb, ta);
    lv_obj_clear_flag(_mail_kb, LV_OBJ_FLAG_HIDDEN);
    if (voice) AppVoiceInput::requestVoiceInput(ta, 300);   // 中文输入 = 语音 (app_home 同款)
}

void AppSettings::_mailKbHide()
{
    if (_mail_kb) lv_obj_add_flag(_mail_kb, LV_OBJ_FLAG_HIDDEN);
    AppVoiceInput::dismissMicButton();
}

// 选定文件夹 (仅置状态, 不切层 — 事件回调里不同步删事件目标祖先;
// 切层由调用方走 _mail_pending 或 onRunning 里显式 _mailSetLv)
void AppSettings::_mailEnterList(const std::string& raw, const std::string& disp)
{
    _mail_folder = raw;
    _mail_folder_disp = disp;
    _mail_total = 0;
    _mail_got = false;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_msgs.clear();                           // 进新夹先清旧列表
        mailc_total = 0;
    }
}

void AppSettings::_mailSetLv(MailLv lv)
{
    _mailKbHide();
    _mail_lv = lv;
    _mailRender();
    switch (lv) {
    case MailLv::Acct:    mailcFetchAccts(); break;
    case MailLv::List:
        // 两列布局: 左列文件夹按钮 + 右列列表 — 夹表与列表并行拉
        mailcFetchFolders(_mail_acct);
        mailcFetchList(_mail_acct, _mail_folder);
        _mailStatus("获取中…");
        break;
    default: break;   // Login/Detail/Compose 不自动拉
    }
}

AppSettings::MailLv AppSettings::_mailBackTarget(bool* close)
{
    *close = false;
    switch (_mail_lv) {
    case MailLv::Compose:  return _mail_reply ? MailLv::Detail : MailLv::List;
    case MailLv::Detail:   return MailLv::List;
    case MailLv::List:     return MailLv::Acct;   // 两列版: 列表页直接回账号层
    case MailLv::Login:    return MailLv::Acct;
    case MailLv::Acct: default: *close = true; return MailLv::Acct;
    }
}

// ── 渲染 (每层: 顶栏标题/状态/动作 + 内容容器重建) ───────────────────────────

void AppSettings::_mailRender()
{
    if (!_email_page || !_mail_content) return;
    lv_obj_clean(_mail_actions);
    lv_obj_clean(_mail_content);
    _mail_swipe_open = nullptr;
    _mail_body_lbl = nullptr;
    _mail_ta_email = _mail_ta_auth = _mail_ta_to = _mail_ta_subj = _mail_ta_body = nullptr;
    lv_obj_add_flag(_mail_hint, LV_OBJ_FLAG_HIDDEN);
    // 两列布局 (List 层): 左列文件夹按钮 300px + 右列列表 920px;
    // 其余层独占全宽, 左列隐藏。
    const bool two_col = (_mail_lv == MailLv::List);
    if (two_col) {
        lv_obj_set_pos(_mail_content, 340, 96);
        lv_obj_set_size(_mail_content, 920, 608);
        if (_mail_sidebar) lv_obj_clear_flag(_mail_sidebar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(_mail_hint, LV_ALIGN_TOP_MID, 160, 370);  // 提示居右列中心
    } else {
        lv_obj_set_pos(_mail_content, 20, 96);
        lv_obj_set_size(_mail_content, 1240, 608);
        if (_mail_sidebar) lv_obj_add_flag(_mail_sidebar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(_mail_hint, LV_ALIGN_TOP_MID, 0, 370);
    }
    switch (_mail_lv) {
    case MailLv::Acct:    _mailRenderAcct();    break;
    case MailLv::Login:   _mailRenderLogin();   break;
    case MailLv::List:
        if (_mail_sidebar) { lv_obj_clean(_mail_sidebar); _mailRenderSidebar(); }
        _mailRenderList();
        break;
    case MailLv::Detail:  _mailRenderDetail();  break;
    case MailLv::Compose: _mailRenderCompose(); break;
    }
}

void AppSettings::_mailRenderAcct()
{
    lv_label_set_text(_mail_title, "邮件 · 账号");
    std::vector<MailAcct> accts;
    std::string err;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        accts = mailc_accts;
        err = mailc_err;
        ok = mailc_accts_ok.load();
    }
    _mailStatus(std::to_string(accts.size()) + " 个账号");

    for (const auto& a : accts) {
        auto* ctx = new MailCtx{this, a.email, "", ""};
        lv_obj_t* row = mail_card(_mail_content, LV_PCT(100), 110, C_CARD,
                                  _mailAcctRow_cb, ctx);
        lv_obj_add_event_cb(row, mail_ctx_del_cb, LV_EVENT_DELETE, ctx);
        mail_label(row, a.email.c_str(), C_TEXT, zh_font_30(), 24, 40, 820,
                   LV_LABEL_LONG_DOT);
        if (a.unread > 0) {
            std::string un = "未读 " + std::to_string(a.unread);
            mail_label(row, un.c_str(), C_ACCENT, zh_font_md(), 850, 44, 160,
                       LV_LABEL_LONG_DOT);
        }
        // 删除 chip (二段确认: 点一下变红「确 认」, 5s 内再点才删)
        auto* dctx = new MailCtx{this, a.email, "", ""};
        lv_obj_t* del = mail_card(row, 120, 70, C_CARD, _mailAcctDel_cb, dctx);
        lv_obj_add_event_cb(del, mail_ctx_del_cb, LV_EVENT_DELETE, dctx);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_set_style_radius(del, 14, 0);
        lv_obj_t* dl = lv_label_create(del);
        lv_label_set_text(dl, "删 除");
        lv_obj_set_style_text_font(dl, zh_font_md(), 0);
        lv_obj_set_style_text_color(dl, lv_color_hex(C_DANGER), 0);
        lv_obj_center(dl);
    }

    // ＋ 添加账号
    lv_obj_t* add = mail_card(_mail_content, LV_PCT(100), 110, C_CARD,
                              _mailAddAcct_cb, this);
    lv_obj_t* al = lv_label_create(add);
    lv_label_set_text(al, "＋ 添加账号");
    lv_obj_set_style_text_font(al, zh_font_30(), 0);
    lv_obj_set_style_text_color(al, lv_color_hex(C_ACCENT), 0);
    lv_obj_center(al);

    if (accts.empty()) {
        if (!ok && !err.empty())
            lv_label_set_text(_mail_hint, ("拉取失败: " + err).c_str());
        else
            lv_label_set_text(_mail_hint, "还没有账号 · 点「＋ 添加账号」登录");
        lv_obj_clear_flag(_mail_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::_mailRenderLogin()
{
    lv_label_set_text(_mail_title, "邮件 · 添加账号");
    _mailStatus("已登录账号更新授权码走同一入口");
    mail_pill(_mail_actions, 202, 190, "登 录", C_ACCENT, C_BG, _mailLoginGo_cb, this);

    lv_obj_t* c1 = mail_card(_mail_content, LV_PCT(100), 96, C_CARD, nullptr, nullptr);
    mail_label(c1, "邮 箱", C_DIM, zh_font_md(), 24, 36, 100, LV_LABEL_LONG_DOT);
    _mail_ta_email = mail_ta(c1, 150, 18, 1050, 60, true, false, _mailTa_cb, this);

    lv_obj_t* c2 = mail_card(_mail_content, LV_PCT(100), 96, C_CARD, nullptr, nullptr);
    mail_label(c2, "授权码", C_DIM, zh_font_md(), 24, 36, 100, LV_LABEL_LONG_DOT);
    _mail_ta_auth = mail_ta(c2, 150, 18, 1050, 60, true, true, _mailTa_cb, this);

    // 授权码获取提示 (oneapp draw_mail_login 同款口径)
    mail_label(_mail_content,
        "邮箱网页版 设置 → POP3/SMTP/IMAP 开启服务并生成授权码\n"
        "(163: 设置→POP3/SMTP/IMAP · QQ: 设置→账号→POP3/IMAP/SMTP 服务)",
        C_DIM, zh_font_md(), 24, 12, 1192, LV_LABEL_LONG_WRAP);
}

// 左列文件夹按钮 (两列布局; 虚拟聚合夹置顶 + 真实夹; 当前选中高亮)
void AppSettings::_mailRenderSidebar()
{
    std::vector<MailFolder> folders;
    std::string err;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        folders = mailc_folders;
        err = mailc_err;
        ok = mailc_folders_ok.load();
    }
    for (const auto& f : folders) {
        const bool sel = (f.raw == _mail_folder);
        auto* ctx = new MailCtx{this, f.raw, f.name, ""};
        lv_obj_t* btn = mail_card(_mail_sidebar, LV_PCT(100), f.virt ? 92 : 76,
                                  sel ? C_ACCENT : (f.virt ? C_CARD_PR : C_CARD),
                                  _mailFolderBtn_cb, ctx);
        lv_obj_add_event_cb(btn, mail_ctx_del_cb, LV_EVENT_DELETE, ctx);
        const uint32_t fg = sel ? C_BG : (f.virt ? C_ACCENT : C_TEXT);
        lv_obj_t* nl = mail_label(btn, f.name.c_str(), fg,
                                  f.virt ? zh_font_30() : zh_font_md(),
                                  20, (f.virt ? 92 : 76) / 2 - (f.virt ? 21 : 17),
                                  200, LV_LABEL_LONG_DOT);
        (void)nl;
        if (f.unread > 0 && !sel) {
            std::string un = std::to_string(f.unread);
            lv_obj_t* ub = mail_label(btn, un.c_str(), C_ACCENT, zh_font_md(),
                                      248, (f.virt ? 92 : 76) / 2 - 15, 44,
                                      LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(ub, LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    if (folders.empty()) {
        lv_obj_t* l = lv_label_create(_mail_sidebar);
        lv_label_set_text(l, ok ? "无文件夹" : ("拉取失败: " + err).c_str());
        lv_obj_set_style_text_font(l, zh_font_md(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
}

void AppSettings::_mailRenderList()
{
    lv_label_set_text(_mail_title, _mail_folder_disp.c_str());
    mail_pill(_mail_actions, 0, 190, "刷 新", C_CARD, C_TEXT, _mailRefreshBtn_cb, this);
    mail_pill(_mail_actions, 202, 190, "写邮件", C_ACCENT, C_BG, _mailComposeBtn_cb, this);

    // 本地过滤兜乐观更新 (2026-11-15 两列版口径):
    // @unread → 只留未读 (标已读即消失); @flagged → 只留旗标 (取旗即消失);
    // 真实夹 = 文件夹浏览, 全量显示 (已读变暗, 不消失)。
    const bool scope_unread  = (_mail_folder == "@unread");
    const bool scope_flagged = (_mail_folder == "@flagged");
    std::vector<MailMsg> view;
    int total = 0;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        for (const auto& m : mailc_msgs) {
            if (scope_unread && m.seen) continue;
            if (scope_flagged && !m.flagged) continue;
            view.push_back(m);
        }
        total = mailc_total;
    }
    _mailStatus(std::to_string(view.size()) + "/" + std::to_string(total) + " 封");

    const bool agg = scope_unread || scope_flagged;   // 聚合视图标注来源夹
    for (const auto& m : view) _mailMakeRow(m, agg);

    if (view.empty()) {
        std::string err;
        bool ok;
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            err = mailc_err;
            ok = mailc_list_ok.load();
        }
        if (_mail_got && !ok && !err.empty())
            lv_label_set_text(_mail_hint, ("拉取失败: " + err).c_str());
        else if (_mail_got)
            lv_label_set_text(_mail_hint, scope_unread ? "没有未读邮件"
                              : scope_flagged ? "没有红旗邮件" : "文件夹是空的");
        else
            lv_label_set_text(_mail_hint, "获取中…");
        lv_obj_clear_flag(_mail_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppSettings::_mailRenderDetail()
{
    MailMsg cur;
    std::string body_txt;
    bool body_ok = false;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        cur = mailc_cur_msg;
        body_txt = mailc_body;
        body_ok = mailc_body_ok.load();
    }
    lv_label_set_text(_mail_title, cur.from.c_str());
    _mailStatus(cur.date);

    // 头部: 发件人 + 日期 / 主题 (≤2 行) / 分隔线
    lv_obj_t* head = mail_card(_mail_content, LV_PCT(100), 152, C_BG, nullptr, nullptr);
    lv_obj_set_style_radius(head, 0, 0);
    mail_label(head, cur.from.c_str(), C_TEXT, zh_font_30(), 24, 10, 850,
               LV_LABEL_LONG_DOT);
    lv_obj_t* d = mail_label(head, cur.date.c_str(), C_DIM, zh_font_md(), 1000, 16,
                             216, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t* subj = mail_label(head, cur.subject.c_str(), C_TEXT, zh_font_30(),
                                24, 56, 1192, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(subj, 88);   // 最多两行, 超出裁剪 (防吃掉正文区)
    lv_obj_t* div = mail_card(head, 1192, 2, C_CARD, nullptr, nullptr);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_set_pos(div, 24, 148);

    // 正文滚动区 (服务端截 6000 字; body done 由 onRunning 填 _mail_body_lbl)
    lv_obj_t* body = mail_card(_mail_content, LV_PCT(100), 350, C_BG, nullptr, nullptr);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_pad_all(body, 12, 0);
    _mail_body_lbl = mail_label(body,
                                body_ok ? body_txt.c_str() : "加载中…",
                                C_SOFT, zh_font_md(), 0, 0, 1180, LV_LABEL_LONG_WRAP);
    if (!body_ok && !body_txt.empty())
        lv_label_set_text(_mail_body_lbl, ("获取失败: " + body_txt).c_str());

    // 动作条: 标已读/未读 | 红旗/取旗 | 回复 (乐观更新, oneapp mail_mark 口径)
    lv_obj_t* bar = mail_card(_mail_content, LV_PCT(100), 84, C_BG, nullptr, nullptr);
    lv_obj_set_style_radius(bar, 0, 0);
    mail_pill(bar, 0, 380, cur.seen ? "标未读" : "标已读", C_CARD, C_TEXT,
              _mailMarkBtn_cb, this);
    mail_pill(bar, 430, 380, cur.flagged ? "取 旗" : "红 旗", C_CARD,
              cur.flagged ? C_DIM : C_WARN, _mailFlagBtn_cb, this);
    mail_pill(bar, 860, 380, "回 复", C_ACCENT, C_BG, _mailReplyBtn_cb, this);
}

void AppSettings::_mailRenderCompose()
{
    lv_label_set_text(_mail_title, _mail_reply ? "回复" : "写邮件");
    _mailStatus(_mail_acct);
    mail_pill(_mail_actions, 202, 190, "发 送", C_ACCENT, C_BG, _mailSendBtn_cb, this);

    lv_obj_t* c1 = mail_card(_mail_content, LV_PCT(100), 96, C_CARD, nullptr, nullptr);
    mail_label(c1, "收件人", C_DIM, zh_font_md(), 24, 36, 110, LV_LABEL_LONG_DOT);
    _mail_ta_to = mail_ta(c1, 160, 18, 1040, 60, true, false, _mailTa_cb, this);

    lv_obj_t* c2 = mail_card(_mail_content, LV_PCT(100), 96, C_CARD, nullptr, nullptr);
    mail_label(c2, "主 题", C_DIM, zh_font_md(), 24, 36, 110, LV_LABEL_LONG_DOT);
    _mail_ta_subj = mail_ta(c2, 160, 18, 1040, 60, true, false, _mailTa_cb, this);

    lv_obj_t* c3 = mail_card(_mail_content, LV_PCT(100), 280, C_CARD, nullptr, nullptr);
    _mail_ta_body = mail_ta(c3, 24, 14, 1192, 240, false, false, _mailTa_cb, this);

    // 回复引用块 (暗色只读, 发送时拼在用户文字后 — oneapp 同款)
    if (_mail_reply && !_mail_quote.empty()) {
        lv_obj_t* qc = mail_card(_mail_content, LV_PCT(100), 200, C_CARD, nullptr, nullptr);
        lv_obj_add_flag(qc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(qc, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(qc, LV_SCROLLBAR_MODE_ACTIVE);
        lv_obj_set_style_pad_all(qc, 12, 0);
        lv_obj_t* ql = mail_label(qc, _mail_quote.c_str(), C_DIM, zh_font_md(),
                                  0, 0, 1160, LV_LABEL_LONG_WRAP);
        (void)ql;
    }

    // 回复预填: 收件人 = 回信地址, 主题 = Re: 去叠加前缀 (oneapp 同款)
    if (_mail_reply) {
        std::string to, subj;
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            to = mailc_meta.from_addr;
            subj = mailc_meta.subject;
        }
        lv_textarea_set_text(_mail_ta_to, to.c_str());
        lv_textarea_set_text(_mail_ta_subj, ("Re: " + mail_strip_reply_prefix(subj)).c_str());
    }
}

// ── 列表滑动行 (旧版 _emailMakeRow 移植: 行 = 横向滚动视口, 点面卡进详情) ─────

void AppSettings::_mailCollapseSwipe(bool anim)
{
    lv_obj_t* row = _mail_swipe_open;
    _mail_swipe_open = nullptr;
    if (row) lv_obj_scroll_to_x(row, 160, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

lv_obj_t* AppSettings::_mailMakeRow(const MailMsg& m, bool agg)
{
    // 行结构: 1240 宽横向滚动视口, 内容三段 [左操作区 | 面卡 | 右操作区],
    // 初始 scroll_x=200 只露面卡。左滑(→400)露右区 / 右滑(→0)露左区;
    // 红旗视图主手势=右滑取消红旗, 其余视图主手势=左滑已读+红旗。
    lv_obj_t* row = lv_obj_create(_mail_content);
    lv_obj_set_size(row, LV_PCT(100), 184);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);  // 拖到头不链给纵向列表

    const bool flagged_view = (_mail_folder == "@flagged");
    const bool act_avail = !m.uid.empty();
    // 已读钮: 未读→「已读」(强调) / 已读→「标未读」(弱化)
    const char* read_txt = m.seen ? "标未读" : "已 读";
    const char* read_act = m.seen ? "mark_unread" : "mark_read";
    const uint32_t read_fg = m.seen ? C_DIM : C_ACCENT;

    const char* l_txt = flagged_view ? "取消红旗" : read_txt;
    const char* l_act = flagged_view ? "unflag" : read_act;
    uint32_t l_fg = flagged_view ? C_WARN : read_fg;
    const char* r_txt = flagged_view ? read_txt : "红 旗";
    const char* r_act = flagged_view ? read_act : "flagread";
    uint32_t r_fg = flagged_view ? read_fg : C_WARN;

    auto mk_zone = [&](int zx, const char* txt, const char* act, uint32_t fg) {
        lv_obj_t* b = lv_obj_create(row);
        lv_obj_set_size(b, 152, 152);
        lv_obj_set_pos(b, zx, 16);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh_font_md(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_obj_center(l);
        lv_obj_add_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);  // PRESSED 冒泡给行做单开合拢
        if (!act_avail) return;  // 兜底条目 (无 uid) 不可操作
        auto* ctx = new MailCtx{this, act, m.fld, m.uid};
        lv_obj_add_event_cb(b, _mailAct_cb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(b, mail_ctx_del_cb, LV_EVENT_DELETE, ctx);
    };
    mk_zone(4, l_txt, l_act, l_fg);        // 左区 (右滑露)
    mk_zone(1084, r_txt, r_act, r_fg);     // 右区 (左滑露); 内容右缘 1236 = 满程 316

    // 面卡 (可点击 → 详情; 拖它滚行) — 两列布局: 右列宽 920
    lv_obj_t* face = lv_obj_create(row);
    lv_obj_set_size(face, 920, 184);
    lv_obj_set_pos(face, 160, 0);
    lv_obj_set_style_bg_color(face, lv_color_hex(C_CARD_PR), 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(face, 18, 0);
    lv_obj_set_style_border_width(face, 0, 0);
    lv_obj_set_style_shadow_width(face, 0, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (act_avail) {
        auto* octx = new MailCtx{this, m.uid, m.fld, ""};
        lv_obj_add_event_cb(face, _mailMsgOpen_cb, LV_EVENT_CLICKED, octx);
        lv_obj_add_event_cb(face, mail_ctx_del_cb, LV_EVENT_DELETE, octx);
    }

    auto mk_label = [&](const char* txt, uint32_t color, const lv_font_t* font,
                        int x, int y, int w, lv_label_long_mode_t mode) {
        lv_obj_t* l = lv_label_create(face);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
        lv_obj_set_pos(l, x, y);
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, mode);
        return l;
    };
    mk_label(m.from.c_str(), C_TEXT, zh_font_30(), 24, 12, 600, LV_LABEL_LONG_DOT);
    if (!m.date.empty()) {
        lv_obj_t* d = mk_label(m.date.c_str(), C_DIM, zh_font_md(),
                               724, 20, 172, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_RIGHT, 0);
    }
    if (m.flagged) {
        // 🚩 无字形 (GB2312/FA 都没有) → 主题 danger 色圆角点
        lv_obj_t* fl = lv_obj_create(face);
        lv_obj_set_size(fl, 18, 18);
        lv_obj_set_pos(fl, 648, 24);
        lv_obj_set_style_bg_color(fl, lv_color_hex(C_DANGER), 0);
        lv_obj_set_style_bg_opa(fl, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(fl, 4, 0);
        lv_obj_set_style_border_width(fl, 0, 0);
        lv_obj_set_style_shadow_width(fl, 0, 0);
        lv_obj_set_style_pad_all(fl, 0, 0);
        lv_obj_clear_flag(fl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(fl, LV_OBJ_FLAG_CLICKABLE);
    }
    // 主题: 未读亮色 / 已读弱化
    mk_label(m.subject.c_str(), m.seen ? C_DIM : C_TEXT, zh_font_30(),
             24, 58, 872, LV_LABEL_LONG_DOT);
    if (!m.preview.empty()) {
        std::string pv = m.preview;
        if (agg && !m.fldn.empty() && m.fld != _mail_folder)
            pv = "[" + m.fldn + "] " + pv;   // 聚合视图标注来源夹 (oneapp 同款)
        lv_obj_t* p = mk_label(pv.c_str(), C_DIM, zh_font_md(), 24, 104, 872,
                               LV_LABEL_LONG_WRAP);
        lv_obj_set_height(p, 64);
        lv_obj_set_style_text_line_space(p, 2, 0);
    }

    // 单开原则: 按下新行时合拢已开的旧行
    lv_obj_add_event_cb(row, _mailRowPressed_cb, LV_EVENT_PRESSED, this);
    // 松手吸合 {0, 160, 316} + 单开登记
    lv_obj_add_event_cb(row, _mailRowScroll_cb, LV_EVENT_SCROLL_END, this);

    // 初始 scroll_x=160 恰好只露面卡 (先强制布局, 否则 range 未算好被钳到 0)
    lv_obj_update_layout(row);
    lv_obj_scroll_to_x(row, 160, LV_ANIM_OFF);
    return row;
}

// ── worker (全部 tryRunDetached; 失败必须复位闸 + 给 UI 出口 — 2026-11-09 铁律)

void AppSettings::mailcFetchAccts()
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;
    bool spawned = GetHAL()->tryRunDetached([]() {
        std::string host = GetHAL()->getConfig("svc_host", "192.168.1.200");
        auto resp = GetHAL()->httpGet("http://" + host + ":8768/api/mail/accounts");
        bool ok = false;
        std::string err;
        std::vector<MailAcct> out;
        if (resp.ok) {
            try {
                auto j = PsramJson::parse(resp.body);
                ok = j.value("ok", false);
                if (!ok) {
                    err = mc_s(j.value("err", PsramString("获取失败")));
                } else if (j.contains("accounts") && j["accounts"].is_array()) {
                    for (const auto& a : j["accounts"]) {
                        if (!a.is_object()) continue;
                        MailAcct m;
                        m.email  = mc_s(a.value("email", PsramString()));
                        m.unread = (int)a.value("unread", 0);
                        out.push_back(std::move(m));
                    }
                }
            } catch (const std::exception& ex) {
                ok = false;
                err = std::string("JSON 解析失败: ") + ex.what();
            }
        } else {
            err = resp.status == 0 ? "网络/timeout (检查 8768)"
                                   : ("HTTP " + std::to_string(resp.status));
        }
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_accts = std::move(out);
            mailc_err = err;
        }
        mailc_accts_ok.store(ok);
        mailc_accts_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_err = "线程创建失败 (内存不足?)";
        mailc_accts_ok.store(false);
        mailc_accts_done.store(true);
    }
}

void AppSettings::mailcLogin(const std::string& email, const std::string& auth)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;
    bool spawned = GetHAL()->tryRunDetached([email, auth]() {
        nlohmann::json body = {{"email", email}, {"auth", auth}};
        auto r = mc_post("/api/mail/accounts", body.dump());
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_login_ok.store(r.ok);
            mailc_login_email = email;
            mailc_login_unread = r.ok ? (int)r.j.value("unread", 0) : 0;
            mailc_err = r.ok ? "" : r.err;
        }
        mailc_login_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_login_ok.store(false);
        mailc_err = "线程创建失败 (内存不足?)";
        mailc_login_done.store(true);
    }
}

void AppSettings::mailcDelAcct(const std::string& email)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;
    bool spawned = GetHAL()->tryRunDetached([email]() {
        nlohmann::json body = {{"email", email}};
        auto r = mc_post("/api/mail/accounts/delete", body.dump());
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_del_ok.store(r.ok);
            mailc_err = r.ok ? "" : r.err;
        }
        mailc_del_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_del_ok.store(false);
        mailc_err = "线程创建失败 (内存不足?)";
        mailc_del_done.store(true);
    }
}

void AppSettings::mailcFetchFolders(const std::string& email)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;
    bool spawned = GetHAL()->tryRunDetached([email]() {
        nlohmann::json body = {{"email", email}};
        auto r = mc_post("/api/mail/folders", body.dump());
        bool ok = r.ok;
        std::vector<MailFolder> out;
        if (ok && r.j.contains("folders") && r.j["folders"].is_array()) {
            for (const auto& f : r.j["folders"]) {
                if (!f.is_object()) continue;
                MailFolder mf;
                mf.raw    = mc_s(f.value("raw", PsramString()));
                mf.name   = mc_s(f.value("name", PsramString("?")));
                mf.unread = (int)f.value("unread", -1);
                mf.total  = (int)f.value("total", -1);
                mf.virt   = mf.raw.rfind("@", 0) == 0;   // 服务端预置聚合虚拟夹
                out.push_back(std::move(mf));
            }
        }
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_folders = std::move(out);
            mailc_folders_email = email;   // 回执对号 (快速换账号弃旧结果)
            mailc_err = ok ? "" : r.err;
        }
        mailc_folders_ok.store(ok);
        mailc_folders_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_err = "线程创建失败 (内存不足?)";
        mailc_folders_ok.store(false);
        mailc_folders_done.store(true);
    }
}

void AppSettings::mailcFetchList(const std::string& email, const std::string& folder)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;
    bool spawned = GetHAL()->tryRunDetached([email, folder]() {
        nlohmann::json body = {{"email", email}, {"folder", folder}, {"limit", 30}};
        auto r = mc_post("/api/mail/list", body.dump());
        bool ok = r.ok;
        std::vector<MailMsg> out;
        int total = 0;
        if (ok && r.j.contains("msgs") && r.j["msgs"].is_array()) {
            total = (int)r.j.value("total", 0);
            for (const auto& m : r.j["msgs"]) {
                if (!m.is_object()) continue;
                MailMsg mm;
                mm.uid     = mc_s(m.value("uid", PsramString()));
                mm.fld     = mc_s(m.value("fld", PsramString(folder)));
                mm.fldn    = mc_s(m.value("fldn", PsramString()));
                mm.from    = mc_s(m.value("from", PsramString("?")));
                mm.subject = mc_s(m.value("subject", PsramString("(无主题)")));
                mm.date    = mc_s(m.value("date", PsramString()));
                mm.preview = mc_s(m.value("preview", PsramString()));
                mm.seen    = m.value("seen", false);
                mm.flagged = m.value("flagged", false);
                out.push_back(std::move(mm));
            }
        }
        size_t n = out.size();
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_msgs = std::move(out);
            mailc_total = total;
            mailc_list_folder = folder;   // 回执对号 (快速换夹弃旧结果)
            mailc_err = ok ? "" : r.err;
        }
        mailc_list_ok.store(ok);
        mailc_list_done.store(true);
        s_busy.store(false);
        mclog::tagInfo(_tag, "mailc list: folder={} ok={} msgs={} total={}",
                       folder, ok, (int)n, total);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_err = "线程创建失败 (内存不足?)";
        mailc_list_ok.store(false);
        mailc_list_done.store(true);
    }
}

void AppSettings::mailcFetchBody(const std::string& email, const std::string& folder,
                                 const std::string& uid)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) {
        // 上一封正文还在取: 本次的请求让位 — 回执按本次 uid/fld 出, UI 显示
        // 失败文案 (用户返回重进即重取; 1-3s 的取正文窗口里连开两信才撞上)
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_body_uid = uid;
        mailc_body_fld = folder;
        mailc_body_ok.store(false);
        mailc_body = "上一封正文还在获取, 稍后重进重试";
        mailc_body_done.store(true);
        return;
    }
    bool spawned = GetHAL()->tryRunDetached([email, folder, uid]() {
        nlohmann::json body = {{"email", email}, {"folder", folder}, {"uid", uid}};
        auto r = mc_post("/api/mail/body", body.dump());
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_body_uid = uid;          // 回执对号 (快速换信弃旧结果)
            mailc_body_fld = folder;
            mailc_body_ok.store(r.ok);
            if (r.ok) {
                mailc_body = mc_s(r.j.value("text", PsramString("(空邮件)")));
                mailc_meta.msg_id    = mc_s(r.j.value("msg_id", PsramString()));
                mailc_meta.refs      = mc_s(r.j.value("refs", PsramString()));
                mailc_meta.from_addr = mc_s(r.j.value("from_addr", PsramString()));
                mailc_meta.date      = mc_s(r.j.value("date", PsramString()));
                mailc_meta.subject   = mc_s(r.j.value("subject", PsramString()));
            } else {
                mailc_body = r.err;        // 失败文案走同一通道 (UI 判 ok 区分)
            }
        }
        mailc_body_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_body_uid = uid;
        mailc_body_fld = folder;
        mailc_body_ok.store(false);
        mailc_body = "线程创建失败 (内存不足?)";
        mailc_body_done.store(true);
    }
}

bool AppSettings::mailcAction(const std::string& email, const std::string& actions_csv,
                              const std::string& folder, const std::string& uid,
                              const char* tip)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return false;
    std::string tipstr(tip);
    bool spawned = GetHAL()->tryRunDetached([email, actions_csv, folder, uid, tipstr]() {
        // flagread = "mark_read,flag" 两连发 (服务端不组合, 顺序由 RLock 串行保证)
        bool ok = true;
        std::string err;
        std::string act = actions_csv;
        size_t pos;
        while ((pos = act.find(',')) != std::string::npos) {
            std::string one = act.substr(0, pos);
            act = act.substr(pos + 1);
            if (one.empty()) continue;
            nlohmann::json body = {{"email", email}, {"folder", folder},
                                   {"uid", uid}, {"action", one}};
            auto r = mc_post("/api/mail/action", body.dump());
            if (!r.ok) { ok = false; err = r.err; break; }
        }
        if (ok && !act.empty()) {
            nlohmann::json body = {{"email", email}, {"folder", folder},
                                   {"uid", uid}, {"action", act}};
            auto r = mc_post("/api/mail/action", body.dump());
            if (!r.ok) { ok = false; err = r.err; }
        }
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_act_ok.store(ok);
            mailc_act_err = ok ? "" : err;
            mailc_act_tip = tipstr;
            mailc_act_action = actions_csv;   // 回滚按 csv 逐一反向
            mailc_act_uid = uid;
            mailc_act_fld = folder;
        }
        mailc_act_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        return false;   // 调用方立即回滚
    }
    return true;
}

void AppSettings::mailcSend(const std::string& email, const std::string& to,
                            const std::string& subject, const std::string& body,
                            const std::string& in_reply_to,
                            const std::string& references)
{
    static std::atomic<bool> s_busy{false};
    if (s_busy.exchange(true)) return;
    bool spawned = GetHAL()->tryRunDetached(
        [email, to, subject, body, in_reply_to, references]() {
        nlohmann::json body_j = {{"email", email}, {"to", to},
                                 {"subject", subject}, {"body", body}};
        if (!in_reply_to.empty()) {           // 回复线程头 (oneapp send_mail 同款)
            body_j["in_reply_to"] = in_reply_to;
            body_j["references"] = references;
        }
        auto r = mc_post("/api/mail/send", body_j.dump());
        {
            std::lock_guard<std::mutex> lk(mailc_mutex);
            mailc_send_ok.store(r.ok);
            mailc_send_err = r.ok ? "" : r.err;
        }
        mailc_send_done.store(true);
        s_busy.store(false);
    });
    if (!spawned) {
        s_busy.store(false);
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_send_ok.store(false);
        mailc_send_err = "线程创建失败 (内存不足?)";
        mailc_send_done.store(true);
    }
}

// ── 回调 ──────────────────────────────────────────────────────────────────────

void AppSettings::_mailBack_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self || !self->_email_page) return;
    bool close = false;
    MailLv nxt = self->_mailBackTarget(&close);
    if (close) self->_mail_pending_close = true;      // 账号层 → 关页回工具页
    else { self->_mail_pending_lv = nxt; self->_mail_pending = true; }
}

void AppSettings::_mailAcctRow_cb(lv_event_t* e)
{
    auto* ctx = static_cast<MailCtx*>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    ctx->self->_mail_acct = ctx->a;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_folders.clear();                        // 进新账号先清旧夹列表
    }
    // 两列版: 选账号直达列表页 (默认「全部未读」, 左列可切文件夹)
    ctx->self->_mailEnterList("@unread", "全部未读");
    ctx->self->_mail_pending_lv = MailLv::List;
    ctx->self->_mail_pending = true;
}

void AppSettings::_mailAcctDel_cb(lv_event_t* e)
{
    auto* ctx = static_cast<MailCtx*>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    AppSettings* self = ctx->self;
    lv_obj_t* chip = lv_event_get_current_target_obj(e);
    if (self->_mail_del_arm != ctx->a ||
        lv_tick_elaps(self->_mail_del_arm_ts) > 5000) {
        self->_mail_del_arm = ctx->a;
        self->_mail_del_arm_ts = lv_tick_get();
        lv_obj_set_style_bg_color(chip, lv_color_hex(C_DANGER), 0);
        lv_obj_t* l = lv_obj_get_child(chip, 0);
        if (l) {
            lv_label_set_text(l, "确 认");
            lv_obj_set_style_text_color(l, lv_color_hex(C_BG), 0);
        }
        self->_mailStatus("再点一次确认删除 " + ctx->a);
        return;
    }
    self->_mail_del_arm.clear();
    self->_mailStatus("删除中…");
    mailcDelAcct(ctx->a);
}

void AppSettings::_mailAddAcct_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self) return;
    self->_mail_pending_lv = MailLv::Login;
    self->_mail_pending = true;
}

void AppSettings::_mailLoginGo_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self || !self->_mail_ta_email || !self->_mail_ta_auth) return;
    std::string em = lv_textarea_get_text(self->_mail_ta_email);
    std::string au = lv_textarea_get_text(self->_mail_ta_auth);
    // trim (oneapp 同款)
    auto trim = [](std::string s) {
        size_t p = s.find_first_not_of(" \t\r\n");
        if (p == std::string::npos) return std::string();
        size_t q = s.find_last_not_of(" \t\r\n");
        return s.substr(p, q - p + 1);
    };
    em = trim(em);
    au = trim(au);
    if (em.empty() || au.empty()) {
        self->_mailStatus("邮箱/授权码未填");
        return;
    }
    size_t at = em.find('@');
    if (at == std::string::npos || em.find('.', at) == std::string::npos) {
        self->_mailStatus("邮箱格式不对 (如 xx@163.com)");
        return;
    }
    self->_mailStatus("连接中… (IMAP 验证)");
    mailcLogin(em, au);
}

// 左列文件夹按钮: 原地切换右列列表 (不换层; 乐观清空 + 重渲染下一帧)
void AppSettings::_mailFolderBtn_cb(lv_event_t* e)
{
    auto* ctx = static_cast<MailCtx*>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    AppSettings* self = ctx->self;
    if (self->_mail_folder == ctx->a) return;         // 已在当前夹
    self->_mail_folder = ctx->a;
    self->_mail_folder_disp = ctx->b;
    self->_mail_total = 0;
    self->_mail_got = false;
    self->_mailCollapseSwipe(false);
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        mailc_msgs.clear();                           // 切夹先清旧列表
        mailc_total = 0;
    }
    self->_mailStatus("获取中…");
    mailcFetchList(self->_mail_acct, self->_mail_folder);
    self->_mail_pending_render = true;                // 下一帧重渲染 (高亮+空列表)
}

void AppSettings::_mailMsgOpen_cb(lv_event_t* e)
{
    auto* ctx = static_cast<MailCtx*>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    AppSettings* self = ctx->self;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        const MailMsg* found = nullptr;
        for (const auto& m : mailc_msgs)
            if (m.uid == ctx->a && m.fld == ctx->b) { found = &m; break; }
        if (!found) return;                           // 乐观更新后行可能已被滤掉
        mailc_cur_msg = *found;
        mailc_body.clear();                           // 详情数据清空等回执
        mailc_body_uid.clear();
        mailc_body_fld.clear();
        mailc_body_ok.store(false);
        mailc_meta = MailMeta();
    }
    self->_mail_pending_lv = MailLv::Detail;
    self->_mail_pending = true;
    mailcFetchBody(self->_mail_acct, ctx->b, ctx->a);
}

void AppSettings::_mailMarkBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self) return;
    std::string uid, fld;
    bool seen;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        uid = mailc_cur_msg.uid;
        fld = mailc_cur_msg.fld;
        seen = mailc_cur_msg.seen;
    }
    self->mail_do_action(seen ? "mark_unread" : "mark_read", uid, fld,
                         seen ? "已标为未读" : "已标为已读");
}

void AppSettings::_mailFlagBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self) return;
    std::string uid, fld;
    bool flagged;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        uid = mailc_cur_msg.uid;
        fld = mailc_cur_msg.fld;
        flagged = mailc_cur_msg.flagged;
    }
    self->mail_do_action(flagged ? "unflag" : "flag", uid, fld,
                         flagged ? "已取消红旗" : "已红旗");
}

void AppSettings::_mailReplyBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self) return;
    std::string msg_id, refs, from_addr, date, subj, body_txt;
    {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        msg_id    = mailc_meta.msg_id;
        refs      = mailc_meta.refs;
        from_addr = mailc_meta.from_addr;
        date      = mailc_meta.date;
        subj      = mailc_meta.subject;
        body_txt  = mailc_body;
    }
    if (msg_id.empty() && from_addr.empty()) {
        self->_mailStatus("正文未加载完, 稍后再试");
        return;
    }
    // 引用块: 截 500 字 → "> " 逐行 (oneapp mail_compose_open 同款)
    std::string q = body_txt;
    if (q.size() > 500) q = q.substr(0, 500) + " …(截断)";
    std::string quote = "在 " + (date.empty() ? "?" : date) + "，" +
                        (from_addr.empty() ? "对方" : from_addr) + " 写道：\n";
    size_t start = 0;
    while (start <= q.size()) {
        size_t nl = q.find('\n', start);
        std::string line = q.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        quote += "> " + line + "\n";
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    self->_mail_quote = quote;
    self->_mail_reply = true;
    self->_mail_pending_lv = MailLv::Compose;
    self->_mail_pending = true;
}

void AppSettings::_mailRefreshBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self) return;
    self->_mailStatus("刷新中…");
    mailcFetchList(self->_mail_acct, self->_mail_folder);
}

void AppSettings::_mailComposeBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self) return;
    self->_mail_reply = false;
    self->_mail_quote.clear();
    self->_mail_pending_lv = MailLv::Compose;
    self->_mail_pending = true;
}

void AppSettings::_mailSendBtn_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self || !self->_mail_ta_to) return;
    if (self->_mail_sending) return;
    auto trim = [](std::string s) {
        size_t p = s.find_first_not_of(" \t\r\n");
        if (p == std::string::npos) return std::string();
        size_t q = s.find_last_not_of(" \t\r\n");
        return s.substr(p, q - p + 1);
    };
    std::string to = trim(lv_textarea_get_text(self->_mail_ta_to));
    std::string subj = self->_mail_ta_subj ? lv_textarea_get_text(self->_mail_ta_subj) : "";
    std::string body = self->_mail_ta_body ? lv_textarea_get_text(self->_mail_ta_body) : "";
    if (to.find('@') == std::string::npos) {
        self->_mailStatus("收件人格式不对");
        return;
    }
    std::string in_reply_to, references;
    if (self->_mail_reply) {
        std::lock_guard<std::mutex> lk(mailc_mutex);
        in_reply_to = mailc_meta.msg_id;         // 回复线程头 (oneapp send_mail 同款)
        references = mailc_meta.refs;
        if (!self->_mail_quote.empty())
            body = body + "\n\n" + self->_mail_quote;   // 引用拼在用户文字后
    }
    self->_mail_sending = true;
    self->_mailStatus("发送中… (SMTP)");
    mailcSend(self->_mail_acct, to, trim(subj), body, in_reply_to, references);
}

void AppSettings::_mailTa_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* ta = lv_event_get_current_target_obj(e);   // LVGL9.2: 返回 void*
    lv_event_code_t code = lv_event_get_code(e);
    if (!self || !ta) return;
    if (code == LV_EVENT_FOCUSED) {
        bool voice = (ta == self->_mail_ta_subj || ta == self->_mail_ta_body);
        self->_mailKbShow(ta, voice);
        if (self->_mail_content) lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else if (code == LV_EVENT_DEFOCUSED) {
        self->_mailKbHide();
    }
}

void AppSettings::_mailKb_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (self && (code == LV_EVENT_READY || code == LV_EVENT_CANCEL))
        self->_mailKbHide();
}

void AppSettings::_mailAct_cb(lv_event_t* e)
{
    auto* ctx = static_cast<MailCtx*>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    static const struct { const char* act; const char* tip; } kTips[] = {
        {"flagread",    "已读 + 红旗"},
        {"unflag",      "已取消红旗"},
        {"mark_read",   "已标为已读"},
        {"mark_unread", "已标为未读"},
    };
    const char* tip = "已完成";
    for (const auto& k : kTips)
        if (ctx->a == k.act) { tip = k.tip; break; }
    // flagread = 已读+红旗两连发 (csv 一次 worker 顺序两请求, 失败整体回滚)
    if (ctx->a == "flagread")
        ctx->self->mail_do_action("mark_read,flag", ctx->c, ctx->b, "已读 + 红旗");
    else
        ctx->self->mail_do_action(ctx->a, ctx->c, ctx->b, tip);
}

void AppSettings::_mailRowPressed_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* row = lv_event_get_current_target_obj(e);
    if (!self || !row) return;
    if (self->_mail_swipe_open && self->_mail_swipe_open != row)
        self->_mailCollapseSwipe(true);
}

void AppSettings::_mailRowScroll_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* row = lv_event_get_current_target_obj(e);
    if (!self || !row || !self->_email_page) return;
    const int x = (int)lv_obj_get_scroll_x(row);
    const int target = (x < 80) ? 0 : (x < 238 ? 160 : 316);
    if (target != x) {
        lv_obj_scroll_to_x(row, target, LV_ANIM_ON);   // 吸合; 第二次 END 再登记
        return;
    }
    if (target == 200) {
        if (self->_mail_swipe_open == row) self->_mail_swipe_open = nullptr;
    } else {
        self->_mail_swipe_open = row;
        self->_mail_swipe_ts = lv_tick_get();
    }
}

void AppSettings::_mailSwipeTimer_cb(lv_timer_t* t)
{
    auto* self = static_cast<AppSettings*>(lv_timer_get_user_data(t));
    if (!self || !self->_email_page || !self->_mail_swipe_open) return;
    if (lv_tick_elaps(self->_mail_swipe_ts) < 5000) return;
    self->_mailCollapseSwipe(true);   // 5s 无操作自动合拢
}

