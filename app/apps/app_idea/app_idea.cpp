/*
 * 想法 (idea) — 复刻 car-dash 车机页「想法」面板。
 *
 * 双栏: 左=记录卡 (文字 textarea + 分类 chips + 录音 + 语音转写),
 * 右=灵感时间线 (按天分组卡片流, 分类过滤, 删除/清空)。
 * 数据链路: hermes :8766 的 /api/idea 系列端点 (与车机网页同一份 Pi ~/idea_data/)。
 *
 * 设备专属管线 (web/桌面构建裁掉):
 * - 录音: xiaozhi_set_input_tap 订阅 16kHz mono s16 (订阅期暂停唤醒词) →
 *   PSRAM 线性 PCM (≤60s) → 补 WAV 头 → 64KB 分片 POST voice-upload
 *   (esp_hosted 大 POST 断言规避, 双色球 70KB 实锤安全档)。
 * - 回放: GET audio/{id}[?fmt=wav] — 非 wav 条目由 hermes ffmpeg 按需转码
 *   16kHz mono → 16k→24k 重采样 → xiaozhi_codec_write (静态互斥)。
 * - 管线状态全文件域静态, worker 不捕获 this (离开 app/析构后安全收尾)。
 */
#include "app_idea.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <mutex>
#include <algorithm>

#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"
#endif
#include "../app_voice_input/app_voice_input.h"

#ifndef PLATFORM_BUILD_DESKTOP
#include <xiaozhi_ctl.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

static const char* TAG = "idea";

// ── 字体 (app_timer 同款三档) ─────────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP
static const lv_font_t* zh_font_lg()
{
    extern const uint8_t font_puhui_common_30_4_bin_start[]
        asm("_binary_font_puhui_common_30_4_bin_start");
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_md()
{
    extern const uint8_t font_puhui_common_30_4_bin_start[]
        asm("_binary_font_puhui_common_30_4_bin_start");
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p))
                 : cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    }();
    return f;
}
#elif defined(PLATFORM_BUILD_WEB)
// web: 三档 cbin 与真机逐字节一致 (资产镜像)
#include "web_fonts.h"
static const lv_font_t* zh_font_lg() { return webui::font_30(); }
static const lv_font_t* zh_font_md() { return webui::font_24(); }
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
#endif

// ── 主题快照 (app_timer 同款) ────────────────────────────────────────────
static uint32_t C_BG, C_CARD, C_CARD0, C_ON, C_ACCENT, C_TEXT, C_TEXT2, C_TRACK, C_DANGER;
static void snap_theme()
{
    const auto& T = theme::cur();
    C_BG     = T.page;
    C_CARD   = T.card[1];
    C_CARD0  = T.card[0];
    C_ON     = T.press;
    C_ACCENT = T.accent;
    C_TEXT   = T.text_hi;
    C_TEXT2  = T.text_dim;
    C_TRACK  = T.track;
    C_DANGER = T.danger;
}

// 分类表 (与 car.html c-work/c-life/c-idea/c-todo 对应, 固定配色不随主题)
static const struct CatDef { const char* id; const char* name; uint32_t color; } CATS[4] = {
    {"work", "工作", 0x4FA3FF},
    {"life", "生活", 0x34D399},
    {"idea", "创意", 0xFBBF24},
    {"todo", "待办", 0xF87171},
};
static const int N_CATS = 4;

// ── 卡片回调 payload (LV_EVENT_DELETE 自释放: 卡片可能不点就被重建冲掉) ──
struct DelReq {
    AppIdea* app;
    std::string id;
    bool voice;
};
struct PlayReq {
    AppIdea* app;
    std::string id;
    std::string mime;
    int dur;
};
struct ConfirmReq {
    AppIdea* app;
    std::string id;
    bool voice;
    bool clear;
};

// ── 小工具 ────────────────────────────────────────────────────────────────
static long long epoch_ms()
{
    return (long long)std::time(nullptr) * 1000;
}

static std::string gen_id()
{
    static const char* AL = "abcdefghijklmnopqrstuvwxyz0123456789";
    char buf[40], suf[7];
    for (int i = 0; i < 6; i++) suf[i] = AL[rand() % 36];
    suf[6] = 0;
    snprintf(buf, sizeof buf, "%lld-%s", epoch_ms(), suf);
    return buf;
}

static void wav_header(uint8_t* h, uint32_t data_len)
{
    auto w16 = [&](int off, uint16_t v) { h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF; };
    auto w32 = [&](int off, uint32_t v) {
        h[off] = v & 0xFF; h[off + 1] = (v >> 8) & 0xFF;
        h[off + 2] = (v >> 16) & 0xFF; h[off + 3] = (v >> 24) & 0xFF;
    };
    memcpy(h, "RIFF", 4);      w32(4, 36 + data_len);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); w32(16, 16);
    w16(20, 1);                // PCM
    w16(22, 1);                // mono
    w32(24, 16000);            w32(28, 16000 * 2);
    w16(32, 2);                w16(34, 16);
    memcpy(h + 36, "data", 4); w32(40, data_len);
}

static std::string hhmm(long long ms)
{
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tm_;
    localtime_r(&t, &tm_);
    char buf[24];
    snprintf(buf, sizeof buf, "%02d:%02d", tm_.tm_hour, tm_.tm_min);
    return buf;
}

// 分组条: 今天/昨天/MM-DD (跨年补年份)
static std::string day_label(long long ms)
{
    std::time_t now = std::time(nullptr);
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tn, td;
    localtime_r(&now, &tn);
    localtime_r(&t, &td);
    long long day_now = (long long)now - (tn.tm_hour * 3600 + tn.tm_min * 60 + tn.tm_sec);
    long long day_it  = (long long)t - (td.tm_hour * 3600 + td.tm_min * 60 + td.tm_sec);
    long long dd = (day_now - day_it) / 86400;
    char buf[40];
    if (dd <= 0) return "今天";
    if (dd == 1) return "昨天";
    if (tn.tm_year == td.tm_year) snprintf(buf, sizeof buf, "%02d-%02d", td.tm_mon + 1, td.tm_mday);
    else snprintf(buf, sizeof buf, "%d-%02d-%02d", td.tm_year + 1900, td.tm_mon + 1, td.tm_mday);
    return buf;
}

static const CatDef* cat_of(const std::string& id)
{
    for (int i = 0; i < N_CATS; i++)
        if (id == CATS[i].id) return &CATS[i];
    return &CATS[0];
}

AppIdea::AppIdea()
{
    setAppInfo().name = "想法";
}

AppIdea::~AppIdea()
{
#ifndef PLATFORM_BUILD_DESKTOP
    // 静态管线缓冲留给进程复用 (只分配一次); 若还在录音/回放, 先停
    _playStop();
    if (s_recording) _recStop(false);
#endif
}

// ── 静态管线状态定义 ──────────────────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP
AppIdea* AppIdea::s_tap_target = nullptr;
std::mutex AppIdea::s_rec_mu;
std::condition_variable AppIdea::s_rec_cv;
std::deque<int16_t> AppIdea::s_backlog;
uint8_t* AppIdea::s_pcm = nullptr;
size_t AppIdea::s_pcm_len = 0;
std::atomic<bool> AppIdea::s_recording{false};
std::atomic<bool> AppIdea::s_rec_full{false};
std::atomic<bool> AppIdea::s_uploading{false};
std::atomic<int> AppIdea::s_up_part{0};
std::atomic<int> AppIdea::s_up_parts{0};
std::atomic<bool> AppIdea::s_up_failed{false};
std::atomic<bool> AppIdea::s_play_failed{false};
std::atomic<bool> AppIdea::s_rec_discarded{false};
std::atomic<int> AppIdea::s_saved_sec{-1};
std::atomic<bool> AppIdea::s_need_refresh{false};
uint32_t AppIdea::s_rec_start = 0;
std::string AppIdea::s_rec_id;
std::string AppIdea::s_rec_cat = "work";
uint8_t* AppIdea::s_play_buf = nullptr;
std::atomic<bool> AppIdea::s_playing{false};
std::atomic<bool> AppIdea::s_play_stop{false};
std::atomic<size_t> AppIdea::s_play_pos{0};
std::atomic<int> AppIdea::s_play_total{0};
#endif

// ── 生命周期 ──────────────────────────────────────────────────────────────
void AppIdea::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    if (_scr) _teardown();                    // 残留屏先拆, 幂等重建 (gallery 同款)
    _alive = std::make_shared<bool>(true);    // 成对铁律: 拆屏置 false 后此处必须重建
    snap_theme();
    _buildUi();
    lv_screen_load(_scr);                     // 必须: 不 load 则 onRunning 误判失焦
    _installSwipeGesture();

    if (GetHAL()->lvKbGroup && _ta) {
        lv_group_add_obj(GetHAL()->lvKbGroup, _ta);
    }
    _fetch();
}

void AppIdea::onRunning()
{
    // 失焦收尾 (openApp 其他 app 不触发 onClose, 自己发现被切走)
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down");
        _teardown();
        return;
    }
    if (!_scr) return;

    // 临时状态提示超时 → 恢复默认 (2026-11-05: 修「播放中/删除中」等挂留不消失)
    if (_status_reset_ms && (int32_t)(GetHAL()->millis() - _status_reset_ms) >= 0) {
        _status_reset_ms = 0;
        _setStatus("", C_TEXT2);
    }
    if (_op_done.exchange(false)) {
        _setStatus("", C_TEXT2);   // 删除/清空完成立即恢复
        _status_reset_ms = 0;
    }

#ifndef PLATFORM_BUILD_DESKTOP
    // 管线结果 → 统一 instance 提示位
    int s2 = s_saved_sec.exchange(-1);
    if (s2 >= 0) _saved_sec = s2;
    if (s_up_failed.exchange(false)) _setStatus("录音上传失败", C_DANGER);
    if (s_play_failed.exchange(false)) _setStatus("回放失败", C_DANGER);
#endif

    if (_list_ready.exchange(false)) _renderList();

    // 拉取失败 4s 后自动重试 (成功后 worker 置 UINT32_MAX 停)
    if (!_fetch_in_flight.load() && _retry_ms != UINT32_MAX &&
        (int32_t)(GetHAL()->millis() - _retry_ms) >= 0) {
        _fetch();
    }
    if (_refresh_needed.exchange(false)) _fetch();

    int saved = _saved_sec.exchange(-1);
    if (saved >= 0) {
        char buf[48];
        if (saved > 0) snprintf(buf, sizeof buf, "语音已保存 (%ds)", saved);
        else           snprintf(buf, sizeof buf, "灵感已保存");
        _setStatus(buf, theme::cur().ok);
        _status_reset_ms = GetHAL()->millis() + 3000;   // 结果提示 3s 后自动消失
    }
    if (_op_failed.exchange(false)) {
        _setStatus("操作失败, 请重试", C_DANGER);
        _status_reset_ms = GetHAL()->millis() + 3000;
    }

#ifndef PLATFORM_BUILD_DESKTOP
    // ── 录音状态 ──
    if (s_recording.load()) {
        uint32_t el = (GetHAL()->millis() - s_rec_start) / 1000;
        if (_mic_lbl) {
            char buf[32];
            snprintf(buf, sizeof buf, "停止 %us", (unsigned)el);
            lv_label_set_text(_mic_lbl, buf);
        }
        if (el >= (uint32_t)REC_MAX_SEC || s_rec_full.load()) {
            _recStop(true);   // 60s 上限自动停并上传
        }
    } else if (_mic_lbl) {
        const char* want = s_uploading.load() ? "上传中…" : "保存语音";
        if (strcmp(lv_label_get_text(_mic_lbl), want) != 0) {
            lv_label_set_text(_mic_lbl, want);
        }
    }
    if (s_uploading.load() && _status_lbl) {
        char buf[48];
        snprintf(buf, sizeof buf, "上传中 %d/%d…", s_up_part.load(), s_up_parts.load());
        lv_label_set_text(_status_lbl, buf);
        lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_ACCENT), 0);
    }
    if (s_need_refresh.exchange(false)) {
        _fetch();
    }
    if (s_rec_discarded.exchange(false)) {
        _setStatus("太短, 已丢弃", C_TEXT2);
        _status_reset_ms = GetHAL()->millis() + 3000;
    }

    // ── 回放进度 (单实例; 播完/停止后自动复位) ──
    if (_play_fill) {
        if (s_playing.load() && s_play_total.load() > 0) {
            int pct = (int)((size_t)s_play_pos.load() * 100 / (size_t)s_play_total.load());
            if (pct > 100) pct = 100;
            lv_obj_set_width(_play_fill, PLAY_BAR_W * pct / 100);
        } else if (!s_playing.load()) {
            lv_obj_set_width(_play_fill, 0);
            if (_play_btn_lbl) lv_label_set_text(_play_btn_lbl, "播放");
            _play_fill = nullptr;
            _play_btn_lbl = nullptr;
            _play_id.clear();
            _setStatus("", C_TEXT2);   // 播放结束, 「播放中…」不挂留
            _status_reset_ms = 0;
        }
    }
#endif
}

void AppIdea::onClose()
{
    mclog::tagInfo(TAG, "on close");
#ifndef PLATFORM_BUILD_DESKTOP
    if (s_recording) _recStop(true);    // 录音中离开: 停止并后台续传
    _playStop();
#endif
    _teardown();
    if (_close_cb) _close_cb();
}

void AppIdea::_teardown()
{
    _alive = std::make_shared<bool>(false);   // 作废手势/延迟回调
    _removeSwipeGesture();
#ifndef PLATFORM_BUILD_DESKTOP
    if (s_recording) _recStop(true);
    s_play_stop = true;                        // 回放 worker 自行退出复位
#endif
    if (GetHAL()->lvKbGroup && _ta) {
        lv_group_remove_obj(_ta);
    }
    if (_modal) { lv_obj_delete(_modal); _modal = nullptr; }
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _ta = _keyboard = _mic_btn = _mic_lbl = nullptr;
    _save_btn = _stt_btn = _status_lbl = nullptr;
    if (_swipe_timer) { lv_timer_del(_swipe_timer); _swipe_timer = nullptr; }
    _idea_swipe_open = nullptr;   // 行随 _scr 删除, 指针一并清
    _filter_row = _list_scroll = _list_panel = nullptr;
    _count_lbl = _play_fill = _play_btn_lbl = nullptr;
    {
        std::lock_guard<std::mutex> lk(_mu);
        _items.clear();
    }
    _play_id.clear();
    _play_dur = 0;
    _filter = "all";
    _sel_cat = 0;
}

// ── UI ────────────────────────────────────────────────────────────────────
void AppIdea::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_size(_scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 顶栏 ──
    lv_obj_t* header = lv_obj_create(_scr);
    lv_obj_set_size(header, SCREEN_W, HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(C_CARD0), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "想法");
    lv_obj_set_style_text_font(title, zh_font_lg(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme::cur().accent), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    _count_lbl = lv_label_create(header);
    lv_label_set_text(_count_lbl, "加载中…");
    lv_obj_set_style_text_font(_count_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_count_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_align(_count_lbl, LV_ALIGN_LEFT_MID, 130, 0);

    lv_obj_t* refresh_btn = lv_button_create(header);
    lv_obj_set_size(refresh_btn, 120, 56);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(C_ON), 0);
    lv_obj_set_style_radius(refresh_btn, 14, 0);
    lv_obj_add_event_cb(refresh_btn, [](lv_event_t* e) {
        static_cast<AppIdea*>(lv_event_get_user_data(e))->_fetch();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* rl = lv_label_create(refresh_btn);
    lv_label_set_text(rl, "刷新");
    lv_obj_set_style_text_font(rl, zh_font_md(), 0);
    lv_obj_center(rl);

    // ── 左栏: 记录卡 ──
    constexpr int LX = 24, LW = 400;
    const int LY = HEADER_H + 12;
    const int LH = SCREEN_H - LY - 16;
    lv_obj_t* card = lv_obj_create(_scr);
    lv_obj_set_pos(card, LX, LY);
    lv_obj_set_size(card, LW, LH);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _ta = lv_textarea_create(card);
    lv_obj_set_size(_ta, LW - 32, 190);
    lv_obj_align(_ta, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_textarea_set_placeholder_text(_ta, "记录灵感…");
    lv_textarea_set_one_line(_ta, false);
    lv_obj_set_style_text_font(_ta, zh_font_md(), 0);
    lv_obj_set_style_text_font(_ta, zh_font_md(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(_ta, lv_color_hex(C_CARD0), 0);
    lv_obj_set_style_border_width(_ta, 0, 0);
    lv_obj_set_style_radius(_ta, 12, 0);
    lv_obj_set_style_pad_all(_ta, 12, 0);
    lv_obj_add_event_cb(_ta, [](lv_event_t* e) {
        static_cast<AppIdea*>(lv_event_get_user_data(e))->_showKeyboard();
    }, LV_EVENT_CLICKED, this);

    // 分类 chips (单选; card 子序: 0=textarea, 1..4=chips) — 2×2:
    // 工作生活一行 / 创意待办一行, 宽 178 占满卡内宽 (368=178*2+12);
    // 行步进 64 (52 高 + 12 行距), 底部 318
    for (int i = 0; i < N_CATS; i++) {
        lv_obj_t* chip = lv_button_create(card);
        lv_obj_set_size(chip, 178, 52);
        lv_obj_align(chip, LV_ALIGN_TOP_LEFT, (i % 2) * 190, 202 + (i / 2) * 64);
        lv_obj_set_style_radius(chip, 26, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(i == _sel_cat ? CATS[i].color : C_TRACK), 0);
        lv_obj_set_user_data(chip, (void*)(intptr_t)i);
        lv_obj_add_event_cb(chip, [](lv_event_t* e) {
            auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
            int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
            self->_sel_cat = i;
            // 重刷 4 个 chip 底色 + 文字色 (子序 1..4; 选中=分类亮底黑字,
            // 未选中=track 深底白字 — 只刷背景会留黑字在深底上看不见)
            lv_obj_t* p = lv_obj_get_parent(lv_event_get_current_target_obj(e));
            for (int k = 0; k < N_CATS; k++) {
                lv_obj_t* c = lv_obj_get_child(p, k + 1);
                bool sel = (k == self->_sel_cat);
                lv_obj_set_style_bg_color(c, lv_color_hex(sel ? CATS[k].color : self->track_color()), 0);
                lv_obj_t* lbl = lv_obj_get_child(c, 0);
                if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0x10151A : C_TEXT), 0);
            }
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* cl = lv_label_create(chip);
        lv_label_set_text(cl, CATS[i].name);
        lv_obj_set_style_text_font(cl, zh_font_md(), 0);
        lv_obj_set_style_text_color(cl, lv_color_hex(i == _sel_cat ? 0x10151A : C_TEXT), 0);
        lv_obj_center(cl);
    }

    // 录音大按钮 + 底部两钮的 y (设备才有录音行)。
    // 2026-11-12 用户定稿两行对调: chips 底 318 → 保存/转写 338..406 →
    // 语音大钮 446..538; 无 mic 平台: 保存 338 起。
    int btn_y = 338;
#ifndef PLATFORM_BUILD_DESKTOP
    lv_obj_t* mic = lv_button_create(card);
    lv_obj_set_size(mic, LW - 32, 92);
    lv_obj_align(mic, LV_ALIGN_TOP_LEFT, 0, 446);
    lv_obj_set_style_bg_color(mic, lv_color_hex(C_DANGER), 0);
    lv_obj_set_style_bg_color(mic, lv_color_mix(lv_color_hex(0), lv_color_hex(C_DANGER), 178), LV_STATE_PRESSED);
    lv_obj_set_style_radius(mic, 46, 0);
    lv_obj_add_event_cb(mic, [](lv_event_t* e) {
        auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
        if (AppIdea::s_recording.load()) self->_recStop(true);
        else self->_recStart();
    }, LV_EVENT_CLICKED, this);
    _mic_lbl = lv_label_create(mic);
    lv_label_set_text(_mic_lbl, "保存语音");
    lv_obj_set_style_text_font(_mic_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_mic_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(_mic_lbl);
    btn_y = 338;   // 保存/转写行上移至 chips 下方 (对调后)
#endif

    // 保存 + 语音转写
    _save_btn = lv_button_create(card);
    lv_obj_set_size(_save_btn, 210, 68);
    lv_obj_align(_save_btn, LV_ALIGN_TOP_LEFT, 0, btn_y);
    lv_obj_set_style_bg_color(_save_btn, lv_color_hex(theme::cur().accent), 0);
    lv_obj_set_style_radius(_save_btn, 14, 0);
    lv_obj_add_event_cb(_save_btn, [](lv_event_t* e) {
        static_cast<AppIdea*>(lv_event_get_user_data(e))->_saveText();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* sl = lv_label_create(_save_btn);
    lv_label_set_text(sl, "保存文字");
    lv_obj_set_style_text_font(sl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(0x10151A), 0);
    lv_obj_center(sl);

    _stt_btn = lv_button_create(card);
    lv_obj_set_size(_stt_btn, 142, 68);
    lv_obj_align(_stt_btn, LV_ALIGN_TOP_LEFT, 226, btn_y);
    lv_obj_set_style_bg_color(_stt_btn, lv_color_hex(C_ON), 0);
    lv_obj_set_style_radius(_stt_btn, 14, 0);
    lv_obj_add_event_cb(_stt_btn, [](lv_event_t* e) {
        auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
#ifndef PLATFORM_BUILD_DESKTOP
        if (AppIdea::s_recording.load()) { self->_setStatus("录音中, 先停止", C_DANGER); return; }
#endif
        // 大麦克风模式 (SSH 同款): 全屏波图 + 立即录音, 静音 2s 自动停,
        // 识别文字自动填入输入框 (requestVoiceInput 小 mic 需再点一次停)
        AppVoiceInput::requestMicInput(self->_ta);
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* tl = lv_label_create(_stt_btn);
    lv_label_set_text(tl, "语音输入");
    lv_obj_set_style_text_font(tl, zh_font_md(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(tl);

    _status_lbl = lv_label_create(card);
    lv_label_set_text(_status_lbl, "");   // 用户定稿: 默认不显提示 (2026-11-12)
    lv_obj_set_style_text_font(_status_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_TEXT2), 0);
    lv_label_set_long_mode(_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_status_lbl, LW - 32);
    lv_obj_align(_status_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    // ── 右栏: 灵感时间线 (滚动) ──
    constexpr int RX = 448;
    const int RW = SCREEN_W - RX - 24;
    const int RY = LY, RH = SCREEN_H - RY - 16;
    _list_scroll = lv_obj_create(_scr);
    lv_obj_set_pos(_list_scroll, RX, RY);
    lv_obj_set_size(_list_scroll, RW, RH);
    lv_obj_set_style_bg_color(_list_scroll, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_list_scroll, 0, 0);
    lv_obj_set_style_pad_all(_list_scroll, 0, 0);
    lv_obj_set_style_pad_bottom(_list_scroll, 24, 0);
    lv_obj_set_scroll_dir(_list_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_list_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_clear_flag(_list_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);   // 否则底缘上拨误判为滚动 (gallery 实锤)

    _filter_row = lv_obj_create(_list_scroll);
    lv_obj_set_size(_filter_row, RW - 24, 60);
    lv_obj_align(_filter_row, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(_filter_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_filter_row, 0, 0);
    lv_obj_set_style_pad_all(_filter_row, 0, 0);
    lv_obj_clear_flag(_filter_row, LV_OBJ_FLAG_SCROLLABLE);

    _list_panel = lv_obj_create(_list_scroll);
    lv_obj_set_size(_list_panel, RW - 24, LV_SIZE_CONTENT);
    lv_obj_align(_list_panel, LV_ALIGN_TOP_LEFT, 0, 68);
    lv_obj_set_style_bg_opa(_list_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list_panel, 0, 0);
    lv_obj_set_style_pad_all(_list_panel, 0, 0);
    lv_obj_set_style_pad_row(_list_panel, 12, 0);
    lv_obj_clear_flag(_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(_list_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(_list_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 屏幕虚拟键盘 (默认隐藏; textarea 顶在左栏上部, 键盘盖下半屏不影响可见性)
    _keyboard = lv_keyboard_create(_scr);
    lv_obj_set_size(_keyboard, SCREEN_W, KB_H);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_keyboard, _ta);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
        static_cast<AppIdea*>(lv_event_get_user_data(e))->_hideKeyboard();
    }, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_keyboard, [](lv_event_t* e) {
        static_cast<AppIdea*>(lv_event_get_user_data(e))->_hideKeyboard();
    }, LV_EVENT_CANCEL, this);

    // 滑开行 5s 无操作自动合拢 (邮件子页同款; _teardown 删)
    if (_swipe_timer) lv_timer_del(_swipe_timer);
    _swipe_timer = lv_timer_create(_swipeTimer_cb, 500, this);

    _renderFilter();
}

// ── 记录卡片左拨露删除 (邮件滑动行同款: 单开 + 吸合 + 5s 自动合拢) ────────────
void AppIdea::_collapseSwipe(bool anim)
{
    lv_obj_t* row = _idea_swipe_open;
    _idea_swipe_open = nullptr;
    if (row) lv_obj_scroll_to_x(row, 0, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

void AppIdea::_swipeTimer_cb(lv_timer_t* t)
{
    auto* self = static_cast<AppIdea*>(lv_timer_get_user_data(t));
    if (!self || !self->_list_panel || !self->_idea_swipe_open) return;
    if (lv_tick_elaps(self->_swipe_ts) < 5000) return;
    self->_collapseSwipe(true);   // 5s 无操作自动合拢 (car-dash 同款)
}

// 分类底色用 track 色要过主题快照 — 成员转发 (chip 回调里拿不到 file-static)
uint32_t AppIdea::track_color() { return C_TRACK; }

void AppIdea::_renderFilter()
{
    if (!_filter_row) return;
    lv_obj_clean(_filter_row);

    auto mk_chip = [&](int slot, const char* name, bool sel, bool danger) {
        lv_obj_t* chip = lv_button_create(_filter_row);
        lv_obj_set_size(chip, 96, 48);
        lv_obj_align(chip, LV_ALIGN_LEFT_MID, slot * 104, 0);
        lv_obj_set_style_radius(chip, 24, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(sel ? theme::cur().accent : (danger ? C_DANGER : C_TRACK)), 0);
        lv_obj_t* l = lv_label_create(chip);
        lv_label_set_text(l, name);
        lv_obj_set_style_text_font(l, zh_font_md(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(sel ? 0x10151A : C_TEXT), 0);
        lv_obj_center(l);
        return chip;
    };

    int slot = 0;
    lv_obj_t* all_chip = mk_chip(slot++, "全部", _filter == "all", false);
    lv_obj_set_user_data(all_chip, (void*)(intptr_t)-1);
    lv_obj_add_event_cb(all_chip, [](lv_event_t* e) {
        auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
        self->_filter = "all";
        self->_renderFilter();
        self->_renderList();
    }, LV_EVENT_CLICKED, this);
    for (int i = 0; i < N_CATS; i++) {
        lv_obj_t* chip = mk_chip(slot++, CATS[i].name, _filter == CATS[i].id, false);
        lv_obj_set_user_data(chip, (void*)(intptr_t)i);
        lv_obj_add_event_cb(chip, [](lv_event_t* e) {
            auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
            int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
            self->_filter = CATS[i].id;
            self->_renderFilter();
            self->_renderList();
        }, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* clr = mk_chip(slot, "清空", false, true);
    lv_obj_add_event_cb(clr, [](lv_event_t* e) {
        static_cast<AppIdea*>(lv_event_get_user_data(e))->_showConfirm(
            "清空全部想法? (含车机端记录)", "", false, true);
    }, LV_EVENT_CLICKED, this);
}

void AppIdea::_renderList()
{
    if (!_list_panel) return;

    std::vector<IdeaItem> vis;
    int n_text = 0, n_voice = 0;
    {
        std::lock_guard<std::mutex> lk(_mu);
        n_text = (int)_items.size();
        for (const auto& it : _items) {
            if (it.voice) n_voice++;
            if (_filter == "all" || it.cat == _filter) vis.push_back(it);
        }
    }

    if (_count_lbl) {
        char buf[64];
        snprintf(buf, sizeof buf, "共 %d 条 · 语音 %d", n_text, n_voice);
        lv_label_set_text(_count_lbl, buf);
    }

    lv_obj_clean(_list_panel);
    _idea_swipe_open = nullptr;   // 旧行已删, 滑开状态一并清 (邮件同款)
    _play_fill = nullptr;        // 重建后旧进度条失效
    _play_btn_lbl = nullptr;
    _play_id.clear();

    if (vis.empty()) {
        lv_obj_t* l = lv_label_create(_list_panel);
        lv_label_set_text(l, "暂无想法 — 说话或打字记录一条");
        lv_obj_set_style_text_font(l, zh_font_md(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT2), 0);
        return;
    }

    std::string last_day;
    for (const auto& it : vis) {
        // 分组条
        std::string day = day_label(it.created);
        if (day != last_day) {
            last_day = day;
            lv_obj_t* hd = lv_label_create(_list_panel);
            std::string txt = "— " + day + " —";
            lv_label_set_text(hd, txt.c_str());
            lv_obj_set_style_text_font(hd, zh_font_md(), 0);
            lv_obj_set_style_text_color(hd, lv_color_hex(C_TEXT2), 0);
        }

        // 行视口 (邮件滑动行同款, 2026-11-12): 横向滚动容器, 卡片占满行宽,
        // 删除区藏在卡片右侧 — 左拨露出。单开原则 + SCROLL_END 吸合。
        lv_obj_t* row = lv_obj_create(_list_panel);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_scroll_dir(row, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);  // 拖到头不链给纵向列表

        // 卡片
        const CatDef* cat = cat_of(it.cat);
        lv_obj_t* card = lv_obj_create(row);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_style_pad_bottom(card, 18, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);  // PRESSED 冒泡给行做单开合拢

        // 头行: 分类 chip + 时间 + 删除
        lv_obj_t* cc = lv_obj_create(card);
        lv_obj_set_size(cc, 96, 44);
        lv_obj_align(cc, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(cc, lv_color_hex(cat->color), 0);
        lv_obj_set_style_radius(cc, 22, 0);
        lv_obj_set_style_border_width(cc, 0, 0);
        lv_obj_set_style_pad_all(cc, 0, 0);
        lv_obj_clear_flag(cc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* ccl = lv_label_create(cc);
        lv_label_set_text(ccl, cat->name);
        lv_obj_set_style_text_font(ccl, zh_font_md(), 0);
        lv_obj_set_style_text_color(ccl, lv_color_hex(0x10151A), 0);
        lv_obj_center(ccl);

        lv_obj_t* tm = lv_label_create(card);
        std::string ts = day + " " + hhmm(it.created);
        lv_label_set_text(tm, ts.c_str());
        lv_obj_set_style_text_font(tm, zh_font_md(), 0);
        lv_obj_set_style_text_color(tm, lv_color_hex(C_TEXT2), 0);
        lv_obj_align(tm, LV_ALIGN_TOP_LEFT, 108, 8);

        // 右侧删除区: 左拨卡片露出 (满程 216px), 点击直接删 (用户定稿: 不再弹
        // 确认窗 — 滑开是明确意图)。与卡片等高同形 (布局后量卡高, 2026-11-12)。
        lv_obj_update_layout(card);
        const int cw = lv_obj_get_width(card);
        const int ch = lv_obj_get_height(card);
        lv_obj_t* zone = lv_obj_create(row);
        lv_obj_set_size(zone, 200, ch);
        lv_obj_set_pos(zone, cw + 16, 0);
        lv_obj_set_style_radius(zone, 16, 0);
        lv_obj_set_style_bg_color(zone, lv_color_hex(theme::cur().danger), 0);
        lv_obj_set_style_radius(zone, 16, 0);
        lv_obj_set_style_border_width(zone, 0, 0);
        lv_obj_set_style_shadow_width(zone, 0, 0);
        lv_obj_set_style_pad_all(zone, 0, 0);
        lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(zone, LV_OBJ_FLAG_EVENT_BUBBLE);   // PRESSED 冒泡给行 (单开)
        {
            auto* req = new DelReq{this, it.id, it.voice};
            lv_obj_add_event_cb(zone, [](lv_event_t* e) {
                auto* req = static_cast<DelReq*>(lv_event_get_user_data(e));
                if (lv_event_get_code(e) == LV_EVENT_DELETE) { delete req; return; }
                if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
                auto* self = req->app;
                self->_deleteItem(req->id, req->voice);
            }, LV_EVENT_ALL, req);
        }
        lv_obj_t* dl = lv_label_create(zone);
        lv_label_set_text(dl, "删除");
        lv_obj_set_style_text_font(dl, zh_font_md(), 0);
        lv_obj_set_style_text_color(dl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(dl);

        // 单开原则: 按下本行时合拢已开的旧行 (邮件 _emailRowPressed_cb 同款)
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
            lv_obj_t* r = lv_event_get_current_target_obj(e);
            if (self && self->_idea_swipe_open && self->_idea_swipe_open != r)
                self->_collapseSwipe(true);
        }, LV_EVENT_PRESSED, this);
        // 松手吸合 {0, 满 216} + 单开登记 (邮件 _emailRowScroll_cb 同款简化:
        // 只有右侧删除区, 无左区 → 两档吸附)
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
            lv_obj_t* r = lv_event_get_current_target_obj(e);
            if (!self) return;
            const int x = (int)lv_obj_get_scroll_x(r);
            const int target = (x < 108) ? 0 : 216;
            if (target != x) {
                lv_obj_scroll_to_x(r, target, LV_ANIM_ON);   // 吸合; 第二次 END 再登记
                return;
            }
            if (target == 0) {
                if (self->_idea_swipe_open == r) self->_idea_swipe_open = nullptr;
            } else {
                self->_idea_swipe_open = r;
                self->_swipe_ts = lv_tick_get();
            }
        }, LV_EVENT_SCROLL_END, this);

        // 正文
        if (it.voice) {
#ifndef PLATFORM_BUILD_DESKTOP
            // 设备: 回放行 — 播放按钮(row 子0) + 进度条(子1) + 时长(子2)
            lv_obj_t* row = lv_obj_create(card);
            lv_obj_set_size(row, LV_PCT(100), 60);
            lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 54);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* pb = lv_button_create(row);
            lv_obj_set_size(pb, 108, 52);
            lv_obj_align(pb, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_set_style_bg_color(pb, lv_color_hex(C_ON), 0);
            lv_obj_set_style_radius(pb, 26, 0);
            {
                auto* preq = new PlayReq{this, it.id, it.mime, it.dur};
                lv_obj_add_event_cb(pb, [](lv_event_t* e) {
                    auto* preq = static_cast<PlayReq*>(lv_event_get_user_data(e));
                    if (lv_event_get_code(e) == LV_EVENT_DELETE) { delete preq; return; }
                    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
                    auto* self = preq->app;
                    lv_obj_t* btn = lv_event_get_current_target_obj(e);
                    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
                    lv_obj_t* bar = lv_obj_get_child(lv_obj_get_parent(btn), 1);
                    lv_obj_t* fill = lv_obj_get_child(bar, 0);
                    if (self->_play_id == preq->id && AppIdea::s_playing.load()) {
                        self->_playStop();                       // 再点 = 停止
                    } else {
                        self->_playStop();                       // 换一条: 先停旧
                        self->_playStart(preq->id, preq->dur, preq->mime, fill, lbl);
                    }
                }, LV_EVENT_ALL, preq);
            }
            lv_obj_t* pbl = lv_label_create(pb);
            lv_label_set_text(pbl, "播放");
            lv_obj_set_style_text_font(pbl, zh_font_md(), 0);
            lv_obj_set_style_text_color(pbl, lv_color_hex(C_TEXT), 0);
            lv_obj_center(pbl);

            lv_obj_t* bar = lv_obj_create(row);
            lv_obj_set_size(bar, PLAY_BAR_W, 14);
            lv_obj_align(bar, LV_ALIGN_LEFT_MID, 124, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(C_TRACK), 0);
            lv_obj_set_style_radius(bar, 7, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_pad_all(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* fill = lv_obj_create(bar);
            lv_obj_set_size(fill, 0, 14);
            lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_set_style_bg_color(fill, lv_color_hex(theme::cur().accent), 0);
            lv_obj_set_style_radius(fill, 7, 0);
            lv_obj_set_style_border_width(fill, 0, 0);
            lv_obj_set_style_pad_all(fill, 0, 0);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);

            char db[24];
            snprintf(db, sizeof db, "%d″", it.dur);
            lv_obj_t* durl = lv_label_create(row);
            lv_label_set_text(durl, db);
            lv_obj_set_style_text_font(durl, zh_font_md(), 0);
            lv_obj_set_style_text_color(durl, lv_color_hex(C_TEXT2), 0);
            lv_obj_align(durl, LV_ALIGN_LEFT_MID, 124 + PLAY_BAR_W + 12, 0);
#else
            // web/桌面: 设备回放管线被裁剪 (hal 无 PCM 输出), 只展示时长提示
            lv_obj_t* row = lv_obj_create(card);
            lv_obj_set_size(row, LV_PCT(100), 56);
            lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 54);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t* vlbl = lv_label_create(row);
            char vb[64];
            snprintf(vb, sizeof vb, "语音 %ds · 请在设备上回放", it.dur);
            lv_label_set_text(vlbl, vb);
            lv_obj_set_style_text_font(vlbl, zh_font_md(), 0);
            lv_obj_set_style_text_color(vlbl, lv_color_hex(C_TEXT2), 0);
            lv_obj_align(vlbl, LV_ALIGN_LEFT_MID, 0, 0);
#endif

            // 备注 (车机端可写, 本端只读展示)
            if (!it.text.empty()) {
                lv_obj_t* note = lv_label_create(card);
                lv_label_set_text(note, it.text.c_str());
                lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(note, LV_PCT(100));
                lv_obj_set_style_text_font(note, zh_font_md(), 0);
                lv_obj_set_style_text_color(note, lv_color_hex(C_TEXT2), 0);
                lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            }
        } else {
            lv_obj_t* body = lv_label_create(card);
            lv_label_set_text(body, it.text.c_str());
            lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(body, LV_PCT(100));
            lv_obj_set_style_text_font(body, zh_font_md(), 0);
            lv_obj_set_style_text_color(body, lv_color_hex(C_TEXT), 0);
            lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 52);
        }
    }
}

// ── 数据操作 ──────────────────────────────────────────────────────────────
std::string AppIdea::_base()
{
    return "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
}

void AppIdea::_fetch()
{
    if (_fetch_in_flight.exchange(true)) return;
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive]() {
        auto resp = GetHAL()->httpGet(_base() + "/api/idea/all");
        if (resp.status != 200 || resp.body.empty()) {
            mclog::tagWarn(TAG, "fetch failed status={}", resp.status);
            if (*alive) _retry_ms = GetHAL()->millis() + 4000;
            _fetch_in_flight = false;
            return;
        }
        _retry_ms = UINT32_MAX;
        std::vector<IdeaItem> items;
        auto j = nlohmann::json::parse(resp.body, nullptr, false);
        if (!j.is_discarded()) {
            if (j.contains("texts")) {
                for (auto& t : j["texts"]) {
                    IdeaItem it;
                    it.id = t.value("id", "");
                    it.text = t.value("text", "");
                    it.cat = t.value("categoryId", "work");
                    it.created = (long long)t.value("createdAt", 0LL);
                    items.push_back(std::move(it));
                }
            }
            if (j.contains("voices")) {
                for (auto& v : j["voices"]) {
                    IdeaItem it;
                    it.voice = true;
                    it.id = v.value("id", "");
                    it.text = v.value("text", "");
                    it.cat = v.value("categoryId", "work");
                    it.created = (long long)v.value("createdAt", 0LL);
                    it.dur = v.value("dur", 0);
                    it.mime = v.value("mime", "audio/webm");
                    items.push_back(std::move(it));
                }
            }
        }
        std::sort(items.begin(), items.end(), [](const IdeaItem& a, const IdeaItem& b) {
            return a.created > b.created;
        });
        {
            std::lock_guard<std::mutex> lk(_mu);
            _items = std::move(items);
        }
        if (*alive) _list_ready = true;
        _fetch_in_flight = false;
    });
}

void AppIdea::_saveText()
{
    if (!_ta) return;
    const char* txt = lv_textarea_get_text(_ta);
    if (!txt || !txt[0]) { _setStatus("先写点什么", C_DANGER); return; }
    if (_save_in_flight.exchange(true)) return;
    std::string body = nlohmann::json{
        {"id", gen_id()},
        {"text", std::string(txt)},
        {"categoryId", CATS[_sel_cat].id},
        {"createdAt", epoch_ms()},
    }.dump();
    lv_textarea_set_text(_ta, "");
    _setStatus("保存中…", C_TEXT2);
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, body]() {
        auto resp = GetHAL()->httpPost(_base() + "/api/idea/text", body,
                                       {{"Content-Type", "application/json"}});
        _save_in_flight = false;
        if (resp.status == 200) {
            _saved_sec = 0;
            _refresh_needed = true;
        } else {
            _op_failed = true;
        }
    }, 16 * 1024);
}

void AppIdea::_deleteItem(const std::string& id, bool voice)
{
    _setStatus("删除中…", C_TEXT2);
    std::string url = _base() + (voice ? "/api/idea/voice-del" : "/api/idea/text-del");
    std::string body = nlohmann::json{{"id", id}}.dump();
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, url, body]() {
        auto resp = GetHAL()->httpPost(url, body, {{"Content-Type", "application/json"}});
        if (resp.status == 200) {
            if (*alive) {
                _refresh_needed = true;
                _op_done = true;        // 「删除中…」完成, 状态标签恢复默认
            }
        } else if (*alive) {
            _op_failed = true;
        }
    });
}

void AppIdea::_clearAll()
{
    _setStatus("清空中…", C_TEXT2);
    std::string body = nlohmann::json{{"texts", true}, {"voices", true}}.dump();
    auto alive = _alive;
    GetHAL()->tryRunDetached([this, alive, body]() {
        auto resp = GetHAL()->httpPost(_base() + "/api/idea/clear-all", body,
                                       {{"Content-Type", "application/json"}});
        if (resp.status == 200) {
            if (*alive) {
                _refresh_needed = true;
                _op_done = true;        // 「清空中…」完成, 状态标签恢复默认
            }
        } else if (*alive) {
            _op_failed = true;
        }
    });
}

void AppIdea::_showConfirm(const std::string& msg, const std::string& id,
                           bool voice, bool clear)
{
    if (!_scr) return;
    if (_modal) { lv_obj_delete_async(_modal); _modal = nullptr; }

    auto* modal = lv_obj_create(_scr);
    _modal = modal;
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* box = lv_obj_create(modal);
    lv_obj_set_size(box, 640, 300);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD0), 0);
    lv_obj_set_style_radius(box, 24, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* l = lv_label_create(box);
    lv_label_set_text(l, msg.c_str());
    lv_obj_set_style_text_font(l, zh_font_lg(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 60);

    auto* req = new ConfirmReq{this, id, voice, clear};
    lv_obj_t* ok = lv_button_create(box);
    lv_obj_set_size(ok, 210, 76);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, -130, -30);
    lv_obj_set_style_bg_color(ok, lv_color_hex(C_DANGER), 0);
    lv_obj_set_style_radius(ok, 16, 0);
    lv_obj_add_event_cb(ok, [](lv_event_t* e) {
        auto* req = static_cast<ConfirmReq*>(lv_event_get_user_data(e));
        if (lv_event_get_code(e) == LV_EVENT_DELETE) { delete req; return; }
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        auto* self = req->app;
        if (self->_modal) { lv_obj_delete_async(self->_modal); self->_modal = nullptr; }
        if (req->clear) self->_clearAll();
        else self->_deleteItem(req->id, req->voice);
    }, LV_EVENT_ALL, req);
    lv_obj_t* okl = lv_label_create(ok);
    lv_label_set_text(okl, clear ? "清空" : "删除");
    lv_obj_set_style_text_font(okl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(okl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(okl);

    lv_obj_t* cancel = lv_button_create(box);
    lv_obj_set_size(cancel, 210, 76);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 130, -30);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_radius(cancel, 16, 0);
    lv_obj_add_event_cb(cancel, [](lv_event_t* e) {
        auto* self = static_cast<AppIdea*>(lv_event_get_user_data(e));
        if (self->_modal) { lv_obj_delete_async(self->_modal); self->_modal = nullptr; }
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    lv_obj_set_style_text_font(cl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(cl);
}

void AppIdea::_setStatus(const char* txt, uint32_t color)
{
    if (!_status_lbl) return;
    lv_label_set_text(_status_lbl, txt);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(color), 0);
}

void AppIdea::_showKeyboard()
{
    if (!_keyboard || !_ta) return;
    if (GetHAL()->usbKeyboardDetect()) return;   // 实体键盘直打, 不弹屏 (lora_chat 同款)
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_keyboard);
}

void AppIdea::_hideKeyboard()
{
    if (!_keyboard) return;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

// ── 底缘上拨退出 (gallery 变体: 滚动共存 + 弹窗只收弹窗) ──────────────────
void AppIdea::_installSwipeGesture()
{
    _removeSwipeGesture();
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        _gesture_ctx = new SwipeCtx{this, _alive, 0, 0, 0};
        lv_indev_add_event_cb(indev, [](lv_event_t* e) {
            auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
            auto alive = ctx->alive.lock();
            if (!alive || !*alive) return;
            AppIdea* app = ctx->app;
            if (!app->_scr || lv_screen_active() != app->_scr) return;   // 前台守卫
            lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
            const lv_event_code_t code = lv_event_get_code(e);
            lv_point_t p;
            if (code == LV_EVENT_PRESSED) {
                lv_indev_get_point(dev, &p);
                ctx->press_x = p.x;
                ctx->press_y = p.y;
                ctx->scroll_at_press = app->_list_scroll ? lv_obj_get_scroll_y(app->_list_scroll) : 0;
                return;
            }
            bool hit = false;
            if (code == LV_EVENT_GESTURE) {
                hit = lv_indev_get_gesture_dir(dev) == LV_DIR_TOP && ctx->press_y > 650;
            } else if (code == LV_EVENT_RELEASED) {
                lv_indev_get_point(dev, &p);
                const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                hit = ctx->press_y > 650 && dy < -120 && abs(dy) > abs(dx) * 2;
            }
            if (!hit) return;
            // 弹窗开着 → 只收弹窗 (radio 同款)
            if (app->_modal) {
                auto* defer = new SwipeCtx{*ctx};
                lv_async_call([](void* u) {
                    auto* c = static_cast<SwipeCtx*>(u);
                    auto a = c->alive.lock();
                    if (a && *a && c->app->_modal) {
                        lv_obj_delete_async(c->app->_modal);
                        c->app->_modal = nullptr;
                    }
                    delete c;
                }, defer);
                return;
            }
            // 列表滚动中的上拨不算退出 (滚动位移 >10px, lottery/gallery 同款)
            if (app->_list_scroll &&
                abs((int)lv_obj_get_scroll_y(app->_list_scroll) - ctx->scroll_at_press) > 10) return;
            auto* defer = new SwipeCtx{*ctx};
            lv_async_call([](void* u) {
                auto* c = static_cast<SwipeCtx*>(u);
                auto a = c->alive.lock();
                if (a && *a) c->app->_requestClose();
                delete c;
            }, defer);
        }, LV_EVENT_ALL, _gesture_ctx);
        _gesture_indev = indev;
        break;
    }
}

void AppIdea::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
        _gesture_ctx = nullptr;
    }
}

// ── 设备专属: 录音 / 回放 ─────────────────────────────────────────────────
#ifndef PLATFORM_BUILD_DESKTOP

void AppIdea::s_tap(const int16_t* data, size_t n)
{
    if (!s_tap_target || !s_recording.load()) return;
    {
        std::lock_guard<std::mutex> lk(s_rec_mu);
        if (s_backlog.size() > 32000) return;   // 积压 >2s 整段丢弃 (防爆内存, voice_input 同款)
        s_backlog.insert(s_backlog.end(), data, data + n);
    }
    s_rec_cv.notify_one();
}

void AppIdea::_recStart()
{
    if (s_recording.load()) return;
    if (s_uploading.load()) { _setStatus("上一条上传中, 稍候", C_DANGER); return; }
    if (!s_pcm) {
        s_pcm = (uint8_t*)heap_caps_malloc(REC_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm) { _setStatus("内存不足, 无法录音", C_DANGER); return; }
    }
    {
        std::lock_guard<std::mutex> lk(s_rec_mu);
        s_backlog.clear();
        s_pcm_len = 0;
    }
    s_rec_full = false;
    s_rec_id = gen_id();
    s_rec_cat = CATS[_sel_cat].id;
    s_rec_start = GetHAL()->millis();
    s_tap_target = this;
    if (!xiaozhi_set_input_tap(&AppIdea::s_tap)) {
        s_tap_target = nullptr;
        _setStatus("麦克风不可用", C_DANGER);
        return;
    }
    s_recording = true;
    if (!GetHAL()->tryRunDetached([]() { AppIdea::s_recWorker(); }, 32 * 1024)) {
        _recStop(false);   // worker 起不来: 立即停, 不留悬挂 tap
        _setStatus("线程不足, 无法录音", C_DANGER);
    } else {
        _setStatus("录音中… 点按钮停止 (≤60s)", C_ACCENT);
    }
}

void AppIdea::_recStop(bool upload)
{
    if (!s_recording.exchange(false)) {
        if (!upload) {
            std::lock_guard<std::mutex> lk(s_rec_mu);
            s_backlog.clear();
            s_pcm_len = 0;
        }
        return;
    }
    if (!upload) {
        std::lock_guard<std::mutex> lk(s_rec_mu);
        s_backlog.clear();
        s_pcm_len = 0;
    }
    s_tap_target = nullptr;
    xiaozhi_set_input_tap(nullptr);      // 恢复唤醒词
    s_rec_cv.notify_all();
}

void AppIdea::s_recWorker()
{
    // 攒 PCM (全走静态, 不捕获 this)
    for (;;) {
        std::vector<int16_t> chunk;
        {
            std::unique_lock<std::mutex> lk(s_rec_mu);
            if (s_backlog.empty()) {
                if (!s_recording.load()) break;
                s_rec_cv.wait_for(lk, std::chrono::milliseconds(200));
                continue;
            }
            chunk.assign(s_backlog.begin(), s_backlog.end());
            s_backlog.clear();
        }
        size_t bytes = chunk.size() * 2;
        size_t space = REC_CAP - s_pcm_len;
        size_t take = bytes < space ? bytes : space;
        // tap 给的是 AFE 增益前的裸 PCM, 幅度极低 (2026-11-05 实测设备录音
        // mean -43.5dB/max -26.4dB, 正常语音 ≈-25/-6) → 写盘时 ×8(+18dB)
        // 软件增益 + 饱和裁剪; 只影响本 app 录音文件, 唤醒词/STT 链路不动。
        int16_t* dst = (int16_t*)(s_pcm + s_pcm_len);
        size_t n_samples = take / 2;
        for (size_t i = 0; i < n_samples; i++) {
            int32_t v = (int32_t)chunk[i] * 8;
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            dst[i] = (int16_t)v;
        }
        s_pcm_len += n_samples * 2;
        if (s_pcm_len >= REC_CAP) s_rec_full = true;
    }
    // 收尾: tap 已 detach。≥1s 上传 (car 版同款误触判定), 不足即弃。
    if (s_pcm_len >= (size_t)(REC_RATE * 2)) {
        s_uploadRun();
    } else {
        std::lock_guard<std::mutex> lk(s_rec_mu);
        s_pcm_len = 0;
        s_rec_discarded = true;   // 太短丢弃, UI 提示 (状态标签别停在「录音中」)
    }
}

// 录音分片上传 (worker 线程内跑, 只碰静态; esp_hosted 大 POST 断言规避)
void AppIdea::s_uploadRun()
{
    s_uploading = true;
    s_up_failed = false;
    int dur = (int)(s_pcm_len / (REC_RATE * 2));
    const size_t total = 44 + s_pcm_len;
    const int parts = (int)((total + UP_CHUNK - 1) / UP_CHUNK);
    uint8_t hdr[44];
    wav_header(hdr, (uint32_t)s_pcm_len);
    s_up_parts = parts;
    mclog::tagInfo(TAG, "upload start id={} dur={}s bytes={} parts={}",
                   s_rec_id, dur, (unsigned long long)total, parts);
    bool ok = true;
    for (int i = 0; i < parts; i++) {
        s_up_part = i;
        size_t off = (size_t)i * UP_CHUNK;
        size_t end = std::min(total, off + UP_CHUNK);
        std::string body;
        body.resize(end - off);
        for (size_t k = off; k < end; k++)
            body[k - off] = (k < 44) ? (char)hdr[k] : (char)s_pcm[k - 44];
        char url[320];
        snprintf(url, sizeof url,
                 "%s/api/idea/voice-upload?id=%s&part=%d&parts=%d&dur=%d"
                 "&mime=audio/wav&cat=%s&ts=%lld",
                 _base().c_str(), s_rec_id.c_str(), i, parts, dur,
                 s_rec_cat.c_str(), epoch_ms());
        auto resp = GetHAL()->httpPost(url, body,
                                       {{"Content-Type", "application/octet-stream"}});
        bool pok = false;
        if (resp.status == 200) {
            auto j = nlohmann::json::parse(resp.body, nullptr, false);
            pok = !j.is_discarded() && j.value("ok", false);
        }
        if (!pok) {
            ok = false;
            mclog::tagWarn(TAG, "part {}/{} FAILED status={}", i, parts, resp.status);
            break;
        }
    }
    s_up_part = parts;
    s_uploading = false;
    if (ok) {
        s_saved_sec = dur;
        s_need_refresh = true;
        mclog::tagInfo(TAG, "voice uploaded id={} dur={}s", s_rec_id, dur);
    } else {
        s_up_failed = true;
    }
    std::lock_guard<std::mutex> lk(s_rec_mu);
    s_pcm_len = 0;   // 成功失败都清, 缓冲留给下次录音
}

// 16k→24k 线性重采样 (app_radio 同款; xiaozhi 输出固定 24kHz, 比率恰 2:3)
struct IdeaResampler {
    int32_t prev = 0;
    int phase = 0;
    int16_t out[3072];
    void process(const int16_t* in, int n, bool reset,
                 const std::function<void(const int16_t*, int)>& sink) {
        if (reset) { prev = 0; phase = 0; }
        int oc = 0;
        for (int i = 0; i < n; i++) {
            int32_t cur = in[i];
            while (phase < 3) {
                out[oc++] = (int16_t)(prev + (cur - prev) * phase / 3);
                if (oc == (int)(sizeof(out) / sizeof(out[0]))) {
                    sink(out, oc);
                    oc = 0;
                }
                phase += 2;
            }
            phase -= 3;
            prev = cur;
        }
        if (oc > 0) sink(out, oc);
    }
};

void AppIdea::_playStart(const std::string& id, int dur, const std::string& mime,
                         lv_obj_t* fill, lv_obj_t* lbl)
{
    _playStop();                       // 停旧的 (worker 经串行互斥自然退场)
    _play_id = id;
    _play_dur = dur;
    _play_fill = fill;
    _play_btn_lbl = lbl;
    if (lbl) lv_label_set_text(lbl, "停止");
    if (fill) lv_obj_set_width(fill, 0);
    _setStatus("播放中…", C_ACCENT);
    s_playing = true;
    s_play_stop = false;
    if (!GetHAL()->tryRunDetached([id, mime]() { AppIdea::s_playWorker(id, mime); }, 32 * 1024)) {
        s_playing = false;
        _setStatus("线程不足, 无法回放", C_DANGER);
    }
}

void AppIdea::_playStop()
{
    s_play_stop = true;                // worker 退出后 onRunning 自动复位 fill/lbl
}

void AppIdea::s_playWorker(std::string id, std::string mime)
{
    static std::mutex s_play_serial;   // 单实例播放: 新 worker 等旧的退场再抢写
    std::lock_guard<std::mutex> lk(s_play_serial);
    s_play_stop = false;
    s_play_pos = 0;
    s_play_total = 0;

    if (!s_play_buf) {
        s_play_buf = (uint8_t*)heap_caps_malloc(PLAY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_play_buf) { s_playing = false; s_play_failed = true; return; }
    }

    // 下载 (非 wav 条目走 hermes 按需转码, 结果缓存)
    std::string url = _base() + "/api/idea/audio/" + id +
                      (mime == "audio/wav" ? "" : "?fmt=wav");
    mclog::tagInfo(TAG, "play fetch {}", url);
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    size_t n = 0;
    if (http) {
        if (esp_http_client_open(http, 0) == ESP_OK) {
            esp_http_client_fetch_headers(http);
            if (esp_http_client_get_status_code(http) == 200) {
                int r;
                while ((r = esp_http_client_read(http, (char*)s_play_buf + n,
                                                 (int)(PLAY_CAP - n))) > 0) {
                    n += (size_t)r;
                    if (n >= PLAY_CAP || s_play_stop.load()) break;
                    if ((n & 0x7FFF) < (size_t)r) vTaskDelay(pdMS_TO_TICKS(2));   // 每 32KB 让 2ms
                }
            }
        }
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    if (s_play_stop.load()) { s_playing = false; return; }
    if (n < 100) {
        mclog::tagWarn(TAG, "play fetch failed n={}", (unsigned long long)n);
        s_playing = false;
        s_play_failed = true;
        return;
    }

    // RIFF 解析: 取 data chunk (hermes 转码恒 16kHz mono s16)
    if (n < 44 || memcmp(s_play_buf, "RIFF", 4) || memcmp(s_play_buf + 8, "WAVE", 4)) {
        s_playing = false;
        s_play_failed = true;
        return;
    }
    uint32_t rate = 0;
    uint16_t ch = 0;
    size_t pos = 12, data_off = 0, data_len = 0;
    while (pos + 8 <= n) {
        uint32_t sz;
        memcpy(&sz, s_play_buf + pos + 4, 4);
        if (!memcmp(s_play_buf + pos, "fmt ", 4) && sz >= 16) {
            memcpy(&ch, s_play_buf + pos + 10, 2);
            memcpy(&rate, s_play_buf + pos + 12, 4);
        }
        if (!memcmp(s_play_buf + pos, "data", 4)) {
            data_off = pos + 8;
            data_len = sz;
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!data_off || rate != (uint32_t)REC_RATE || ch != 1) {
        mclog::tagWarn(TAG, "wav unsupported rate={} ch={}", rate, ch);
        s_playing = false;
        s_play_failed = true;
        return;
    }
    size_t end = std::min(n, data_off + data_len);
    s_play_total = (int)(end - data_off);

    // 播放: 16k→24k 重采样 → codec_write (静态互斥, 与 play_tone/收音机同规)
    static std::mutex s_out_mu;
    static std::vector<int16_t> in_buf;   // 复用堆缓冲
    in_buf.resize(960);                   // 60ms @16k
    IdeaResampler rs;
    size_t off = data_off;
    bool first = true;
    while (off + in_buf.size() * 2 <= end) {
        if (s_play_stop.load()) break;
        memcpy(in_buf.data(), s_play_buf + off, in_buf.size() * 2);
        rs.process(in_buf.data(), (int)in_buf.size(), first, [](const int16_t* d, int cnt) {
            std::lock_guard<std::mutex> lks(s_out_mu);
            xiaozhi_codec_write(d, cnt);   // 阻塞式: DMA 满即背压
        });
        first = false;
        off += in_buf.size() * 2;
        s_play_pos = (int)(off - data_off);
    }
    s_playing = false;
    mclog::tagInfo(TAG, "play done id={} stop={}", id, (int)s_play_stop.load());
}
#endif  // !PLATFORM_BUILD_DESKTOP
