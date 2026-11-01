#include "app_idle_screen.h"
#include "../app_home/app_home.h"
#include "../app_voice_input/app_voice_input.h"
#include "../utils/datetime/lunar_calendar.h"
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifndef PLATFORM_BUILD_DESKTOP
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

// Full-coverage Chinese font (same blob already linked in for the HA/Claude
// screens) — GB2312 common ~3500 glyphs, no tofu boxes. On device it's a
// cbin blob used in-place from flash; on the desktop sim, the linked 20px
// C-array font.
#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
static const lv_font_t* zh_font_lg()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
#endif

static constexpr int SCREEN_W = 1280;
static constexpr int SCREEN_H = 720;
static constexpr uint32_t C_BG   = 0x000000;
static constexpr uint32_t C_TEXT = 0xEAF3FB;
static constexpr uint32_t C_DIM  = 0x8FA8C4;

// Group box the clock/date/weather move around inside — kept well clear of
// the screen edges (burn-in guard: OLED/LCD panels showing the same static
// bright text in the same spot for hours can leave a faint ghost image).
static constexpr int GROUP_W = 600;
static constexpr int GROUP_H = 212;   // 时钟+日期+天气+提醒/行程 四行 (2026-11-01 加行4)
static constexpr int MARGIN  = 60;
static const struct { int x, y; } POSITIONS[] = {
    {(SCREEN_W - GROUP_W) / 2, (SCREEN_H - GROUP_H) / 2},  // center
    {MARGIN,                    MARGIN},                     // top-left
    {SCREEN_W - GROUP_W - MARGIN, MARGIN},                    // top-right
    {MARGIN,                    SCREEN_H - GROUP_H - MARGIN}, // bottom-left
    {SCREEN_W - GROUP_W - MARGIN, SCREEN_H - GROUP_H - MARGIN}, // bottom-right
};
static constexpr int POSITION_COUNT = sizeof(POSITIONS) / sizeof(POSITIONS[0]);

// ── 照片屏助助手 ─────────────────────────────────────────────────────────
// 缓冲布局 [lv_image_dsc_t][像素] (app_player 封面同款); 服务端 PIL 已把照片
// 转成 1280x720 RGB565 裸字节, 设备零解码直显。
static constexpr int kPhW = 1280, kPhH = 720;
static constexpr int kPhBytes = kPhW * kPhH * 2;

static void ph_buf_free(void* b)
{
    if (!b) return;
#ifndef PLATFORM_BUILD_DESKTOP
    heap_caps_free(b);
#else
    free(b);
#endif
}

static void* ph_buf_alloc()
{
#ifndef PLATFORM_BUILD_DESKTOP
    void* b = heap_caps_malloc(sizeof(lv_image_dsc_t) + kPhBytes,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void* b = malloc(sizeof(lv_image_dsc_t) + kPhBytes);
#endif
    if (!b) return nullptr;
    auto* dsc = static_cast<lv_image_dsc_t*>(b);
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = kPhW;
    dsc->header.h = kPhH;
    dsc->header.stride = kPhW * 2;
    dsc->data_size = kPhBytes;
    dsc->data = static_cast<const uint8_t*>(b) + sizeof(lv_image_dsc_t);
    return b;
}

// 下载第 idx 张照片 → 已填好 dsc 的缓冲; 失败返回 nullptr。
// 设备: esp_http_client 直读 + 32KB/2ms 节流 (SDIO RX 缓冲需喘息, app_player
// 640px 封面实锤); 桌面: hal httpGet 一步到位。
static void* ph_download(int idx)
{
    std::string url = "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200")
                    + ":8766/screensaver/photo.rgb565?i=" + std::to_string(idx);
    void* buf = ph_buf_alloc();
    if (!buf) return nullptr;
    auto* dsc = static_cast<lv_image_dsc_t*>(buf);
    char* dst = const_cast<char*>(reinterpret_cast<const char*>(dsc->data));

#ifdef PLATFORM_BUILD_DESKTOP
    auto resp = GetHAL()->httpGet(url);
    if (resp.ok && (int)resp.body.size() == kPhBytes) {
        memcpy(dst, resp.body.data(), kPhBytes);
        return buf;
    }
    ph_buf_free(buf);
    return nullptr;
#else
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 15000;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) { ph_buf_free(buf); return nullptr; }
    bool ok = false;
    if (esp_http_client_open(http, 0) == ESP_OK) {
        esp_http_client_fetch_headers(http);
        if (esp_http_client_get_status_code(http) == 200) {
            int len = 0, n;
            while ((n = esp_http_client_read(http, dst + len, kPhBytes - len)) > 0) {
                len += n;
                if (len >= kPhBytes) break;
                if ((len & 0x7FFF) < n) vTaskDelay(pdMS_TO_TICKS(2));
            }
            ok = (len == kPhBytes);
        }
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (!ok) { ph_buf_free(buf); return nullptr; }
    return buf;
#endif
}

void AppIdleScreen::onCreate() {}

void AppIdleScreen::onResume() {}

void AppIdleScreen::dismiss()
{
    lv_display_trigger_activity(NULL);
    _hide();
    _undim();
}

void AppIdleScreen::onPause()
{
    _hide();
    _undim();
}

void AppIdleScreen::onRunning()
{
    // 前台独占(2026-10-31): 收音机/播放器界面期间完全不进屏保 ——
    // 不弹照片/时钟遮盖也不熄屏, 界面保持可见
    if (_blocked) {
        if (_overlay) _hide();
        _undim();
        return;
    }

    // 播放抑制(收音机后台播放): 不弹时钟遮盖(会盖住别的 app), 但闲置熄屏
    // 仍然允许 —— 听歌熄屏更合理也更省电 (2026-10-28 语义调整)
    if (_suppressed) {
        if (_overlay) _hide();
        _refreshConfig();
        uint32_t idle_ms = lv_display_get_inactive_time(nullptr);
        if (idle_ms < _idle_timeout_ms) {
            _undim();
            return;
        }
        if (!_dimmed) _dim();
        return;
    }

    // Don't cover an in-progress voice-input session (recording / result overlay).
    if (AppVoiceInput::isMicActive()) {
        if (_overlay) _hide();
        _undim();
        return;
    }

    _refreshConfig();

    uint32_t idle_ms = lv_display_get_inactive_time(nullptr);

    // Any touch resets LVGL's inactivity timer for the whole display —
    // including a tap on the overlay itself. A small idle_ms here means
    // "something was touched since we showed it": hide the overlay / restore
    // brightness. The overlay's own click handler already hides it instantly;
    // this is just a safety net in case that somehow didn't fire.
    if (idle_ms < _idle_timeout_ms) {
        if (_overlay) _hide();
        _undim();
        return;
    }

    // 闲置超时: 勾选屏保 → 照片轮播(失败回退时钟); 不勾 → 熄屏
    if (_ss_enabled) {
        if (!_overlay) _show();
        else if (_clock_mode) _updateClock();
        else _photoTick();
    } else {
        if (!_dimmed) _dim();
    }
}

void AppIdleScreen::setSuppressed(bool s)
{
    _suppressed = s;
    if (s && _overlay) _hide();
    if (s) _undim();
}

void AppIdleScreen::setBlocked(bool b)
{
    if (_blocked == b) return;
    _blocked = b;
    if (b) {   // 进入收音机/播放器界面: 撤掉已弹的屏保并恢复亮度
        if (_overlay) _hide();
        _undim();
    } else {
        // 离开界面: 重置闲置计时, 从新窗口起算(防非触摸路径关闭时立即弹屏保)
        lv_display_trigger_activity(NULL);
    }
}

void AppIdleScreen::_refreshConfig()
{
    uint32_t now = GetHAL()->millis();
    if (_last_cfg_ms != 0 && now - _last_cfg_ms < CFG_MS) return;
    _last_cfg_ms = now;
    _ss_enabled  = GetHAL()->getConfig("screensaver_en", "1") != "0";
    int m = atoi(GetHAL()->getConfig("screensaver_min", "5").c_str());
    if (m != 2 && m != 5 && m != 10) m = 5;   // 只认设置弹窗里的三档
    _idle_timeout_ms = (uint32_t)m * 60 * 1000;
#ifdef PLATFORM_BUILD_DESKTOP
    // 桌面调试 (同 TAB5_THEME/TAB5_PAGE 模式): TAB5_IDLE_MS=毫秒 强制闲置超时,
    // TAB5_SS_EN=0 强制关屏保走降亮度分支, 免等 2 分钟
    if (const char* e = getenv("TAB5_IDLE_MS")) {
        if (atoi(e) > 0) _idle_timeout_ms = (uint32_t)atoi(e);
    }
    if (const char* e = getenv("TAB5_SS_EN")) _ss_enabled = atoi(e) != 0;
#endif
}

void AppIdleScreen::_dim()
{
    _pre_dim_brt = GetHAL()->getDisplayBrightness();
    _dimmed = true;
    // 2026-10-28 真机实测: 背光驱动在低占空比段有视觉下限, 5%(gamma 后
    // ~1.1% duty)依旧亮 — 闲置降亮直接熄屏(duty 0), 触摸恢复原亮度
    GetHAL()->setDisplayBrightness(0);
}

void AppIdleScreen::_undim()
{
    if (!_dimmed) return;
    _dimmed = false;
    GetHAL()->setDisplayBrightness(_pre_dim_brt);
}

void AppIdleScreen::_show()
{
    if (_overlay) return;

    _clock_mode = false;

    _overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_set_style_pad_all(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(_overlay);

    // 照片模式 (2026-10-29): 全屏 lv_image, 服务端已转好 1280x720 RGB565。
    _photo_img = lv_image_create(_overlay);
    lv_obj_set_pos(_photo_img, 0, 0);
    lv_obj_clear_flag(_photo_img, LV_OBJ_FLAG_CLICKABLE);   // 点击落到 overlay 关闭

    // Touch anywhere on the overlay dismisses it immediately (PRESSED, not
    // CLICKED, so it reacts on touch-down rather than waiting for release).
    lv_obj_add_event_cb(_overlay, [](lv_event_t* e) {
        static_cast<AppIdleScreen*>(lv_event_get_user_data(e))->_hide();
    }, LV_EVENT_PRESSED, this);

    // 右下角时间/日期叠加 (2026-10-29 用户要求): 半透明黑圆角底衬保证任何
    // 照片上都可读; 标签不可点, 触摸始终落到 overlay 关闭。
    // 有提醒/行程时 scrim 加高一行显事件 (2026-11-01), 初值按已知数据定,
    // 避免 _applyAgenda 首个 tick 又跳变一次。
    std::string ag0;
#ifdef PLATFORM_BUILD_DESKTOP
    // 桌面调试 (同 TAB5_IDLE_MS 模式): 注入假事件文本, 免等真有提醒
    if (const char* e = getenv("TAB5_AGENDA_TXT")) {
        std::lock_guard<std::mutex> lk(_agm);
        _ag_txt = e;
    }
#endif
    { std::lock_guard<std::mutex> lk(_agm); ag0 = _ag_txt; }
    _ph_scrim = lv_obj_create(_overlay);
    lv_obj_set_size(_ph_scrim, ag0.empty() ? 320 : 480, ag0.empty() ? 96 : 140);
    lv_obj_align(_ph_scrim, LV_ALIGN_BOTTOM_RIGHT, -28, -28);
    lv_obj_set_style_bg_color(_ph_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_ph_scrim, LV_OPA_40, 0);
    lv_obj_set_style_radius(_ph_scrim, 18, 0);
    lv_obj_set_style_border_width(_ph_scrim, 0, 0);
    lv_obj_set_style_pad_all(_ph_scrim, 0, 0);
    lv_obj_clear_flag(_ph_scrim, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // 两行布局 (2026-10-29 用户定稿): 行1=日期+时间, 行2=星期+农历
    // 全部 30px 统一字号 (2026-10-29 用户要求): 行1="10月29日 17:43"
    // (单标签, zh_font_lg 含数字), 行2="周三 农历xx"
    _ph_time_lbl = lv_label_create(_ph_scrim);
    lv_obj_set_style_text_font(_ph_time_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_ph_time_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(_ph_time_lbl, LV_ALIGN_TOP_RIGHT, -24, 12);
    lv_obj_clear_flag(_ph_time_lbl, LV_OBJ_FLAG_CLICKABLE);

    _ph_week_lbl = lv_label_create(_ph_scrim);
    lv_obj_set_style_text_font(_ph_week_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_ph_week_lbl, lv_color_hex(0xE8F0F6), 0);
    lv_obj_align(_ph_week_lbl, LV_ALIGN_TOP_RIGHT, -24, 54);
    lv_obj_clear_flag(_ph_week_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 行3 (2026-11-01): 下一条提醒/行程, 农历下右对齐; 固定宽 + LONG_DOT
    // 超长自动省略 (事件标题理论可任意长), 无事件时保持空文本=不可见。
    _ph_ag_lbl = lv_label_create(_ph_scrim);
    lv_obj_set_style_text_font(_ph_ag_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_ph_ag_lbl, lv_color_hex(0xC6D6E6), 0);
    lv_obj_set_width(_ph_ag_lbl, 432);
    lv_label_set_long_mode(_ph_ag_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(_ph_ag_lbl, LV_ALIGN_TOP_RIGHT, -24, 96);
    lv_obj_clear_flag(_ph_ag_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(_ph_ag_lbl, ag0.c_str());

    _ph_clock_ms = 0;
    _updatePhotoClock();

    // 本会话起始序号: NVS 持久推进, 跨会话接着往下看
    _pidx = atoi(GetHAL()->getConfig("ss_photo_idx", "0").c_str()) + 1;
    GetHAL()->setConfig("ss_photo_idx", std::to_string(_pidx));
    _last_photo_ms = GetHAL()->millis();
    _photo_retry_ms = 0;
    _photo_fails = 0;
    _pb_fail.store(false);
    _fetchPhotoAsync(_pidx);

    // 弹出即拉一次提醒/行程, 之后每 AGENDA_MS 刷新 (tick 里续拉)
    _ag_fetch_ms = GetHAL()->millis();
    _fetchAgenda();

#ifdef PLATFORM_BUILD_DESKTOP
    // 桌面调试: 强制时钟回退模式 (免断照片服务才能调时钟 UI)
    if (getenv("TAB5_SS_CLOCK")) {
        _clock_mode = true;
        _buildClockContent();
        return;
    }
#endif
}

// ── 提醒/行程 (hermes /agenda, 同 app_home 数据源) ──────────────────────
// worker 只写 _ag_txt; LVGL 线程在 1s 时钟 tick 里 _applyAgenda 消费。
// 失败保持旧值 (屏保是低频展示面, 下个周期自然重试)。
void AppIdleScreen::_fetchAgenda()
{
    if (_ag_fetching.exchange(true)) return;
    std::string base = "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
    if (!GetHAL()->tryRunDetached([this, base]() {
            bool publish = false;
            std::string txt;
            try {
                auto j = nlohmann::json::parse(GetHAL()->httpGet(base + "/agenda").body);
                if (j.value("ok", false)) {   // 明确成功: 无 next=真无事项
                    txt = (j.contains("next") && j["next"].is_object())
                          ? j["next"].value("text", std::string())
                          : std::string();
                    publish = true;
                }
            } catch (...) {}                  // 网络/解析失败: 保持旧值
            if (publish) {
                std::lock_guard<std::mutex> lk(_agm);
                _ag_txt = txt;
            }
            _ag_fetching.store(false);
        })) {
        _ag_fetching.store(false);
    }
}

// LVGL 线程: 文本有变才重绘 (label 新建时初值非空也能对上); 照片 scrim
// 尺寸随有无事件在 两行 320x96 ↔ 三行 480x140 间切换。
void AppIdleScreen::_applyAgenda()
{
    std::string txt;
    { std::lock_guard<std::mutex> lk(_agm); txt = _ag_txt; }
    if (_ph_ag_lbl && _ph_scrim) {
        const char* cur = lv_label_get_text(_ph_ag_lbl);
        if (txt != (cur ? cur : "")) {
            lv_label_set_text(_ph_ag_lbl, txt.c_str());
            lv_obj_set_size(_ph_scrim, txt.empty() ? 320 : 480,
                            txt.empty() ? 96 : 140);
        }
    }
    if (_ag_lbl) {
        const char* cur = lv_label_get_text(_ag_lbl);
        if (txt != (cur ? cur : "")) lv_label_set_text(_ag_lbl, txt.c_str());
    }
}

// ── 照片轮播 ─────────────────────────────────────────────────────────────
void AppIdleScreen::_fetchPhotoAsync(int idx)
{
    if (_fetching.exchange(true)) return;   // 上一张还在拉
    uint32_t gen = _ss_gen.load();
    if (!GetHAL()->tryRunDetached([this, idx, gen]() {
            void* buf = ph_download(idx);
            std::lock_guard<std::mutex> lk(_pm);
            if (_ss_gen.load() != gen || buf == nullptr) {
                ph_buf_free(buf);           // 会话已结束(隐藏)或下载失败
                _pb_fail.store(true);
            } else {
                if (_pb_next) ph_buf_free(_pb_next);   // 防御: 上一张未被消费
                _pb_next = buf;
                _pb_ready.store(true);
            }
            _fetching.store(false);
        })) {
        _fetching.store(false);
    }
}

void AppIdleScreen::_applyNextPhoto()
{
    if (!_pb_next || !_photo_img) return;
    void* buf = _pb_next;
    _pb_next = nullptr;
    lv_image_set_src(_photo_img, static_cast<lv_image_dsc_t*>(buf));
    lv_obj_invalidate(_photo_img);
    ph_buf_free(_pb_cur);                   // set_src 已换, 旧缓冲不再被引用
    _pb_cur = buf;
    _pidx++;                                // 消费成功才推进序号
    _last_photo_ms = GetHAL()->millis();
    _photo_fails = 0;
    _fetchPhotoAsync(_pidx);                // 立即预取下一张
}

// 照片页右下角时间/日期 (1s 节流; 与时钟模式同源数据)
void AppIdleScreen::_updatePhotoClock()
{
    uint32_t now = GetHAL()->millis();
    if (_ph_clock_ms != 0 && now - _ph_clock_ms < 1000) return;
    _ph_clock_ms = now;
    if (!_ph_time_lbl) return;

    _agendaTick(now);
    _applyAgenda();

    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);
    static const char* const wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    // 行1: "10月29日 17:43" (单标签统一 30px)
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%d月%d日 %02d:%02d",
             lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    lv_label_set_text(_ph_time_lbl, tbuf);

    // 行2: 星期 + 农历
    char wbuf[48];
    int n = snprintf(wbuf, sizeof(wbuf), "周%s", wd[lt.tm_wday]);
    char lbuf[24];
    if (n > 0 && lunar::format_lunar_date(lt.tm_year + 1900, lt.tm_mon + 1,
                                          lt.tm_mday, lbuf, sizeof(lbuf)))
        snprintf(wbuf + n, sizeof(wbuf) - n, " 农历%s", lbuf);
    lv_label_set_text(_ph_week_lbl, wbuf);
}

void AppIdleScreen::_photoTick()
{
    _updatePhotoClock();
    uint32_t now = GetHAL()->millis();

    // 已下载好的下一张: 首张立即应用, 其余到 60s 点位换图
    if (_pb_ready.load()) {
        bool first = (_pb_cur == nullptr);
        if (first || now - _last_photo_ms >= PHOTO_MS) {
            _pb_ready.store(false);
            _applyNextPhoto();
        }
        return;
    }

    // 失败: 首张连败两次 → 回退时钟模式; 已有照片在显则稍后重试下一张
    if (_pb_fail.exchange(false)) {
        _photo_fails++;
        if (_pb_cur == nullptr && _photo_fails >= 2) {
            _clock_mode = true;
            _buildClockContent();
            return;
        }
        _photo_retry_ms = now + (_pb_cur ? 15000 : 5000);
    }

    // 无下一张且不在拉取 → 到点/到重试时刻补拉 (重试同一序号)
    if (!_fetching.load() && _pb_next == nullptr) {
        bool want = (_pb_cur == nullptr)
                 || (now - _last_photo_ms >= PHOTO_MS)
                 || (_photo_retry_ms != 0 && now >= _photo_retry_ms);
        if (want) {
            _photo_retry_ms = 0;
            _fetchPhotoAsync(_pidx);
        }
    }
}

// 时钟内容(照片失败回退时构建; 即原屏保 UI —— 时间/农历日期/天气 + 防烧屏漂移)
void AppIdleScreen::_buildClockContent()
{
    if (_photo_img) {                       // 照片对象退场, 时钟接管 overlay
        lv_obj_delete(_photo_img);
        _photo_img = nullptr;
    }
    if (_ph_scrim) {                        // 右下角叠加一并退场
        lv_obj_delete(_ph_scrim);
        _ph_scrim = _ph_time_lbl = _ph_week_lbl = _ph_ag_lbl = nullptr;
    }
    _group = lv_obj_create(_overlay);
    lv_obj_set_size(_group, GROUP_W, GROUP_H);
    lv_obj_set_style_bg_opa(_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_group, 0, 0);
    lv_obj_set_style_pad_all(_group, 0, 0);
    lv_obj_clear_flag(_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_group, LV_OBJ_FLAG_CLICKABLE);  // clicks pass through to _overlay

    _time_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_time_lbl, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_time_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_align(_time_lbl, LV_ALIGN_TOP_MID, 0, 0);

    _date_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_date_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_date_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_date_lbl, LV_ALIGN_TOP_MID, 0, 70);

    _weather_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_weather_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_weather_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(_weather_lbl, LV_ALIGN_TOP_MID, 0, 116);

    // 行4 (2026-11-01): 下一条提醒/行程, 超长右缘截断 (LONG_DOT 需固定宽)
    _ag_lbl = lv_label_create(_group);
    lv_obj_set_style_text_font(_ag_lbl, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_ag_lbl, lv_color_hex(0xC6D6E6), 0);
    lv_obj_set_width(_ag_lbl, GROUP_W);
    lv_label_set_long_mode(_ag_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_ag_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_ag_lbl, LV_ALIGN_TOP_MID, 0, 162);
    lv_obj_clear_flag(_ag_lbl, LV_OBJ_FLAG_CLICKABLE);

    _pos_index = 0;
    lv_obj_set_pos(_group, POSITIONS[0].x, POSITIONS[0].y);
    _last_clock_update_ms = 0;  // force immediate first render
    _last_move_ms = GetHAL()->millis();
    _updateClock();
}

void AppIdleScreen::_hide()
{
    if (!_overlay) return;
    lv_obj_delete(_overlay);
    _overlay = _group = _time_lbl = _date_lbl = _weather_lbl = nullptr;
    _photo_img = nullptr;
    _ph_scrim = _ph_time_lbl = _ph_week_lbl = nullptr;
    _ph_ag_lbl = _ag_lbl = nullptr;
    // 照片缓冲与在途下载: 会话代递增让 worker 自弃, 已发布的就地释放
    {
        std::lock_guard<std::mutex> lk(_pm);
        _ss_gen++;
        ph_buf_free(_pb_cur);   _pb_cur  = nullptr;
        ph_buf_free(_pb_next);  _pb_next = nullptr;
        _pb_ready.store(false);
        _pb_fail.store(false);
    }
}

void AppIdleScreen::_reposition()
{
    _pos_index = (_pos_index + 1) % POSITION_COUNT;
    lv_obj_set_pos(_group, POSITIONS[_pos_index].x, POSITIONS[_pos_index].y);
}

void AppIdleScreen::_updateClock()
{
    uint32_t now = GetHAL()->millis();
    if (_last_clock_update_ms != 0 && now - _last_clock_update_ms < 1000) return;
    _last_clock_update_ms = now;

    _agendaTick(now);
    _applyAgenda();

    if (now - _last_move_ms >= MOVE_MS) {
        _last_move_ms = now;
        _reposition();
    }

    time_t t = time(nullptr);
    struct tm lt = {};
    localtime_r(&t, &lt);

    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(_time_lbl, tbuf);

    static const char* const wd[] = {"日", "一", "二", "三", "四", "五", "六"};
    char dbuf[64];
    int n = snprintf(dbuf, sizeof(dbuf), "%d月%d日 周%s", lt.tm_mon + 1, lt.tm_mday, wd[lt.tm_wday]);
    char lbuf[24];
    if (n > 0 && lunar::format_lunar_date(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lbuf, sizeof(lbuf)))
        snprintf(dbuf + n, sizeof(dbuf) - n, " 农历%s", lbuf);
    lv_label_set_text(_date_lbl, dbuf);

    std::string temp, cond;
    if (_home) _home->getWeather(temp, cond);
    if (!temp.empty() || !cond.empty()) {
        char wbuf[48];
        snprintf(wbuf, sizeof(wbuf), "%s  %s", cond.c_str(), temp.c_str());
        lv_label_set_text(_weather_lbl, wbuf);
    } else {
        lv_label_set_text(_weather_lbl, "");
    }
}
