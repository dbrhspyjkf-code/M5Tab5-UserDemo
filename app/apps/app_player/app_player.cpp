/*
 * app_player — Sonos/HomePod 播放器实现(参考 M5DIAL app_sonos)。
 * HA 配置与 app_ha 同源: NVS ha_host + ha_weather::token()。
 */
#include "app_player.h"
#include "../utils/theme/theme.h"
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <mooncake_log.h>
#include "../app_ha/ha_weather.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>

// URL 百分号编码(歌词查询参数含中文/空格)
static std::string url_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 0xF]; }
    }
    return o;
}
#include <thread>
#include "../utils/datetime/lunar_calendar.h"

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include "../utils/assets/assets_partition.h"
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
static const lv_font_t* zh_font_md()
{
    // font_zh_24 在 assets 分区(app_radio 同源);找不到退回大字体,只影响观感
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p))
                 : cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    }();
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
#endif

static const char* TAG = "player";

// HA 的 media_position 是"时间戳时刻的位置", 静止不动 —— 真实位置须加
// (当前时刻 - media_position_updated_at)。这是 HA 标准算法(2026-10-26 进度
// 条 0:00-0:15 循环实锤: 不做校正, 阈值再多也会周期性误重置)。
static int64_t iso8601_utc_epoch(const std::string& s)
{
    int y, mo, d, h, mi; double sec;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6)
        return -1;
    int64_t days = lunar::detail::days_from_civil(y, mo, d);
    return days * 86400 + h * 3600 + mi * 60 + (int64_t)sec;
}

// 配色: 主题快照(onOpen 时从全局主题取, 见 utils/theme/theme.h)
static uint32_t C_BG, C_CARD, C_ON, C_ACCENT, C_TEXT, C_TEXT2;

static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    C_BG = 0x000000;
    C_CARD = T.card[1];  C_ON = T.press;  C_ACCENT = T.accent;
    C_TEXT = T.text_hi;  C_TEXT2 = T.text_dim;
}

// 页配置: 实体结论见头注释(M5DIAL 2026-10-22 验证)
const AppPlayer::PageCfg AppPlayer::_pages[kPageCount] = {
    {"客厅 SONOS",   "media_player.ke_ting_2"},
    {"起居室 HOMEPOD", "media_player.qi_ju_shi_2"},
};

AppPlayer::AppPlayer()
{
    setAppInfo().name = "AppPlayer";
}

void AppPlayer::onCreate()
{
    // 与 HA/收音机等一致: 安装不自动打开,等用户从主页点入
}

// ─── UI 构建 ───────────────────────────────────────────────────────────────

static lv_obj_t* mk_button(lv_obj_t* parent, int w, int h, lv_event_cb_t cb, void* user)
{
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    // 按下变色 (同主页按钮, 2026-10-28 用户要求)
    lv_obj_set_style_bg_color(btn, lv_color_mix(lv_color_hex(0), lv_color_hex(C_CARD), 178), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    return btn;
}

void AppPlayer::onOpen()
{
    if (_scr) return;   // 防御: 重复 onOpen 不叠屏
    mclog::tagInfo(TAG, "on open");
    snap_theme();
    // 前台独占(2026-10-31): 播放器界面期间完全不进屏保(不弹遮盖也不熄屏)
    if (_block) _block(true);

    // HA 客户端: 每次打开重建(Settings 改地址后重开即生效,同 app_ha)
    _ha = std::make_shared<HaClient>();
    {
        // app_ha 同源配置: NVS ha_host(默认走 .local) + token
        std::string host = GetHAL()->getConfig("ha_host", "orangepi6plus.local");
        HaClient::Config cfg;
        cfg.url   = ha_weather::baseUrl(host);
        cfg.token = ha_weather::token();
        cfg.pollIntervalMs = 4000;   // 播放器需要比 app_ha(12s)更跟手
        _ha->init(cfg);
    }
    _title.clear(); _artist.clear(); _state.clear();
    _volume = -1; _has_data = false;
    _pos = -1; _dur = -1;
    { std::lock_guard<std::mutex> lk(_lyr_mu); _lyr_t.clear(); _lyr_l.clear(); }
    _lyr_idx = -1;
    if (_lyr_now) lv_label_set_text(_lyr_now, "");

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 顶行: 当前源名居中(切页 = 屏幕左右拨动, 不再有页签按钮) ──
    _page_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_page_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_page_lbl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(_page_lbl, _pages[_page].tab);
    lv_obj_align(_page_lbl, LV_ALIGN_TOP_MID, 0, 32);   // 屏幕正中
    lv_obj_add_flag(_page_lbl, LV_OBJ_FLAG_CLICKABLE);  // 点击标题切换 SONOS/HomePod
    lv_obj_add_event_cb(_page_lbl, _tabClick_cb, LV_EVENT_CLICKED, this);

    // 状态 chip 已取消(2026-10-26): 播放状态由播放键图标表达

    // ── 右侧大封面(320×320,占位暗卡先垫底) ──
    _cover_card = lv_obj_create(_scr);
    lv_obj_set_pos(_cover_card, 720, 130);
    lv_obj_set_size(_cover_card, _COVER, _COVER);
    lv_obj_set_style_bg_color(_cover_card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(_cover_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_cover_card, 0, 0);
    lv_obj_set_style_radius(_cover_card, 16, 0);
    lv_obj_set_scrollbar_mode(_cover_card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(_cover_card, LV_OBJ_FLAG_SCROLLABLE);
    _cover_img = lv_image_create(_scr);
    lv_obj_set_pos(_cover_img, 720, 130);
    lv_obj_set_size(_cover_img, _COVER, _COVER);
    lv_obj_add_flag(_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(_cover_img, 16, 0);
    lv_obj_set_style_clip_corner(_cover_img, true, 0);

    // ── 中央 now-playing(左侧文字区,避开封面) ──
    _title_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_title_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_title_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(_title_lbl, 500);        // 中轴 = 进度条中心 x=360
    lv_obj_align(_title_lbl, LV_ALIGN_TOP_LEFT, 120, 150);
    lv_obj_set_style_text_align(_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(_title_lbl, "…");

    _artist_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_artist_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_artist_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_width(_artist_lbl, 500);
    lv_obj_align(_artist_lbl, LV_ALIGN_TOP_LEFT, 120, 218);
    lv_obj_set_style_text_align(_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_artist_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(_artist_lbl, "");

    // ── 播放进度条(纯显示, 不拖动; 无时长信息时整条隐藏) ──
    _prog_slider = lv_slider_create(_scr);
    lv_obj_set_size(_prog_slider, 520, 12);   // 与音量条同长同起点
    lv_obj_align(_prog_slider, LV_ALIGN_TOP_LEFT, 120, 288);   // 均分: 120|520|120|400|120
    lv_slider_set_range(_prog_slider, 0, 100);
    lv_obj_clear_flag(_prog_slider, LV_OBJ_FLAG_CLICKABLE);   // 取消拖动
    lv_obj_set_style_bg_color(_prog_slider, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_prog_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(_prog_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(_prog_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_prog_slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(_prog_slider, LV_OPA_TRANSP, LV_PART_KNOB);   // 旋钮隐形
    lv_obj_set_style_pad_all(_prog_slider, 0, LV_PART_KNOB);
    lv_obj_add_flag(_prog_slider, LV_OBJ_FLAG_HIDDEN);
    _prog_lbl = lv_label_create(_scr);        // 已播时间(条左)
    lv_obj_set_style_text_font(_prog_lbl, zh_font_md(), 0);   // 与音量百分比同字体
    lv_obj_set_style_text_color(_prog_lbl, lv_color_hex(C_TEXT2), 0);
    lv_label_set_text(_prog_lbl, "");
    lv_obj_set_width(_prog_lbl, 60);
    lv_obj_set_style_text_align(_prog_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(_prog_lbl, LV_ALIGN_TOP_LEFT, 52, 284);
    lv_obj_add_flag(_prog_lbl, LV_OBJ_FLAG_HIDDEN);
    _prog_total = lv_label_create(_scr);      // 总时长(条右)
    lv_obj_set_style_text_font(_prog_total, zh_font_md(), 0);   // 与音量百分比同字体
    lv_obj_set_style_text_color(_prog_total, lv_color_hex(C_TEXT2), 0);
    lv_label_set_text(_prog_total, "");
    lv_obj_align(_prog_total, LV_ALIGN_TOP_LEFT, 648, 284);
    lv_obj_add_flag(_prog_total, LV_OBJ_FLAG_HIDDEN);

    // ── 音量行 ──
    lv_obj_t* vol_icon = lv_label_create(_scr);
    lv_obj_set_style_text_color(vol_icon, lv_color_hex(C_TEXT2), 0);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(vol_icon, LV_ALIGN_TOP_LEFT, 68, 359);

    _vol_slider = lv_slider_create(_scr);
    lv_obj_set_size(_vol_slider, 520, 14);
    lv_obj_align(_vol_slider, LV_ALIGN_TOP_LEFT, 120, 365);
    lv_slider_set_range(_vol_slider, 0, 100);
    lv_obj_add_event_cb(_vol_slider, _vol_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_vol_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(_vol_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(_vol_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_TEXT), LV_PART_KNOB);

    _vol_lbl = lv_label_create(_scr);
    lv_obj_set_style_text_font(_vol_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_vol_lbl, lv_color_hex(C_TEXT2), 0);   // 与进度条时间同色
    lv_label_set_text(_vol_lbl, "--%");
    lv_obj_align(_vol_lbl, LV_ALIGN_TOP_LEFT, 648, 355);

    // ── 控制条(上一曲/播放暂停/下一曲;居中于左侧文字区) ──
    lv_obj_t* prev = mk_button(_scr, 90, 90, _prevClick_cb, this);
    lv_obj_align(prev, LV_ALIGN_LEFT_MID, 201, 136);
    lv_obj_t* pl = lv_label_create(prev);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(pl, LV_SYMBOL_PREV);
    lv_obj_center(pl);

    lv_obj_t* play = mk_button(_scr, 120, 120, _playClick_cb, this);
    lv_obj_align(play, LV_ALIGN_LEFT_MID, 320, 136);   // 中轴=进度条中心 380
    lv_obj_set_style_bg_color(play, lv_color_hex(C_ON), 0);
    _play_lbl = lv_label_create(play);
    lv_obj_set_style_text_font(_play_lbl, &lv_font_montserrat_42, 0);   // ESP 无 48
    lv_obj_set_style_text_color(_play_lbl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(_play_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(_play_lbl);

    lv_obj_t* next = mk_button(_scr, 90, 90, _nextClick_cb, this);
    lv_obj_align(next, LV_ALIGN_LEFT_MID, 469, 136);
    lv_obj_t* nl = lv_label_create(next);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(nl, LV_SYMBOL_NEXT);
    lv_obj_center(nl);

    // ── 歌词单行: 仅当前行(亮), 随播放位置滚动 ──
    _lyr_now = lv_label_create(_scr);
    lv_obj_set_style_text_font(_lyr_now, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_lyr_now, lv_color_hex(C_TEXT), 0);
    lv_obj_set_width(_lyr_now, 1000);
    lv_obj_set_style_text_align(_lyr_now, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(_lyr_now, LV_LABEL_LONG_DOT);
    lv_label_set_text(_lyr_now, "");
    lv_obj_align(_lyr_now, LV_ALIGN_TOP_MID, 0, 622);   // 封面底(560)与屏幕底(720)的中间

    // 底部提示
    _hint_lbl = lv_label_create(_scr);   // 仅触摸后 3s 内显示(onRunning 驱动)
    lv_obj_set_style_text_font(_hint_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_hint_lbl, lv_color_hex(C_TEXT2), 0);
    lv_label_set_text(_hint_lbl, "点顶部标题切换 SONOS/HomePod · 上拨退出");
    lv_obj_align(_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);

    _switchPage(_page);
    // 手势 alive 旗标重建 (app_stocks 同款, 2026-10-29): onClose 置 false 后
    // 不重建则第二次进入上拨手势静默失效 (收音机实锤的同款 bug)
    _alive = std::make_shared<bool>(true);
    _installSwipeGesture();
    lv_screen_load_anim(_scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ─── 数据 ──────────────────────────────────────────────────────────────────

void AppPlayer::_pollEntity()
{
    if (!_ha) return;
    const char* entity = _pages[_page].entity;
    std::string st = _ha->getEntityState(entity);
    if (st.empty()) {
        if (_has_data) { _has_data = false; _refreshUi(); }
        return;
    }
    std::string t = _ha->getEntityAttr(entity, "media_title");
    std::string a = _ha->getEntityAttr(entity, "media_artist");
    std::string v = _ha->getEntityAttr(entity, "volume_level");
    std::string pic = _ha->getEntityAttr(entity, "entity_picture");
    std::string durs = _ha->getEntityAttr(entity, "media_duration");
    std::string poss = _ha->getEntityAttr(entity, "media_position");
    int new_dur = durs.empty() ? -1 : (int)(atof(durs.c_str()) + 0.5);
    // 时间戳漂移校正: HA 的 position 是 updated_at 时刻的值(静止不动),
    // 真实位置 = position + (now - updated_at), 否则必然周期性误重置
    int new_pos = poss.empty() ? -1 : (int)(atof(poss.c_str()) + 0.5);
    if (new_pos >= 0) {
        std::string upd = _ha->getEntityAttr(entity, "media_position_updated_at");
        int64_t ue = upd.empty() ? -1 : iso8601_utc_epoch(upd);
        int64_t ne = (int64_t)time(nullptr);
        if (ue > 0 && ne >= ue) new_pos += (int)(ne - ue);
    }
    bool track_chg = (new_dur > 0 && _dur > 0 && new_dur != _dur);
    bool pos_resync = false;
    if (new_dur > 0 && new_pos >= 0) {
        if (new_pos > new_dur) new_pos = new_dur;
        // 采纳: 首次/换曲/与本地差≥2s(真seek或停走); ±1s 是取整噪声
        if (_pos < 0 || track_chg || new_pos > _pos + 1 || new_pos < _pos - 1) {
            _pos = new_pos; _dur = new_dur;
            _pos_ms = GetHAL()->millis();
            pos_resync = true;
        }
    } else {                                    // 无时长(电台流/idle): 收起
        _pos = -1; _dur = -1;
    }
    bool pos_jump = track_chg || pos_resync;
    int vol = v.empty() ? -1 : (int)(atof(v.c_str()) * 100 + 0.5f);
    bool changed = !_has_data || t != _title || a != _artist || st != _state || vol != _volume;
    bool track_new = t != _title || a != _artist;
    _title = t; _artist = a; _state = st; _volume = vol; _has_data = true;
    if (track_new && !t.empty())
        _startLyricsFetch(t, a, new_dur > 0 ? new_dur : 0);
    if (changed || pos_jump) _refreshUi();
    // 封面:entity_picture 变化(切歌/切页)才重拉; 失败后 10s 退避重试
    // (旧逻辑一次性: 失败即永不重试 —— 尺寸不匹配时永远黑屏, 2026-10-26 实锤)
    if (pic != _art_key && GetHAL()->millis() >= _art_retry_ms) {
        _art_key = pic;
        if (!pic.empty()) _startArtFetch();
    }
    // 拉取完成 → LVGL 线程上屏
    if (_art_ready.exchange(false) && _cover_dsc && _cover_img) {
        lv_image_set_src(_cover_img, _cover_dsc);
        lv_obj_clear_flag(_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_cover_card, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppPlayer::_sendCmd(const char* service, const char* extra_json)
{
    if (!_ha) return;
    _ha->callService("media_player", service, _pages[_page].entity,
                     extra_json ? extra_json : "");
}

// ─── UI 刷新 ───────────────────────────────────────────────────────────────

void AppPlayer::_switchPage(int idx)
{
    if (idx < 0 || idx >= kPageCount) return;
    _page = idx;
    if (_page_lbl) lv_label_set_text(_page_lbl, _pages[idx].tab);
    _title.clear(); _artist.clear(); _state.clear();
    _volume = -1; _has_data = false;
    // 封面复位:暗卡垫底,键清空(新页轮询后按需重拉)
    if (_cover_img)  lv_obj_add_flag(_cover_img, LV_OBJ_FLAG_HIDDEN);
    if (_cover_card) lv_obj_clear_flag(_cover_card, LV_OBJ_FLAG_HIDDEN);
    _art_key.clear();
    _refreshUi();
    if (_ha) _ha->requestRefresh();
}

void AppPlayer::_refreshUi()
{
    // 状态 chip
    const char* chip = "离线";
    if (_has_data) {
        if (_state == "playing")       chip = "播放中";
        else if (_state == "paused")   chip = "已暂停";
        else if (_state == "idle")     chip = "空闲";
        else if (_state == "off")      chip = "关闭";
        else                           chip = _state.c_str();
    } else {
        chip = "连接中…";
    }
    if (_state_chip) lv_label_set_text(_state_chip, chip);   // chip 已取消, 保险判空

    // now-playing
    lv_label_set_text(_title_lbl, _has_data && !_title.empty() ? _title.c_str()
                            : "（无播放信息）");
    lv_label_set_text(_artist_lbl, _artist.c_str());

    // 播放键
    lv_label_set_text(_play_lbl, _state == "playing" ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    // 进度条(有时长才显示)
    if (_prog_slider) {
        if (_dur > 0 && _pos >= 0) {
            lv_obj_clear_flag(_prog_slider, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_prog_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(_prog_total, LV_OBJ_FLAG_HIDDEN);
            _prog_sync = true;
            lv_slider_set_range(_prog_slider, 0, _dur);
            lv_slider_set_value(_prog_slider, _pos, LV_ANIM_OFF);
            _prog_sync = false;
            lv_label_set_text_fmt(_prog_lbl, "%d:%02d", _pos / 60, _pos % 60);
            lv_label_set_text_fmt(_prog_total, "%d:%02d", _dur / 60, _dur % 60);
        } else {
            lv_obj_add_flag(_prog_slider, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_prog_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(_prog_total, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 音量(程序置值时屏蔽回调)
    if (_vol_slider) {
        _slider_sync = true;
        if (_volume >= 0) {
            lv_slider_set_value(_vol_slider, _volume, LV_ANIM_OFF);
            lv_label_set_text_fmt(_vol_lbl, "%d%%", _volume);
        } else {
            lv_label_set_text(_vol_lbl, "--%");
        }
        _slider_sync = false;
    }
}

// ─── 事件 ──────────────────────────────────────────────────────────────────

void AppPlayer::_tabClick_cb(lv_event_t* e)
{
    // 点顶部标题 = 切到另一页(两页互切)
    auto* self = static_cast<AppPlayer*>(lv_event_get_user_data(e));
    self->_switchPage(1 - self->_page);
}

void AppPlayer::_prevClick_cb(lv_event_t* e)
{
    static_cast<AppPlayer*>(lv_event_get_user_data(e))->_sendCmd("media_previous_track");
}

void AppPlayer::_nextClick_cb(lv_event_t* e)
{
    static_cast<AppPlayer*>(lv_event_get_user_data(e))->_sendCmd("media_next_track");
}

void AppPlayer::_playClick_cb(lv_event_t* e)
{
    auto* self = static_cast<AppPlayer*>(lv_event_get_user_data(e));
    self->_sendCmd("media_play_pause");
    // 乐观更新: 4s 轮询会校正
    if (self->_state == "playing") self->_state = "paused";
    else                           self->_state = "playing";
    self->_refreshUi();
}

// ── 歌词: 后台拉取 OrangePi 8769 的 LRCLIB 代理, 解析 LRC 时间轴 ──
// 切歌时先清空显示(UI 线程调用), 记录最新期望; 若上一首还在拉取中, 由该在途
// 任务收尾时捡起新期望重拉(最多 4 轮)——直接丢弃新请求会让旧词"定住"。
void AppPlayer::_startLyricsFetch(const std::string& title, const std::string& artist, int duration)
{
    {
        std::lock_guard<std::mutex> lk(_lyr_mu);
        _lyr_want_title = title; _lyr_want_artist = artist; _lyr_want_dur = duration;
        _lyr_t.clear(); _lyr_l.clear();      // 立即丢掉旧词, 避免"定住"
        _lyr_idx = -1;
    }
    if (_lyr_now) lv_label_set_text(_lyr_now, "");
    if (_lyr_fetching.exchange(true)) return;   // 在途任务会捡起新期望
    GetHAL()->tryRunDetached([this]() {
        for (int attempt = 0; attempt < 4; attempt++) {
            std::string title, artist;
            int dur;
            {
                std::lock_guard<std::mutex> lk(_lyr_mu);
                if (_lyr_want_title.empty()) break;
                title = _lyr_want_title; artist = _lyr_want_artist; dur = _lyr_want_dur;
            }
            std::string url = "http://192.168.1.200:8769/lyrics?title=" + url_encode(title)
                            + "&artist=" + url_encode(artist);
            if (dur > 0) url += "&duration=" + std::to_string(dur);
            auto resp = GetHAL()->httpGet(url);
            std::vector<std::pair<float, std::string>> ent;   // (时间, 行)
            if (resp.ok) {
                try {
                    auto j = nlohmann::json::parse(resp.body);
                    if (j.value("ok", false)) {
                        std::string lrc = j.value("synced", std::string());
                        size_t pos = 0;
                        while (pos != std::string::npos && ent.size() < 400) {
                            size_t eol = lrc.find('\n', pos);
                            std::string line = lrc.substr(pos, (eol == std::string::npos ? lrc.size() : eol) - pos);
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                            size_t p = 0;
                            float first_t = -1;
                            std::vector<float> extra_t;
                            while (p + 1 < line.size() && line[p] == '[') {
                                size_t cl = line.find(']', p);
                                if (cl == std::string::npos) break;
                                int mm = 0, ss = 0; float ff = 0;
                                if (sscanf(line.c_str() + p + 1, "%d:%d%f", &mm, &ss, &ff) >= 2) {
                                    float t = mm * 60 + ss + (ff > 0 && ff < 1 ? ff : 0);
                                    if (first_t < 0) first_t = t; else extra_t.push_back(t);
                                    p = cl + 1;
                                } else break;
                            }
                            std::string text = line.substr(p < line.size() ? p : line.size());
                            if (first_t >= 0) {
                                ent.push_back({first_t, text});
                                for (float et : extra_t) ent.push_back({et, text});
                            }
                            pos = (eol == std::string::npos) ? std::string::npos : eol + 1;   // +2 会吃掉下一行行首的 [
                        }
                    }
                } catch (...) {}
            }
            // 多时间戳行(副歌重复)展开后乱序 —— 必须按时间稳定排序,
            // 否则滚动扫描在第一个乱序点卡住("只显示第一句"实锤)
            std::stable_sort(ent.begin(), ent.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<float> ts;
            std::vector<std::string> ls;
            ts.reserve(ent.size());
            ls.reserve(ent.size());
            for (auto& e : ent) { ts.push_back(e.first); ls.push_back(e.second); }
            bool stable = false;
            {
                std::lock_guard<std::mutex> lk(_lyr_mu);
                if (_lyr_want_title == title && _lyr_want_artist == artist) {
                    _lyr_t.swap(ts);
                    _lyr_l.swap(ls);
                    _lyr_idx = -1;
                    _lyr_key = artist + "|" + title;
                    stable = true;             // 期望没再变: 完成
                }
            }
            if (stable) break;
            // 期望又变了(继续切歌): 回圈拉最新一首
        }
        _lyr_fetching.store(false);
        // 注: store(false) 前几微秒若 UI 恰好请求新歌(exchange 失败被拒),
        // 该首会暂时无词(显示已清空, 不会"定住"), 下次切歌即恢复 —— 概率极小,
        // 用 4 轮重拉循环覆盖了其余全部切歌窗口。
    });
}

// 按播放位置刷新歌词行(当前行亮 + 下一行暗)
void AppPlayer::_updateLyricLine()
{
    if (!_lyr_now) return;
    std::lock_guard<std::mutex> lk(_lyr_mu);
    if (_lyr_t.empty()) {
        if (_lyr_idx != -1) {
            _lyr_idx = -1;
            lv_label_set_text(_lyr_now, "");
        }
        return;
    }
    if (_pos < 0) return;
    int idx = 0;
    while (idx + 1 < (int)_lyr_t.size() && _lyr_t[idx + 1] <= (float)_pos) idx++;
    if (idx == _lyr_idx) return;
    _lyr_idx = idx;
    lv_label_set_text(_lyr_now, _lyr_l[idx].c_str());
}

// 播放中本地走秒(轮询 4s 校正一次; UI 只在值变化时更新, 无重绘负担)
void AppPlayer::_advancePos()
{
    if (_dur <= 0 || _pos < 0 || _state != "playing") return;
    uint32_t now = GetHAL()->millis();
    int p = _pos + (int)((now - _pos_ms) / 1000);
    if (p >= _dur) p = _dur;      // 到头停住, 轮询/换曲会校正
    if (p != _pos) {
        _pos = p; _pos_ms = now;
        if (_prog_slider && !lv_obj_has_flag(_prog_slider, LV_OBJ_FLAG_HIDDEN)) {
            _prog_sync = true;
            lv_slider_set_value(_prog_slider, _pos, LV_ANIM_OFF);
            _prog_sync = false;
            lv_label_set_text_fmt(_prog_lbl, "%d:%02d", _pos / 60, _pos % 60);
        }
    }
}

void AppPlayer::_prog_cb(lv_event_t* e)
{
    auto* self = static_cast<AppPlayer*>(lv_event_get_user_data(e));
    if (self->_prog_sync || self->_dur <= 0) return;
    int sec = lv_slider_get_value(self->_prog_slider);
    char extra[48];
    snprintf(extra, sizeof(extra), "\"seek_position\":%d", sec);
    self->_sendCmd("media_seek", extra);
    // 乐观更新: 轮询会校正
    self->_pos = sec; self->_pos_ms = GetHAL()->millis();
    lv_label_set_text_fmt(self->_prog_lbl, "%d:%02d/%d:%02d",
                          sec / 60, sec % 60, self->_dur / 60, self->_dur % 60);
}

void AppPlayer::_vol_cb(lv_event_t* e)
{
    auto* self = static_cast<AppPlayer*>(lv_event_get_user_data(e));
    if (self->_slider_sync) return;   // 程序置值不回发
    int v = lv_slider_get_value(self->_vol_slider);
    lv_label_set_text_fmt(self->_vol_lbl, "%d%%", v);
    char extra[48];
    snprintf(extra, sizeof(extra), "\"volume_level\":%d.%02d", v / 100, v % 100);
    self->_sendCmd("volume_set", extra);
}

// ─── 生命周期 ──────────────────────────────────────────────────────────────

void AppPlayer::onRunning()
{
    // 底部提示: 触摸后 3s 内显示, 平时隐藏
    if (_hint_lbl) {
        bool touched = lv_display_get_inactive_time(nullptr) < 3000;
        if (touched == lv_obj_has_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN)) {
            if (touched) lv_obj_clear_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);
            else         lv_obj_add_flag(_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static uint32_t last_poll = 0;
    uint32_t now = GetHAL()->millis();
    if (now - last_poll >= 1000) {   // 1s: 本地走秒 + 查缓存(HaClient 4s 拉一次)
        last_poll = now;
        _advancePos();
        _updateLyricLine();
        _pollEntity();
    }
}

void AppPlayer::onClose()
{
    mclog::tagInfo(TAG, "on close");
    // 前台独占解除(2026-10-31): 离开播放器界面恢复常规屏保语义
    if (_block) _block(false);
    // 先回主屏再删对象 —— 删除 active screen 会让 LVGL 悬空(app_radio/AppHA 同序)
    if (_close_cb) _close_cb();
    *_alive = false;
    _removeSwipeGesture();
    if (_ha) { _ha->destroy(); _ha.reset(); }
    // 封面缓冲释放;若此刻有拉取在途,其自持局部指针,失败自清/成功时
    // _closing 后无人消费,200KB PSRAM 级泄漏可忽略(Tab5 PSRAM 32MB)
#ifndef PLATFORM_BUILD_DESKTOP
    if (_cover_buf) {
        heap_caps_free(_cover_buf);
        _cover_buf = nullptr;
        _cover_dsc = nullptr;
    }
#endif
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
}

// ─── 封面拉取(detached 线程;PSRAM 直读,零解码) ──────────────────────────

#ifndef PLATFORM_BUILD_DESKTOP

void AppPlayer::_startArtFetch()
{
    if (_art_fetching.exchange(true)) return;   // 上一张还在拉
    // tryRunDetached(8KB 栈, 失败返回 false)——std::thread 默认 16KB 内部栈,
    // 低内存时创建抛 system_error, 未捕获即 abort 重启(2026-10-26 实锤)
    if (!GetHAL()->tryRunDetached([this]() { _artFetchTask(this); })) {
        _art_fetching.store(false);   // 拉取失败: 允许下次重试
    }
}

void AppPlayer::_artFetchTask(AppPlayer* self)
{
    // 缓冲布局:[lv_image_dsc_t][320*320*2 像素],PSRAM 一体分配
    constexpr int kPixels = _COVER * _COVER * 2;
    void* buf = heap_caps_malloc(sizeof(lv_image_dsc_t) + kPixels, MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
        self->_art_fetching.store(false);
        return;
    }
    auto* dsc = static_cast<lv_image_dsc_t*>(buf);
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = _COVER;
    dsc->header.h = _COVER;
    dsc->header.stride = _COVER * 2;
    dsc->data_size = kPixels;
    dsc->data = static_cast<const uint8_t*>(buf) + sizeof(lv_image_dsc_t);

    const char* player = self->_page == 0 ? "sonos" : "pod";
    char url[128];
    snprintf(url, sizeof(url),
             "http://192.168.1.200:8766/assets/cover.rgb565?player=%s&size=460", player);

    bool ok = false;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http) {
        if (esp_http_client_open(http, 0) == ESP_OK) {
            esp_http_client_fetch_headers(http);
            if (esp_http_client_get_status_code(http) == 200) {
                char* dst = const_cast<char*>(reinterpret_cast<const char*>(dsc->data));
                int len = 0;
                int n;
                while ((n = esp_http_client_read(http, dst + len, kPixels - len)) > 0) {
                    len += n;
                    if (len >= kPixels) break;
                    // 大图节流: 每 32KB 让 2ms —— esp_hosted 的 SDIO RX 缓冲池
                    // 需要喘息, 一次灌 800KB 触发 sdio_rx_get_buffer 断言重启
                    // (2026-10-26 640px 封面实锤)
                    if ((len & 0x7FFF) < n) vTaskDelay(pdMS_TO_TICKS(2));
                }
                ok = (len == kPixels);
            }
        }
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    if (!ok) {
        heap_caps_free(buf);
        self->_art_fetching.store(false);
        self->_art_retry_ms = GetHAL()->millis() + 10000;   // 10s 后重试
        self->_art_key.clear();   // 关键: 清键让下轮轮询重新触发(否则 pic==key 永不重试)
        return;
    }
    // 发布:换下旧缓冲,置就绪标志(LVGL 线程在 _pollEntity 消费)
    if (self->_cover_buf) heap_caps_free(self->_cover_buf);
    self->_cover_buf = buf;
    self->_cover_dsc = dsc;
    self->_art_ready.store(true);
    self->_art_fetching.store(false);
}

#else   // 桌面构建:无封面(模拟器不接 hermes)

void AppPlayer::_startArtFetch() {}
void AppPlayer::_artFetchTask(AppPlayer*) {}

#endif

// ─── 上拨手势(app_radio 同款) ─────────────────────────────────────────────

void AppPlayer::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            static lv_point_t s_gpress{0, 0};   // PRESSED 起点 (底部起手判定)
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                const lv_event_code_t gcode = lv_event_get_code(e);
                lv_indev_t* gdev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (gcode == LV_EVENT_PRESSED) {
                    lv_point_t gp; lv_indev_get_point(gdev, &gp);
                    s_gpress = gp;
                    return;
                }
                if (gcode != LV_EVENT_GESTURE) return;
                lv_dir_t dir = lv_indev_get_gesture_dir(gdev);
                if (dir == LV_DIR_TOP) {
                    // 双守卫: 只在本 app 前台 + 屏幕最下缘 (y>650) 起手
                    if (!ctx->app->_scr || lv_screen_active() != ctx->app->_scr) return;
                    if (s_gpress.y <= 650) return;
                    auto* defer = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<SwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a) c->app->_requestClose();
                        delete c;
                    }, defer);
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppPlayer::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // _gesture_ctx 故意不释放(app_radio 同款: 移除后回调不再触达,16B 可忽略)
}

void AppPlayer::_requestClose() { close(); }
