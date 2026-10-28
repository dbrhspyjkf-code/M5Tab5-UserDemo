/*
 * app_radio — 网络收音机（参考 folo ai-passport 的 radio_app 移植到 Tab5）。
 *
 * 链路: OrangePi:8769 radio_relay.py (ffmpeg → 16kHz mono s16le PCM)
 *   → HTTP 流读 → PSRAM 环形缓冲(~1s) → 16k→24k 线性重采样 →
 *   xiaozhi_codec_write (esp_codec_dev) 输出, 零解码。
 * 注意: bsp codec 句柄在本机从未初始化(bsp_codec_init 被禁), 其 i2s 函数
 * 指针为 NULL —— 直接调用即空指针跳转重启(2026-10-26 崩溃实锤)。音频输出
 * 必须走 xiaozhi 的 AudioCodec(24kHz mono)。
 * 16bit PCM 任意字节读入需奇数字节拼对(Passport 实测教训)。Passport 上的
 * "播放期必须关 WiFi 省电"在 Tab5 不适用: WiFi 走 esp_hosted 远程协议,
 * esp_wifi_set_ps 在播放链路里疑似崩溃源(2026-10-26 "连接中"即重启)。
 */
#include "app_radio.h"
#include "../utils/theme/theme.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <xiaozhi_ctl.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#ifndef PLATFORM_BUILD_DESKTOP
#include <cbin_font.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../utils/assets/assets_partition.h"

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

static const char* TAG = "radio";

// 电台表(与 Passport 同源中继; freq=真实 FM 频点 0.1MHz, 0=纯网络台)
struct RadioStation {
    const char* name;
    const char* path;
    int freq;
};
static uint16_t s_vu_h[20] = {0};   // 当前条高(px), draw 回调与 _updateVu 共享

static const RadioStation kStations[] = {
    {"深圳交通广播",    "szjt",  1062},
    {"深圳新闻广播",    "szxw",  898},
    {"深圳飞扬971",     "szfy",  971},
    {"深圳生活广播",    "szsh",  942},
    {"央广中国之声",    "zgzs",  0},
    {"央广经济之声",    "jjzs",  0},
    {"广东音乐之声",    "gdyy",  993},
    {"羊城交通广播",    "gdyz",  1052},
    {"广东新闻FM91.4",  "gdxw",  914},
    {"珠江经济FM97.4",  "zjjs",  974},
    {"BBC World Service", "bbcws", 0},
    {"NPR News",        "npr",   0},
    {"KEXP 独立音乐",   "kexp",  0},
    {"Radio Paradise",  "rp",    0},
};
static const int kStationCount = sizeof(kStations) / sizeof(kStations[0]);
static const char* kRelayBase  = "http://192.168.1.200:8769/radio/";

// 配色: 主题快照(onOpen 时从全局主题取, 见 utils/theme/theme.h);
// 绘制回调在 LVGL 线程里读这些全局, 零锁。
static uint32_t C_BG, C_CARD, C_CARD_ON, C_ACCENT, C_TEXT, C_TEXT2,
                C_AMBER, C_AMBER_SOFT, C_DIAL_BG, C_BORDER, C_TRACK,
                C_OK, C_WARN, C_DANGER;

static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    C_BG = 0x000000;
    C_CARD = T.card[1];  C_CARD_ON = T.press;  C_ACCENT = T.accent;
    C_TEXT = T.text_hi;  C_TEXT2 = T.text_dim; C_AMBER = T.accent;
    C_AMBER_SOFT = T.text_dim;  C_DIAL_BG = T.track;
    C_BORDER = T.border;  C_TRACK = T.track;
    C_OK = T.ok;  C_WARN = T.warn;  C_DANGER = T.danger;
}

// ═══════════════════════════════════════════════════════════════════════
// 流引擎（仅真机; 常驻 detached 线程, 原子量通信, 绝不触碰 LVGL）
// ═══════════════════════════════════════════════════════════════════════
#ifndef PLATFORM_BUILD_DESKTOP

namespace {

struct TaskCtx {
    std::atomic<int>*  want;
    std::atomic<bool>* closing;
    std::atomic<int>*  state;
    std::atomic<int>*  lvl_b;   // 低/中/高频段能量 0..100
    std::atomic<int>*  lvl_m;
    std::atomic<int>*  lvl_t;
};

// 一阶 IIR 分频能量(16k 采样率): bass≈<230Hz, mid≈230-3k, treble=>3k。
// 逐块计算, 写入原子量供 UI 电平图读取。
void levels_update(const int16_t* p, int n, const TaskCtx& ctx)
{
    static int32_t lp1 = 0, lp2 = 0;
    int64_t e1 = 0, e2 = 0, e3 = 0;
    for (int i = 0; i < n; i++) {
        int32_t x = p[i];
        lp1 += (x - lp1) * 12 / 100;
        lp2 += (x - lp2) * 35 / 100;
        int32_t b = lp1;
        int32_t m = lp2 - lp1;
        int32_t t = x - lp2;
        e1 += (int64_t)b * b; e2 += (int64_t)m * m; e3 += (int64_t)t * t;
    }
    if (n <= 0) return;
    auto lvl = [n](int64_t e) {
        // RMS → 0..100 (满量程正弦≈193 封顶; 电台典型 25..70)
        double rms = sqrt((double)e / n);
        int v = (int)(rms / 35.0);   // 增益再加倍: 柱高翻倍的主要来源
        return v > 100 ? 100 : v;
    };
    ctx.lvl_b->store(lvl(e1));
    ctx.lvl_m->store(lvl(e2));
    ctx.lvl_t->store(lvl(e3));
}

constexpr int kRingSize = 131072;        // 128KB ≈ 4s @ 32KB/s — 后台播放时前台
                                         // app(股票页 canvas 重绘等)有 CPU 突发,
                                         // 加大缓冲防 underrun (2026-10-28)
// 卡顿诊断: EAGAIN 断流次数 / armed 后环空次数, 每 30s 打一次
static int s_diag_stalls = 0, s_diag_underruns = 0, s_diag_read = 0;
uint8_t* s_ring = nullptr;
int s_head = 0, s_tail = 0;
bool s_armed = false;

inline int ring_used()
{
    int u = s_head - s_tail;
    return u >= 0 ? u : u + kRingSize;
}

// 16k→24k 线性重采样(xiaozhi 输出固定 24kHz; 比率恰 2:3)。
// 相位状态跨块保持: 每输出步进 2/3 输入样本, 每 3 输出耗 2 输入。
struct Resampler {
    int32_t prev = 0;   // 上一输入样本
    int phase = 0;      // 下一输出点在 (prev, cur) 区间的 1/3 步位置
    int16_t out[3072];  // 输出缓冲(2048 输入样本最多产 3072)
    void process(const int16_t* in, int n, bool reset) {
        if (reset) { prev = 0; phase = 0; }
        int oc = 0;
        for (int i = 0; i < n; i++) {
            int32_t cur = in[i];
            while (phase < 3) {
                out[oc++] = (int16_t)(prev + (cur - prev) * phase / 3);
                if (oc == (int)(sizeof(out) / sizeof(out[0]))) {
                    xiaozhi_codec_write(out, oc);
                    oc = 0;
                }
                phase += 2;
            }
            phase -= 3;
            prev = cur;
        }
        if (oc > 0) xiaozhi_codec_write(out, oc);   // 阻塞式: DMA 满即背压
    }
};
static Resampler s_rs;

void ring_drain()
{
    if (!s_ring) return;
    if (!s_armed) {
        if (ring_used() < kRingSize / 4) return;   // 起播攒 1/4 环
        s_armed = true;
    }
    if (ring_used() == 0) { s_diag_underruns++; return; }   // 欠载: 环空
    static int16_t chunk[2048];                    // 恒取偶数样本
    while (ring_used() >= (int)sizeof(chunk)) {
        int contig = kRingSize - s_tail;
        if (contig > (int)sizeof(chunk)) contig = sizeof(chunk);
        memcpy(chunk, s_ring + s_tail, contig);
        s_tail = (s_tail + contig) % kRingSize;
        if (contig < (int)sizeof(chunk)) {
            memcpy((uint8_t*)chunk + contig, s_ring, sizeof(chunk) - contig);
            s_tail = sizeof(chunk) - contig;
        }
        s_rs.process(chunk, sizeof(chunk) / sizeof(chunk[0]), false);
    }
}

void ring_push(const uint8_t* p, int len)
{
    while (len > 0) {
        int free_b = kRingSize - 1 - ring_used();
        if (free_b == 0) {
            ring_drain();                          // 输出端阻塞=背压
            free_b = kRingSize - 1 - ring_used();
            if (free_b == 0) vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int take = len < free_b ? len : free_b;
        int contig = kRingSize - s_head;
        if (contig > take) contig = take;
        memcpy(s_ring + s_head, p, contig);
        s_head = (s_head + contig) % kRingSize;
        p += contig;
        len -= contig;
    }
}

// 任意字节数喂入, 奇数字节跨次拼对(Passport 踩过的强噪声坑)
void feed_pcm(const uint8_t* p, int n, uint8_t& odd_carry, bool& have_odd)
{
    int off = 0;
    if (have_odd && n >= 1) {
        uint8_t pair[2] = {odd_carry, p[0]};
        ring_push(pair, 2);
        have_odd = false;
        off = 1;
        p += 1;
    }
    int bytes = n - off;
    if (bytes & 1) {
        odd_carry = p[bytes - 1];
        have_odd = true;
        bytes -= 1;
    }
    if (bytes > 0) ring_push(p, bytes);
}

// 换台失败时 cur 置 -1: 用户重按同台也能触发重连
void teardown_stream(esp_http_client_handle_t& http)
{
    if (http) {
        esp_http_client_cleanup(http);
        http = nullptr;
    }
    s_head = s_tail = 0;
    s_armed = false;
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

void radio_task_fn(TaskCtx ctx)
{
    esp_http_client_handle_t http = nullptr;
    int cur = -2;
    static uint8_t inbuf[4096];
    uint8_t odd_carry = 0;
    bool have_odd = false;
    int stall = 0;

    int64_t diag_t0 = esp_timer_get_time();
    while (!ctx.closing->load()) {
        int want = ctx.want->load();
        if (esp_timer_get_time() - diag_t0 >= 30000000) {   // 30s 诊断
            mclog::tagInfo(TAG, "diag: read={}KB stalls={} underruns={}",
                           s_diag_read / 1024, s_diag_stalls, s_diag_underruns);
            s_diag_read = s_diag_stalls = s_diag_underruns = 0;
            diag_t0 = esp_timer_get_time();
        }

        if (want != cur) {               // 换台 / 停止 / 重试
            teardown_stream(http);
            cur = want;
            odd_carry = 0;
            have_odd = false;
            stall = 0;

            if (cur < 0) {
                ctx.state->store(0);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 播放期关 WiFi 省电: 省电模式的收发突发化 → 流式 PCM 供不上 →
            // 卡顿(2026-10-26 实测"声音卡"; 之前的崩溃是 bsp 空指针, 与此无关)
            esp_wifi_set_ps(WIFI_PS_NONE);
            ctx.state->store(1);

            char url[128];
            snprintf(url, sizeof(url), "%s%s", kRelayBase, kStations[cur].path);
            esp_http_client_config_t hcfg = {};
            hcfg.url          = url;
            hcfg.buffer_size  = 4096;   // 大收包缓冲: 流式吞吐(小缓冲实测只拿到 ~70%)
            hcfg.buffer_size_tx = 1024;
            hcfg.timeout_ms   = 8000;
            http = esp_http_client_init(&hcfg);
            bool ok = false;
            if (http && esp_http_client_open(http, 0) == ESP_OK && !ctx.closing->load()) {
                esp_http_client_fetch_headers(http);
                int sc = esp_http_client_get_status_code(http);
                int first = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
                if (sc == 200 && first > 0) {
                    ok = true;
                    if (!s_ring)
                        s_ring = (uint8_t*)heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    s_rs.prev = 0; s_rs.phase = 0;   // 重采样器随新流复位
                    feed_pcm(inbuf, first, odd_carry, have_odd);
                    mclog::tagInfo(TAG, "stream up: {} (sc={} first={}B ring={})",
                                   kStations[cur].name, sc, first, s_ring ? "ok" : "none");
                }
            }
            if (!ok) {
                mclog::tagWarn(TAG, "connect failed: {}", kStations[cur].name);
                teardown_stream(http);
                cur = -1;
                ctx.state->store(3);
                continue;
            }
            esp_http_client_set_timeout_ms(http, 1000);   // 感知停止/断流
            ctx.state->store(2);
        }

        if (!http) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        int n = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
        if (n == 0) {                    // 服务端关闭
            teardown_stream(http);
            cur = -1;
            ctx.state->store(3);
            continue;
        }
        if (n < 0) {
            if (n != -ESP_ERR_HTTP_EAGAIN) {
                teardown_stream(http);
                cur = -1;
                ctx.state->store(3);
                continue;
            }
            s_diag_stalls++;
            ring_drain();                         // 断流期环内余量续播
            if (++stall >= 15) {                  // 连续 15s 无数据才判失败
                teardown_stream(http);
                cur = -1;
                ctx.state->store(3);
            }
            continue;
        }
        stall = 0;
        s_diag_read += n;
        levels_update((const int16_t*)inbuf, n / 2, ctx);
        feed_pcm(inbuf, n, odd_carry, have_odd);
        ring_drain();
    }

    teardown_stream(http);
    ctx.lvl_b->store(0); ctx.lvl_m->store(0); ctx.lvl_t->store(0);
    mclog::tagInfo(TAG, "stream task exit");
}

}  // namespace

void AppRadio::_startTask()
{
    if (_task_running.load()) return;
    _task_running.store(true);
    GetHAL()->tryRunDetached([this]() {
        radio_task_fn({&_want_station, &_app_closing, &_state, &_lvl_b, &_lvl_m, &_lvl_t});
        _task_running.store(false);
    });
}

#else   // 桌面模拟器: 无流引擎
void AppRadio::_startTask() {}
#endif

// ═══════════════════════════════════════════════════════════════════════
// UI
// ═══════════════════════════════════════════════════════════════════════
AppRadio::AppRadio()
{
    setAppInfo().name = "AppRadio";
}

void AppRadio::onCreate()
{
    // 与 HA/股票等一致: 安装不自动打开, 等用户从主页点入 (open 会抢主屏)
}

int AppRadio::_row_y(int i) const { return 6 + i * 47; }   // 14 台一屏全显, 免滚动

void AppRadio::onOpen()
{
    if (_scr) return;   // 防御: 状态异常时重复 onOpen 不叠屏(泄漏)
    mclog::tagInfo(TAG, "on open");
    snap_theme();
    // 后台播放(2026-10-28): 抑制时钟遮盖的条件是"在播"而非"页面前台"。
    // 重开收音机时若流任务还在播, 这里即刻恢复抑制。
    if (_suppress) _suppress(streamActive());
    _app_closing.store(false);
    _ui_state    = -1;
    _ui_station  = -1;

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 左: 电台列表(滚动) ──
    lv_obj_t* list = lv_obj_create(_scr);
    lv_obj_set_pos(list, 40, 28);
    lv_obj_set_size(list, 560, 664);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);   // 无卡片底: 与黑底一体
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 18, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);   // 一屏全显, 禁滚动: 上拨留给退出手势

    _row_sel = lv_obj_create(list);
    lv_obj_set_size(_row_sel, 544, 44);
    lv_obj_set_pos(_row_sel, 8, _row_y(0));
    lv_obj_set_style_bg_color(_row_sel, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_bg_opa(_row_sel, LV_OPA_80, 0);
    lv_obj_set_style_border_width(_row_sel, 0, 0);
    lv_obj_set_style_radius(_row_sel, 12, 0);
    lv_obj_clear_flag(_row_sel, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    for (int i = 0; i < kStationCount; i++) {
        lv_obj_t* row = lv_obj_create(list);
        lv_obj_set_size(row, 544, 44);
        lv_obj_set_pos(row, 8, _row_y(i));
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, _rowClick_cb, LV_EVENT_CLICKED, this);

        char freq[20];
        if (kStations[i].freq > 0) snprintf(freq, sizeof(freq), "%d.%d", kStations[i].freq / 10, kStations[i].freq % 10);
        else                        snprintf(freq, sizeof(freq), "网络");
        lv_obj_t* fl = lv_label_create(row);
        lv_obj_set_style_text_font(fl, zh_font_lg(), 0);   // 含中文"网络"
        lv_obj_set_style_text_color(fl, lv_color_hex(C_ACCENT), 0);
        lv_label_set_text(fl, freq);
        lv_obj_align(fl, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t* nl = lv_label_create(row);
        lv_obj_set_style_text_font(nl, zh_font_lg(), 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(nl, kStations[i].name);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, 132, 0);
    }

    // ── 右: 播放面板 ──
    lv_obj_t* panel = lv_obj_create(_scr);
    lv_obj_set_pos(panel, 640, 28);
    lv_obj_set_size(panel, 600, 664);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);  // 无卡片底: 与黑底一体
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    _status_lbl = lv_label_create(panel);   // 状态即顶行文字(待机/连接中/正在播放/失败)
    lv_label_set_text(_status_lbl, "待机");
    lv_obj_set_style_text_font(_status_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(_status_lbl, 40, 20);

    // 频率读数: FM 台显示真实频点, 网络台不编造(leo-radio 原则)
    _now_freq = lv_label_create(panel);
    lv_label_set_text(_now_freq, "—");
    lv_obj_set_style_text_font(_now_freq, zh_font_md(), 0);   // 含中文"网络电台"
    lv_obj_set_style_text_color(_now_freq, lv_color_hex(C_AMBER), 0);
    lv_obj_align(_now_freq, LV_ALIGN_TOP_RIGHT, -40, 14);

    _now_name = lv_label_create(panel);
    lv_label_set_text(_now_name, "—");
    lv_obj_set_style_text_font(_now_name, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_now_name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(_now_name, 40, 64);
    lv_label_set_long_mode(_now_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_now_name, 520);

    // ── 调谐刻度盘(参考 leo-radio / Passport radio_app) ─────────────
    // 88.0-108.0MHz 横向刻度: 主刻度每 2MHz 带数字, 副刻度每 0.5MHz;
    // 刻度与基线走绘制事件直绘(零对象零分配), 指针+光晕是仅有的两个子对象。
    {
        constexpr int DIAL_X = 40, DIAL_Y = 128, DIAL_W = 520, DIAL_H = 92;
        constexpr int TRK_L = 28, TRK_R = 492;             // 刻度轨道(盘内坐标)
        _dial = lv_obj_create(panel);
        lv_obj_set_pos(_dial, DIAL_X, DIAL_Y);
        lv_obj_set_size(_dial, DIAL_W, DIAL_H);
        lv_obj_set_style_bg_color(_dial, lv_color_hex(C_DIAL_BG), 0);
        lv_obj_set_style_bg_opa(_dial, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_dial, 1, 0);
        lv_obj_set_style_border_color(_dial, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_radius(_dial, 10, 0);
        lv_obj_set_style_pad_all(_dial, 0, 0);
        lv_obj_clear_flag(_dial, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_event_cb(_dial, _dial_draw_cb, LV_EVENT_DRAW_MAIN, (void*)nullptr);

        // 主刻度数字 88..108 每 2MHz(静态 label, 只建一次)
        for (int mhz = 88; mhz <= 108; mhz += 2) {
            int x = TRK_L + (long)(mhz * 10 - 880) * (TRK_R - TRK_L) / 200;
            lv_obj_t* num = lv_label_create(_dial);
            lv_obj_set_style_text_font(num, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(num, lv_color_hex(C_TEXT2), 0);
            lv_label_set_text_fmt(num, "%d", mhz);
            lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(num, 34);
            lv_obj_set_pos(num, x - 17, 8);
        }

        // 指针光晕(跟随指针) + 琥珀指针
        _needle_glow = lv_obj_create(_dial);
        lv_obj_set_size(_needle_glow, 18, 8);
        lv_obj_set_pos(_needle_glow, _needle_x(_freq_of(0)) - 9, 50);
        lv_obj_set_style_bg_color(_needle_glow, lv_color_hex(C_AMBER), 0);
        lv_obj_set_style_bg_opa(_needle_glow, LV_OPA_20, 0);
        lv_obj_set_style_border_width(_needle_glow, 0, 0);
        lv_obj_set_style_radius(_needle_glow, 4, 0);
        lv_obj_clear_flag(_needle_glow, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        _needle = lv_obj_create(_dial);
        lv_obj_set_size(_needle, 4, 30);
        lv_obj_set_pos(_needle, _needle_x(_freq_of(0)), 30);
        lv_obj_set_style_bg_color(_needle, lv_color_hex(C_AMBER), 0);
        lv_obj_set_style_bg_opa(_needle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_needle, 0, 0);
        lv_obj_set_style_radius(_needle, 2, 0);
        lv_obj_clear_flag(_needle, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_set_user_data(_needle, _needle_glow);
    }

    // ── 电平图: 单条带对象, 20 根条在绘制事件里直绘(零对象开销) ──
    // 刻度盘底 240 + 8 → 条带 248 起; 底 y=364(按钮顶 388 - 24), 最高 112px
    _vu_strip = lv_obj_create(panel);
    lv_obj_set_pos(_vu_strip, 40, 228);
    lv_obj_set_size(_vu_strip, 520, 158);
    lv_obj_set_style_bg_opa(_vu_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_vu_strip, 0, 0);
    lv_obj_set_style_pad_all(_vu_strip, 0, 0);
    lv_obj_clear_flag(_vu_strip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_event_cb(_vu_strip, _vu_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);

    auto mk_btn = [&](int x, int y, int d, const lv_font_t* ifont, const char* sym, lv_obj_t** icon_out) {
        lv_obj_t* b = lv_obj_create(panel);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_size(b, d, d);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        // 按下变色 (同主页按钮, 2026-10-28 用户要求)
        lv_obj_set_style_bg_color(b, lv_color_mix(lv_color_hex(0), lv_color_hex(C_CARD_ON), 178), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_opa(b, LV_OPA_70, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* ic = lv_label_create(b);
        lv_obj_set_style_text_font(ic, ifont, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(ic, sym);
        lv_obj_center(ic);
        if (icon_out) *icon_out = ic;
        return b;
    };
    lv_obj_t* prev = mk_btn(105, 428, 70, &lv_font_montserrat_28, LV_SYMBOL_PREV, nullptr);
    lv_obj_add_event_cb(prev, _prevClick_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* play = mk_btn(245, 408, 110, &lv_font_montserrat_32, LV_SYMBOL_PLAY, &_play_icon);
    lv_obj_add_event_cb(play, _playClick_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* next = mk_btn(425, 428, 70, &lv_font_montserrat_28, LV_SYMBOL_NEXT, nullptr);
    lv_obj_add_event_cb(next, _nextClick_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* vl = lv_label_create(panel);
    lv_label_set_text(vl, "音量");
    lv_obj_set_style_text_font(vl, zh_font_md(), 0);
    lv_obj_set_style_text_color(vl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(vl, 40, 600);

    int vol = xiaozhi_get_speaker_volume();
    _vol_slider = lv_slider_create(panel);
    lv_obj_set_size(_vol_slider, 320, 14);
    lv_obj_set_pos(_vol_slider, 130, 610);
    lv_slider_set_range(_vol_slider, 0, 100);
    lv_slider_set_value(_vol_slider, vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(_vol_slider, _vol_cb, LV_EVENT_VALUE_CHANGED, this);

    _vol_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(_vol_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_vol_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(_vol_lbl, 480, 605);
    lv_label_set_text_fmt(_vol_lbl, "%d%%", vol);

    _installSwipeGesture();
    lv_screen_load(_scr);

    // 几何版本标记: 从串口确证当前固件的布局参数(排查改了没生效)
    mclog::tagInfo(TAG, "ui-geom: vu_base=380 vu_max=88 btn_play_y=388 btn_d=110/70 vol_y=532");

    _startTask();
}

void AppRadio::onRunning()
{
    // 若上次任务已退出(如 app 重开竞态)而用户想播 → 拉起新任务
    if (!_task_running.load()) _startTask();

    if (_state.load() != _ui_state)        _refreshStateUi();
    if (_want_station.load() != _ui_station) _refreshStationUi();

    uint32_t now = GetHAL()->millis();
    if (now - _vu_ms >= 120) {             // 8fps: CPU 让路音频(Passport 电平表教训)
        _vu_ms = now;
        _updateVu();
    }
}

// 电平条: 低频靠左 / 中频居中 / 高频靠右 插值; 各条平滑系数不同→自然跳动。
// 只做数值平滑 + 一次 invalidate, 绘制全部在 _vu_draw_cb(LVGL 渲染线程单趟)。
void AppRadio::_updateVu()
{
    if (!_vu_strip) return;
    float b = _lvl_b.load(), m = _lvl_m.load(), t = _lvl_t.load();
    bool changed = false;
    for (int i = 0; i < 20; i++) {
        float w = i / 19.0f;
        float wb = w < 0.5f ? 1 - 2 * w : 0;          // 左半→bass
        float wt = w > 0.5f ? 2 * w - 1 : 0;          // 右半→treble
        float wm = 1 - wb - wt;                       // 中间→mid
        float v = (b * wb + m * wm + t * wt);
        float target = 4 + v * 1.48f;                 // 4..152px(底 384, 刻度盘下 228 起)
        if (target > 152) target = 152;
        float k = target > _vu_cur[i] ? 0.45f : 0.10f + (i % 5) * 0.04f;
        _vu_cur[i] += (target - _vu_cur[i]) * k;
        int h = (int)(_vu_cur[i] + 0.5f);
        if (h < 3) h = 3;
        if (h > 152) h = 152;
        if ((uint16_t)h != s_vu_h[i]) { s_vu_h[i] = h; changed = true; } // 变化才重绘
    }
    if (changed) lv_obj_invalidate(_vu_strip);
}

// 绘制事件: 20 根琥珀矩形自底(条带内 y=144)向上生长
void AppRadio::_vu_draw_cb(lv_event_t* e)
{
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_90;
    dsc.bg_color = lv_color_hex(C_AMBER);
    dsc.radius = 2;
    for (int i = 0; i < 20; i++) {
        int h = s_vu_h[i];
        lv_area_t a;
        a.x1 = oc.x1 + i * 26;
        a.x2 = a.x1 + 19;
        a.y2 = oc.y1 + 156;          // 条底(条带内 y=156 → 面板 y=384)
        a.y1 = a.y2 - h + 1;
        lv_draw_rect(layer, &dsc, &a);
    }
}

void AppRadio::onClose()
{
    mclog::tagInfo(TAG, "on close");
    // 后台播放(2026-10-28): 关掉 UI 不停流 —— _want_station/_app_closing
    // 都不动, 流任务继续, atomics 随实例常驻; 重开收音机 onOpen 后 UI 从
    // 原子量恢复显示当前播放状态。彻底停止 = 在收音机页按暂停 (want=-1)。
    // 抑制条件跟随播放状态: 在播则时钟遮盖继续压制, 未播则解除。
    if (_suppress) _suppress(streamActive());
    // 先切回主屏再删对象 —— 删除 active screen 会让 LVGL 悬空(AppHA 同序)
    if (_close_cb) _close_cb();
    *_alive = false;
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
}

// ── 刻度盘(参考 leo-radio) ─────────────────────────────────────────────
// 轨道几何与 onOpen 里的 DIAL 常量一致
static constexpr int kTrkL = 28, kTrkR = 492;
static constexpr int kBandLow = 880, kBandHigh = 1080;   // 0.1MHz

int AppRadio::_freq_of(int idx) const
{
    if (idx < 0 || idx >= kStationCount) return kBandLow;
    if (kStations[idx].freq != 0) return kStations[idx].freq;
    // 网络台按列表位置均布(两端内缩, 防指针贴边); 读数不编造频点
    int span = kBandHigh - kBandLow;
    return kBandLow + span / 8 + (long)idx * span * 3 / 4 / (kStationCount - 1);
}

int AppRadio::_needle_x(int freq_tenths) const
{
    return kTrkL + (long)(freq_tenths - kBandLow) * (kTrkR - kTrkL) / (kBandHigh - kBandLow);
}

void AppRadio::_needle_anim_cb(void* var, int32_t v)
{
    auto* needle = static_cast<lv_obj_t*>(var);
    lv_obj_set_x(needle, v);
    if (auto* glow = static_cast<lv_obj_t*>(lv_obj_get_user_data(needle)))
        lv_obj_set_x(glow, v - 9);
}

void AppRadio::_move_needle(int idx, bool animate)
{
    if (!_needle) return;
    int target = _needle_x(_freq_of(idx));
    int cur = lv_obj_get_x(_needle);
    lv_anim_del(_needle, nullptr);
    if (animate && target != cur) {
        int dist = target > cur ? target - cur : cur - target;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, _needle);
        lv_anim_set_exec_cb(&a, _needle_anim_cb);
        lv_anim_set_values(&a, cur, target);
        uint32_t dur = 220 + (uint32_t)dist * 2;
        if (dur > 650) dur = 650;
        lv_anim_set_duration(&a, dur);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        _needle_anim_cb(_needle, target);
    }
}

// 刻度与基线直绘(LVGL9 绘制坐标为屏幕绝对坐标, 须加对象自身偏移)
void AppRadio::_dial_draw_cb(lv_event_t* e)
{
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    auto rect = [&](int x, int y, int w, int h, uint32_t color, int radius = 0) {
        dsc.bg_color = lv_color_hex(color);
        dsc.radius = radius;
        lv_area_t a;
        a.x1 = oc.x1 + x;
        a.y1 = oc.y1 + y;
        a.x2 = a.x1 + w - 1;
        a.y2 = a.y1 + h - 1;
        lv_draw_rect(layer, &dsc, &a);
    };

    const int span = kTrkR - kTrkL;
    for (int i = 0; i <= 40; i++) {          // 每 0.5MHz 一格, 每 2MHz 主刻度
        bool major = (i % 4 == 0);
        if (!major && (i % 2) != 0) continue; // 只画整 MHz 的副刻度
        int x = kTrkL + (long)i * span / 40;
        rect(x, major ? 32 : 38, major ? 2 : 1, major ? 14 : 8,
             major ? C_AMBER_SOFT : 0x2A4A6A);
    }
    rect(kTrkL - 8, 47, span + 16, 2, 0x1E3A5C);   // 基线
}

// ── 状态/选台刷新 ──────────────────────────────────────────────────────
void AppRadio::_refreshStateUi()
{
    static const char* kTxt[4]    = {"待机", "连接中…", "正在播放", "连接失败"};
    static const uint32_t kCol[4] = {C_TEXT2, C_WARN, C_OK, C_DANGER};
    _ui_state = _state.load();
    if (_status_lbl) {
        lv_label_set_text(_status_lbl, kTxt[_ui_state]);
        lv_obj_set_style_text_color(_status_lbl, lv_color_hex(kCol[_ui_state]), 0);
    }
    if (_play_icon) {
        lv_label_set_text(_play_icon,
            (_ui_state == 1 || _ui_state == 2) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    // 播放状态变化 → 同步时钟遮盖抑制 (后台播放语义, 见 onClose)
    if (_suppress) _suppress(streamActive());
}

bool AppRadio::streamActive() const
{
    // want>=0 = 播放或重连中 (连接失败也会自动重试, 视作在播)
    return _task_running.load() && _want_station.load() >= 0;
}

std::string AppRadio::streamInfo() const
{
    int w = _want_station.load();
    if (!_task_running.load() || w < 0 || w >= kStationCount) return std::string();
    return std::string(kStations[w].name);
}

void AppRadio::_refreshStationUi()
{
    _ui_station = _want_station.load();
    int show = (_ui_station >= 0) ? _ui_station : _last_station;
    if (show < 0 || show >= kStationCount) return;
    if (_row_sel) {
        lv_obj_set_y(_row_sel, _row_y(show));
        lv_obj_scroll_to_view(_row_sel, LV_ANIM_ON);
    }
    if (_now_name) lv_label_set_text(_now_name, kStations[show].name);
    if (_now_freq) {
        if (kStations[show].freq > 0)
            lv_label_set_text_fmt(_now_freq, "%d.%d MHz", kStations[show].freq / 10, kStations[show].freq % 10);
        else
            lv_label_set_text(_now_freq, "网络电台");
    }
    _move_needle(show, true);
}

// ── 交互 ───────────────────────────────────────────────────────────────
void AppRadio::_play(int idx)
{
    if (idx < 0 || idx >= kStationCount) return;
    _last_station = idx;
    _want_station.store(idx);
}

void AppRadio::_rowClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    app->_play((int)(intptr_t)lv_obj_get_user_data(
        static_cast<lv_obj_t*>(lv_event_get_target(e))));
}

void AppRadio::_playClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    int st = app->_state.load();
    if (st == 1 || st == 2) app->_want_station.store(-1);   // 暂停(断流)
    else                    app->_play(app->_last_station); // 播放/重试
}

void AppRadio::_prevClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    app->_play((app->_last_station - 1 + kStationCount) % kStationCount);
}

void AppRadio::_nextClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    app->_play((app->_last_station + 1) % kStationCount);
}

void AppRadio::_vol_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(app->_vol_slider);
    xiaozhi_set_speaker_volume(v);
    lv_label_set_text_fmt(app->_vol_lbl, "%d%%", v);
}

// ── 上拨手势退出(app_stocks 同款) ──────────────────────────────────────
void AppRadio::_installSwipeGesture()
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
                if (lv_indev_get_gesture_dir(gdev) == LV_DIR_TOP) {
                    // 双守卫: 只在本 app 前台 + 屏幕最下缘 (y>650) 起手
                    // (indev 手势广播给所有注册回调, 2026-10-27 实锤)
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

void AppRadio::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // _gesture_ctx 故意不释放(同 app_stocks: 移除后回调不再触达, 泄漏 16B 可忽略)
}

void AppRadio::_requestClose()
{
    // 小智(app_xiaozhi)同款 —— 主页系 app 的正确退出: close() 让 mooncake
    // 真正关闭本 app(onClose 里切主屏+清理)。只调 _close_cb 的话 mooncake
    // 状态不切换, 收音机仍为激活 app, 主屏假死(2026-10-26 实锤)。
    // stocks/puzzle 的 _close_cb() 模式仅适用于 close_cb=openApp() 的工具页 app。
    close();
}
