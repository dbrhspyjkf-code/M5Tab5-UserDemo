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
#ifdef PLATFORM_BUILD_WEB
#include "web_media.h"   // platforms/web/hal (仅 web 构建在 include 路径)
#endif
#include "../utils/theme/theme.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <xiaozhi_ctl.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>

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
static const lv_font_t* zh_font_sm()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#elif defined(PLATFORM_BUILD_WEB)
// web: 三档 cbin 与真机逐字节一致 (资产镜像)
#include "web_fonts.h"
static const lv_font_t* zh_font_lg() { return webui::font_30(); }
static const lv_font_t* zh_font_md() { return webui::font_24(); }
static const lv_font_t* zh_font_sm() { return webui::font_20(); }
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh_font_lg() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_md() { return &font_puhui_20_4; }
static const lv_font_t* zh_font_sm() { return &font_puhui_20_4; }
#endif

static const char* TAG = "radio";

// 电台表(与 Passport 同源中继; freq=真实 FM 频点 0.1MHz, 0=纯网络台)
struct RadioStation {
    const char* name;
    const char* path;
    int freq;
};

// ── 动态电台表(2026-11-02): 列表以 relay /radio/stations 下发为准, 终端
// 增删台零改动; 内置表仅作离线兜底。shared_ptr + mutex 换表, 读者局部
// 持有快照(流任务遍历期间旧表存活, 无迭代器失效)。
struct StationDyn {
    std::string name;
    std::string path;
    int freq = 0;
};
static std::mutex s_st_mu;
static std::shared_ptr<const std::vector<StationDyn>> s_stations;
static void st_replace(std::shared_ptr<const std::vector<StationDyn>> v)
{
    std::lock_guard<std::mutex> g(s_st_mu);
    s_stations = std::move(v);
}
static uint16_t s_vu_h[20] = {0};   // 当前条高(px), draw 回调与 _updateVu 共享
static uint16_t s_vu_pk[20] = {0};  // 峰值帽高(px): 超峰记录, 8fps 每帧 ×0.92 缓降

// ICY「正在播放」歌名(relay /nowplaying 端点, 流任务 20s 轮询; seq=0 表示
// 写入中, UI 见非 0 新值才拷贝 —— 单写者 seqlock-lite, 任务绝不触碰 LVGL)
static char s_np_title[192] = {0};
static std::atomic<uint32_t> s_np_seq{0};
[[maybe_unused]] static uint32_t s_np_gen = 0;

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
    {"CNN 新闻",        "cnn",   0},
    {"CNN 国际台",      "cnni",  0},
    {"MSNBC 新闻",      "msnbc", 0},
    {"WNYC 纽约公共",   "wnyc",  0},
    {"WBEZ 芝加哥公共", "wbez",  0},
    {"KEXP 独立音乐",   "kexp",  0},
    {"Radio Paradise",  "rp",    0},
};
static const int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

// 快照访问(懒加载内置种子; 读者局部持有 shared_ptr, 换表不影响在用快照)
static std::shared_ptr<const std::vector<StationDyn>> st_snapshot()
{
    {
        std::lock_guard<std::mutex> g(s_st_mu);
        if (!s_stations) {   // 懒加载: 内置表作离线兜底种子
            auto v = std::make_shared<std::vector<StationDyn>>();
            for (const auto& s : kStations) v->push_back({s.name, s.path, s.freq});
            s_stations = v;
        }
        return s_stations;
    }
}
static int st_count()
{
    auto t = st_snapshot();
    return t ? (int)t->size() : 0;
}
static bool st_at(int i, StationDyn& out)   // 越界返回 false
{
    auto t = st_snapshot();
    if (!t || i < 0 || i >= (int)t->size()) return false;
    out = (*t)[i];
    return true;
}
static const char* kRelayBase  = "http://192.168.1.200:8769/radio/";

// 播放输出(2026-11-01): pid=nullptr 即本机 I2S 直放; 外部经 relay→MA 转投。
// player_id 是 Music Assistant 内的 ID(与播放器 app 的两页同一对设备)。
struct RadioOutput { const char* name; const char* full; const char* pid; };
static const RadioOutput kOutputs[] = {
    {"本机",   "本机播放",       nullptr},
    {"客厅",   "客厅 Sonos",     "RINCON_804AF273361501400"},
    {"起居室", "起居室 HomePod", "ap464e4403a588"},
};
static const int kOutputCount = sizeof(kOutputs) / sizeof(kOutputs[0]);

// 配色: 主题快照(onOpen 时从全局主题取, 见 utils/theme/theme.h);
// 绘制回调在 LVGL 线程里读这些全局, 零锁。
static uint32_t C_BG, C_CARD, C_CARD_ON, C_ACCENT, C_TEXT, C_TEXT2,
                C_AMBER, C_AMBER_SOFT, C_DIAL_BG, C_BORDER, C_TRACK,
                C_OK, C_WARN, C_DANGER;

static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    C_BG = T.page;      // 屏底随主题
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
    std::atomic<bool>* ext_on;  // 外部设备在播: 抑制本机拉流
    std::atomic<bool>* np_kick; // UI 请求立即清+拉歌名(外部换台用)
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

// ①ICY 正在播放歌名: relay 对上游另开 Icy-MetaData 连接收割 StreamTitle,
// 端点直接回 text/plain 歌名(HLS 国内台无 ICY, relay 负缓存, 返回空串)。
// 短超时失败静默 —— 主流不被它阻塞(ring 128KB≈4s 可吸收收割期停喂)。
void fetch_nowplaying(int idx)
{
    StationDyn st;
    if (!st_at(idx, st)) return;
    char url[160];
    snprintf(url, sizeof(url), "%s%s/nowplaying", kRelayBase, st.path.c_str());
    esp_http_client_config_t cfg = {};
    cfg.url         = url;
    cfg.timeout_ms  = 4500;
    cfg.buffer_size = 1024;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) == 200) {
            char buf[160] = {0};
            int n = esp_http_client_read(h, buf, (int)sizeof(buf) - 1);
            if (n > 0) {
                while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
                    buf[--n] = 0;
                if (n > 0) {
                    s_np_seq.store(0);
                    memcpy(s_np_title, buf, n + 1);
                    s_np_seq.store(++s_np_gen);
                    mclog::tagInfo(TAG, "np: {}", s_np_title);
                }
            }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
}

// 外部播放电平流(2026-11-02): 拉 relay /lv(~40B/s 文本 "b m t\\n"),
// 解析写三频段电平原子量 —— VU 由真实音频电平驱动(ffmpeg astats 收割)。
// 阻塞至: 外部停止(ext_on=false)/换台(want 变)/断流。返回是否收到过数据
// (false 时调用方退避再重连)。
static bool fetch_levels_stream(int station, TaskCtx& ctx)
{
    StationDyn st;
    if (!st_at(station, st)) return false;
    char url[160];
    snprintf(url, sizeof(url), "%s%s/lv", kRelayBase, st.path.c_str());
    esp_http_client_config_t cfg = {};
    cfg.url         = url;
    cfg.timeout_ms  = 4000;
    cfg.buffer_size = 512;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    char line[32];
    int line_len = 0;
    bool got_any = false;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) == 200) {
            esp_http_client_set_timeout_ms(h, 2000);
            static char inbuf_lv[64];
            while (!ctx.closing->load() && ctx.ext_on->load() &&
                   ctx.want->load() == station) {
                int n = esp_http_client_read(h, inbuf_lv, sizeof(inbuf_lv));
                if (n == 0) break;          // 服务端关闭
                if (n < 0) {
                    if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
                    break;                  // 断流: 外层重连
                }
                got_any = true;
                for (int i = 0; i < n; i++) {
                    char c = inbuf_lv[i];
                    if (c == '\n') {
                        line[line_len] = 0;
                        int b, m, t;
                        if (sscanf(line, "%d %d %d", &b, &m, &t) == 3) {
                            ctx.lvl_b->store(b);
                            ctx.lvl_m->store(m);
                            ctx.lvl_t->store(t);
                        }
                        line_len = 0;
                    } else if (line_len < (int)sizeof(line) - 1) {
                        line[line_len++] = c;
                    }
                }
            }
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return got_any;
}

// 换台失败/断流后进入退避; mark_fail 统一三条失败路径
void radio_task_fn(TaskCtx ctx)
{
    esp_http_client_handle_t http = nullptr;
    int cur = -2;
    static uint8_t inbuf[4096];
    uint8_t odd_carry = 0;
    bool have_odd = false;
    int stall = 0;

    // ②换台去抖(参考 esp32-p4-webradio commit_station): 目标稳定 300ms 才
    // 拆建流, 连点 prev/next 合并; 停止(want<0)与重按同台(值不变)立即生效
    int last_want = -99;
    int64_t want_changed_at = 0;
    // ③失败退避: 同台重试按 3s→6s→12s→24s→30s 封顶; 换到别的台不受限
    int fail_count = 0;
    int failed_station = -1;
    int64_t retry_ok_at = 0;
    int64_t np_last = 0;   // ①ICY 歌名轮询节流

    auto mark_fail = [&](int st, const char* why) {
        fail_count++;
        failed_station = st;
        int bo = 3000 << (fail_count - 1 > 4 ? 4 : fail_count - 1);
        if (bo > 30000) bo = 30000;
        retry_ok_at = esp_timer_get_time() + (int64_t)bo * 1000;
        teardown_stream(http);
        cur = -1;
        ctx.state->store(3);
        StationDyn stn;
        st_at(st, stn);
        mclog::tagWarn(TAG, "{}: {} (retry in {}s)", why, stn.name, bo / 1000);
    };

    int64_t diag_t0 = esp_timer_get_time();
    while (!ctx.closing->load()) {
        bool ext = ctx.ext_on->load();
        int want = ctx.want->load();

        // ①外部换台的歌名刷新请求: 清旧值 + 本轮立即拉新台
        if (ctx.np_kick->exchange(false)) {
            s_np_seq.store(0);
            s_np_title[0] = 0;
            s_np_seq.store(++s_np_gen);
            np_last = 0;
        }
        // ①ICY 歌名: 每 20s 一次(relay 收割+缓存; 失败静默, 下轮再试)。
        // 本机拉流中(cur>=0) 或 外部播放中(want>=0) 都轮询 —— 端点独立于
        // PCM 流, 外部转投时歌名栏照常走
        {
            int np_idx = ext ? want : (http ? cur : -1);
            if (np_idx >= 0 && esp_timer_get_time() - np_last >= 20000000LL) {
                np_last = esp_timer_get_time();
                fetch_nowplaying(np_idx);
            }
        }

        // ①外部播放中(2026-11-01): 本机流急停; want<0(外部暂停)同步待机态。
        // 2026-11-02: 外部播放期间拉 relay /lv 电平流(~40B/s)驱动 VU——
        // 真实音频电平(relay 侧 ffmpeg astats 三频段 RMS), 断流/失败退避重连。
        // 电平流是小包流(10B/125ms), **WiFi 省电必须关**: DTIM 省电会把
        // 小包攒到几百 ms 才交付 → VU 慢动作(relay 侧到达已实测平滑,
        // 设备端被省电卡住是最后一环; Tab5 桌面插电, 功耗无虞)
        if (ext) {
            if (http) { teardown_stream(http); cur = -1; }
            if (want < 0 && ctx.state->load() != 0)
                ctx.state->store(0);
            if (want >= 0) {
                esp_wifi_set_ps(WIFI_PS_NONE);
                if (!fetch_levels_stream(want, ctx))
                    vTaskDelay(pdMS_TO_TICKS(2000));   // 连不上: 退避再试
            } else {
                esp_wifi_set_ps(WIFI_PS_MAX_MODEM);   // 外部停止: 恢复省电
                ctx.lvl_b->store(0); ctx.lvl_m->store(0); ctx.lvl_t->store(0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        if (want != last_want) {
            last_want = want;
            want_changed_at = esp_timer_get_time();
        }
        bool settled = want < 0 ||
                       (esp_timer_get_time() - want_changed_at) >= 300000;
        if (want >= 0 && want == failed_station &&
            esp_timer_get_time() < retry_ok_at) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (esp_timer_get_time() - diag_t0 >= 30000000) {   // 30s 诊断
            mclog::tagInfo(TAG, "diag: read={}KB stalls={} underruns={}",
                           s_diag_read / 1024, s_diag_stalls, s_diag_underruns);
            s_diag_read = s_diag_stalls = s_diag_underruns = 0;
            diag_t0 = esp_timer_get_time();
        }

        if (want != cur && settled) {    // 换台 / 停止 / 重试
            teardown_stream(http);
            cur = want;
            odd_carry = 0;
            have_odd = false;
            stall = 0;

            if (cur < 0) {
                failed_station = -1;     // 用户主动停: 清退避记忆, 再播立即连
                ctx.state->store(0);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 播放期关 WiFi 省电: 省电模式的收发突发化 → 流式 PCM 供不上 →
            // 卡顿(2026-10-26 实测"声音卡"; 之前的崩溃是 bsp 空指针, 与此无关)
            esp_wifi_set_ps(WIFI_PS_NONE);
            ctx.state->store(1);

            StationDyn stn;
            if (!st_at(cur, stn)) {   // 表更新后下标失效: 视作停止
                cur = -1;
                ctx.state->store(0);
                continue;
            }
            char url[128];
            snprintf(url, sizeof(url), "%s%s", kRelayBase, stn.path.c_str());
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
                                   stn.name, sc, first, s_ring ? "ok" : "none");
                }
            }
            if (!ok) {
                mark_fail(cur, "connect failed");
                continue;
            }
            fail_count = 0;
            failed_station = -1;
            esp_http_client_set_timeout_ms(http, 1000);   // 感知停止/断流
            ctx.state->store(2);
            // 清上一台的歌名并立刻拉新台的(relay 无缓存时会在端点内收割 ~2s)
            s_np_seq.store(0);
            s_np_title[0] = 0;
            s_np_seq.store(++s_np_gen);
            np_last = 0;
        }

        if (!http) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        int n = esp_http_client_read(http, (char*)inbuf, sizeof(inbuf));
        if (n == 0) {                    // 服务端关闭
            mark_fail(cur, "stream ended");
            continue;
        }
        if (n < 0) {
            if (n != -ESP_ERR_HTTP_EAGAIN) {
                mark_fail(cur, "read error");
                continue;
            }
            s_diag_stalls++;
            ring_drain();                         // 断流期环内余量续播
            if (++stall >= 15) {                  // 连续 15s 无数据才判失败
                mark_fail(cur, "stall timeout");
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
    int64_t now = lv_tick_get();
    if (now < _spawn_retry_at) return;   // 上次孵化失败, 退避窗口内不重复试
    _task_running.store(true);
    // 16KB → hal 走 PSRAM 栈, 免疫内部 RAM 碎片期(2026-11-08: 8KB 内部栈
    // 在碎片窗口 pthread_create ENOMEM, 旗标卡 true 致收音机永久哑火)
    bool ok = GetHAL()->tryRunDetached([this]() {
        radio_task_fn({&_want_station, &_app_closing, &_state,
                       &_lvl_b, &_lvl_m, &_lvl_t, &_ext_on, &_np_kick});
        _task_running.store(false);
    }, 16384);
    if (!ok) {
        // 孵化失败必须复位旗标并退避重试: 否则 _task_running 卡 true,
        // 之后按播放/重开 app 全被上面的守卫静默吞掉(实锤一次失败永久哑火)
        _task_running.store(false);
        _spawn_retry_at = now + 2000;
        if (_want_station.load() >= 0) _state.store(3);   // 用户在等播 → UI 出失败态
        mclog::tagWarn(TAG, "stream task spawn failed, retry in 2s");
    }
}

#else   // 桌面模拟器: 无流引擎
#ifdef PLATFORM_BUILD_WEB
// web: 音频由主线程 <audio> 承担 (web_media), 无流任务线程。
// _task_running 在首次 _play 时置位 (♪ 图标/播放判定语义与真机一致)。
void AppRadio::_startTask() {}
#else
void AppRadio::_startTask() {}
#endif
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

int AppRadio::_row_y(int i) const { return 6 + i * 47; }   // 动态行数, 13 行可见, 列表滚动

// ── 动态电台表(2026-11-02) ─────────────────────────────────────────────
// 列表以 relay /radio/stations 为准(增删台只改 relay), 内置表离线兜底。
void AppRadio::_rebuildStationRows()
{
    if (!_list) return;
    lv_obj_clean(_list);   // 旧行+高亮条一起清, 全部重建

    _row_sel = lv_obj_create(_list);
    lv_obj_set_size(_row_sel, 544, 44);
    lv_obj_set_pos(_row_sel, 8, _row_y(0));
    lv_obj_set_style_bg_color(_row_sel, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_bg_opa(_row_sel, LV_OPA_80, 0);
    lv_obj_set_style_border_width(_row_sel, 0, 0);
    lv_obj_set_style_radius(_row_sel, 12, 0);
    lv_obj_clear_flag(_row_sel, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    auto tbl = st_snapshot();
    if (!tbl) return;
    for (size_t i = 0; i < tbl->size(); i++) {
        const StationDyn& st = (*tbl)[i];
        lv_obj_t* row = lv_obj_create(_list);
        lv_obj_set_size(row, 544, 44);
        lv_obj_set_pos(row, 8, _row_y((int)i));
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, _rowClick_cb, LV_EVENT_CLICKED, this);

        char freq[20];
        if (st.freq > 0) snprintf(freq, sizeof(freq), "%d.%d", st.freq / 10, st.freq % 10);
        else             snprintf(freq, sizeof(freq), "网络");
        lv_obj_t* fl = lv_label_create(row);
        lv_obj_set_style_text_font(fl, zh_font_lg(), 0);   // 含中文"网络"
        lv_obj_set_style_text_color(fl, lv_color_hex(C_ACCENT), 0);
        lv_label_set_text(fl, freq);
        lv_obj_align(fl, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t* nl = lv_label_create(row);
        lv_obj_set_style_text_font(nl, zh_font_lg(), 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(nl, st.name.c_str());
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, 132, 0);
    }
}

void AppRadio::_fetchStations()
{
    // 后台线程: 只碰 HTTP 与原子量, 不触 LVGL。失败静默(内置表兜底)。
    auto r = GetHAL()->httpGet(std::string(kRelayBase) + "stations");
    if (r.status != 200) return;
    try {
        auto j = nlohmann::json::parse(r.body);
        auto arr = j.at("stations");
        if (!arr.is_array() || arr.empty()) return;
        auto v = std::make_shared<std::vector<StationDyn>>();
        for (auto& e : arr) {
            StationDyn st;
            st.path = e.value("key", "");
            st.name = e.value("name", "");
            st.freq = e.value("freq", 0);
            if (!st.path.empty() && !st.name.empty())
                v->push_back(std::move(st));
        }
        if (v->empty()) return;
        auto cur = st_snapshot();
        bool same = cur && cur->size() == v->size();
        if (same)
            for (size_t i = 0; i < v->size(); i++)
                if ((*cur)[i].path != (*v)[i].path) { same = false; break; }
        if (same) return;   // 表没变, 不折腾
        // 播放索引重映射(旧 path → 新下标); 播放中的台被删则停(want=-1)
        auto findNew = [&](const std::string& path) {
            for (size_t i = 0; i < v->size(); i++)
                if ((*v)[i].path == path) return (int)i;
            return -1;
        };
        int w = _want_station.load();
        if (cur && w >= 0 && w < (int)cur->size())
            _want_station.store(findNew((*cur)[w].path));
        if (cur && _last_station >= 0 && _last_station < (int)cur->size()) {
            _last_station = findNew((*cur)[_last_station].path);
            if (_last_station < 0) _last_station = 0;
        }
        st_replace(v);
        _stations_dirty.store(true);   // onRunning(LVGL 线程)重建列表
        mclog::tagInfo(TAG, "stations updated: {} 台", v->size());
    } catch (const std::exception& e) {
        mclog::tagWarn(TAG, "stations parse fail: {}", e.what());
    }
}

void AppRadio::onOpen()
{
    if (_scr) return;   // 防御: 状态异常时重复 onOpen 不叠屏(泄漏)
    mclog::tagInfo(TAG, "on open");
    snap_theme();
    // 后台播放(2026-10-28): 抑制时钟遮盖的条件是"在播"而非"页面前台"。
    // 重开收音机时若流任务还在播, 这里即刻恢复抑制。
    if (_suppress) _suppress(streamActive());
    // 前台独占(2026-10-31): 收音机界面期间完全不进屏保(不弹遮盖也不熄屏)
    if (_block) _block(true);
    _app_closing.store(false);
    _ui_state    = -1;
    _ui_station  = -1;
    // 输出选择恢复(NVS; 只记选择, 不自动恢复播放 —— 按播放才播)
    _output_sel = atoi(GetHAL()->getConfig("radio_out", "0").c_str());
    if (_output_sel < 0 || _output_sel >= kOutputCount) _output_sel = 0;
#ifdef PLATFORM_BUILD_WEB
    _output_sel = 0;   // web: 固定浏览器本机播放
#endif

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 左: 电台列表 ──
    // 2026-11-02 加 5 个美式新闻台(共 19)超出 14 行一屏 → 启用滚动。
    // 滚动容器会吃掉其区域内的上拨手势: 列表高度收到 y645, 屏幕 650 以下
    // (上拨退出的起手区)保持空白, 手势不再被列表滚动吞掉。
    // 行内容由 _rebuildStationRows() 按动态表生成(relay /stations 下发)。
    _list = lv_obj_create(_scr);
    lv_obj_set_pos(_list, 40, 28);
    lv_obj_set_size(_list, 560, 617);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);   // 无卡片底: 与黑底一体
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 18, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    _rebuildStationRows();

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

    // ── 输出 chip(2026-11-01): 状态行与频率读数之间, 点开输出选择弹窗。
    // 宽 132 右缘 x=432, 让开右对齐的频率/网络电台读数(最宽"106.2 MHz"
    // 左缘≈440)——初版 190 宽曾盖住该读数
    _out_chip = lv_obj_create(panel);
    lv_obj_set_pos(_out_chip, 300, 12);
    lv_obj_set_size(_out_chip, 132, 42);
    lv_obj_set_style_radius(_out_chip, 21, 0);
    lv_obj_set_style_bg_color(_out_chip, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_bg_opa(_out_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_out_chip, 1, 0);
    lv_obj_set_style_border_color(_out_chip, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_opa(_out_chip, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(_out_chip, 0, 0);
    lv_obj_clear_flag(_out_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_out_chip, LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t* ic = lv_label_create(_out_chip);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(C_ACCENT), 0);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_t* tl = lv_label_create(_out_chip);   // 文字部分(_refreshOutputUi 刷新)
        lv_obj_set_style_text_font(tl, zh_font_md(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT), 0);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 48, 0);
        lv_obj_set_user_data(_out_chip, tl);
    }
#ifdef PLATFORM_BUILD_WEB
    // web 版只在浏览器播放: 隐藏输出选择 chip (真机 Sonos/HomePod 转投不适用)
    lv_obj_add_flag(_out_chip, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_add_event_cb(_out_chip, _outChip_cb, LV_EVENT_CLICKED, this);
    _refreshOutputUi();

    _now_name = lv_label_create(panel);
    lv_label_set_text(_now_name, "—");
    lv_obj_set_style_text_font(_now_name, zh_font_lg(), 0);
    lv_obj_set_style_text_color(_now_name, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(_now_name, 40, 58);
    lv_label_set_long_mode(_now_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_now_name, 520);

    // ①ICY 正在播放歌名(20px 副行; 国内台 HLS 无 ICY = 恒空不占眼)
    _now_title = lv_label_create(panel);
    lv_label_set_text(_now_title, "");
    lv_obj_set_style_text_font(_now_title, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_now_title, lv_color_hex(C_TEXT2), 0);
    lv_label_set_long_mode(_now_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_now_title, 520);
    lv_obj_set_pos(_now_title, 40, 98);
    if (uint32_t sq = s_np_seq.load()) {   // 后台播放中重进: 恢复已取到的歌名
        _np_seen = sq;
        lv_label_set_text(_now_title, s_np_title);
    }

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

    // 手势 alive 旗标重建 (app_stocks 同款): onClose 里 *_alive=false 后,
    // 旧 shared_ptr 永久作废 —— 不重建的话第二次进收音机上拨手势在 alive
    // 检查处静默失效 (2026-10-29 实锤: 后台播放重进后无法上拨回主页)
    _alive = std::make_shared<bool>(true);
    _installSwipeGesture();
    lv_screen_load(_scr);

    // 外部输出在播中重进: 音量滑条同步外部设备音量(异步拉取, onRunning 应用)
    if (_output_sel > 0 && _ext_on.load()) _extVolSync();

    // 动态电台表: 进 app 后台拉一次(relay 增删台对终端零改动)
    GetHAL()->tryRunDetached([this]() { _fetchStations(); });

    // 几何版本标记: 从串口确证当前固件的布局参数(排查改了没生效)
    mclog::tagInfo(TAG, "ui-geom: name_y=58 title_y=98 vu_base=384 vu_max=152 btn_play_y=388 vol_y=532");

    _startTask();
}

void AppRadio::onRunning()
{
#ifdef PLATFORM_BUILD_WEB
    // web: <audio> 状态回读 (SAB) —— 连接中(1) → 播放(2)/错误(3)
    if (_want_station.load() >= 0 && _state.load() == 1) {
        if (webui::web_media_is_playing())      _state.store(2);
        else if (webui::web_media_has_err())    _state.store(3);
    }
#endif
    // 若上次任务已退出(如 app 重开竞态)而用户想播 → 拉起新任务
    if (!_task_running.load()) _startTask();

    if (_state.load() != _ui_state)        _refreshStateUi();
    if (_want_station.load() != _ui_station) _refreshStationUi();

    // 动态表更新落地: 后台线程拉到新表后在这里(LVGL 线程)重建列表
    if (_stations_dirty.exchange(false)) {
        _rebuildStationRows();
        _ui_station = -1;              // 表变了, 强制整套刷新(高亮/台名/指针)
        _refreshStationUi();
    }

    // ①歌名: 流任务换了新值才拷进 label(seq=0 是写入中, 跳过)
    if (uint32_t sq = s_np_seq.load(); sq != 0 && sq != _np_seen && _now_title) {
        _np_seen = sq;
        lv_label_set_text(_now_title, s_np_title);   // 任务 20s 才写一次, 拷贝窗口安全
    }

    // ①外部音量查询结果落地(切输出/重进时拉取; 消费即清)
    if (int ev = _ext_vol.exchange(-1); ev >= 0 && _output_sel > 0 && _vol_slider) {
        lv_slider_set_value(_vol_slider, ev, LV_ANIM_OFF);
        if (_vol_lbl) lv_label_set_text_fmt(_vol_lbl, "%d%%", ev);
    }

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
    // 外部播放时 _lvl_* 由流任务的 /lv 电平流写入(真实音频电平,
    // 源 ~1-2Hz + 8fps 平滑 = 平滑呼吸式跳动); 伪电平动画已移除
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
        // ④峰值帽: 超峰即记, 8fps 每帧 ×0.92 缓降, 永不低于条顶(0.92^8≈0.5/s)
        float pk = s_vu_pk[i];
        if ((float)h >= pk) pk = (float)h;
        else                pk *= 0.92f;
        if (pk < (float)h) pk = (float)h;
        int ph = (int)(pk + 0.5f);
        if (ph < h) ph = h;
        if (ph > 152) ph = 152;
        bool pchg = ((uint16_t)ph != s_vu_pk[i]);
        s_vu_pk[i] = (uint16_t)ph;
        if ((uint16_t)h != s_vu_h[i] || pchg) { s_vu_h[i] = h; changed = true; } // 变化才重绘
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
    // ④峰值帽: 各条峰高的 2px 亮线(琥珀混白), 条到峰时叠成亮顶
    dsc.bg_color = lv_color_mix(lv_color_hex(C_AMBER), lv_color_white(), LV_OPA_50);
    dsc.bg_opa  = LV_OPA_COVER;
    dsc.radius  = 1;
    for (int i = 0; i < 20; i++) {
        int ph = s_vu_pk[i];
        if (ph < 5) continue;
        lv_area_t a;
        a.x1 = oc.x1 + i * 26;
        a.x2 = a.x1 + 19;
        a.y2 = oc.y1 + 156 - ph;
        a.y1 = a.y2 - 1;
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
    // 前台独占解除(2026-10-31): 离开收音机界面恢复常规屏保语义
    // (后台在播 → 不弹遮盖但允许熄屏; 已停 → 完全常规屏保)
    if (_block) _block(false);
    // 先切回主屏再删对象 —— 删除 active screen 会让 LVGL 悬空(AppHA 同序)
    if (_close_cb) _close_cb();
    // 输出弹窗挂在 layer_top(不在 _scr 树上), 关页必须显式收掉
    _closeOutputModal();
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
    StationDyn st;
    if (!st_at(idx, st)) return kBandLow;
    if (st.freq != 0) return st.freq;
    // 网络台按列表位置均布(两端内缩, 防指针贴边); 读数不编造频点
    int span = kBandHigh - kBandLow;
    return kBandLow + span / 8 + (long)idx * span * 3 / 4 / (st_count() - 1);
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
        // 外部播放中: 状态带上输出名("正在播放 · 客厅")
        if (_ui_state == 2 && _output_sel > 0 && _output_sel < kOutputCount)
            lv_label_set_text_fmt(_status_lbl, "%s · %s", kTxt[2], kOutputs[_output_sel].name);
        else
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

void AppRadio::pauseForFocus()
{
    // 油管开播时踢收音机: 本机在播 → want=-1 断流 (后台语义不变, 用户回收音机
    // 可续播); 外放在播 → cmd stop 真停音箱。ui 状态随原子量自然恢复。
    if (_ext_on.load()) {
        _extStop();
        return;
    }
    if (_want_station.load() >= 0) _want_station.store(-1);
}

std::string AppRadio::streamInfo() const
{
    int w = _want_station.load();
    StationDyn st;
    if (!_task_running.load() || !st_at(w, st)) return std::string();
    return st.name;
}

void AppRadio::_refreshStationUi()
{
    _ui_station = _want_station.load();
    int show = (_ui_station >= 0) ? _ui_station : _last_station;
    StationDyn st;
    if (show < 0 || !st_at(show, st)) return;
    if (_row_sel) {
        lv_obj_set_y(_row_sel, _row_y(show));
        lv_obj_scroll_to_view(_row_sel, LV_ANIM_ON);
    }
    if (_now_name) lv_label_set_text(_now_name, st.name.c_str());
    if (_now_freq) {
        if (st.freq > 0)
            lv_label_set_text_fmt(_now_freq, "%d.%d MHz", st.freq / 10, st.freq % 10);
        else
            lv_label_set_text(_now_freq, "网络电台");
    }
    _move_needle(show, true);
}

// ── 交互 ───────────────────────────────────────────────────────────────
void AppRadio::_play(int idx)
{
    if (idx < 0 || idx >= st_count()) return;
    if (_yt_stopper) _yt_stopper();   // 音频焦点: 踢油管 (暂停/停外放)
    _last_station = idx;
    _want_station.store(idx);
    if (_output_sel > 0) { _extStart(idx); return; }   // 外部模式: 转投
#ifdef PLATFORM_BUILD_WEB
    // web 本机模式: <audio> 播 relay mp3 直流; 状态由 onRunning 轮询回读
    _task_running.store(true);
    _state.store(1);   // 连接中
    StationDyn st;
    if (st_at(idx, st)) webui::web_media_radio(st.path.c_str(), true);
#endif
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
    if (st == 1 || st == 2) {
        if (app->_ext_on.load()) app->_extStop();   // 外部: 停投(流任务见 ext_on=false+want<0 置待机)
        app->_want_station.store(-1);               // 暂停(本机断流)
#ifdef PLATFORM_BUILD_WEB
        if (app->_output_sel == 0) webui::web_media_radio(nullptr, false);
#endif
    }
    else                    app->_play(app->_last_station); // 播放/重试
}

void AppRadio::_prevClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    int n = st_count();
    if (n <= 0) return;
    app->_play((app->_last_station - 1 + n) % n);
}

void AppRadio::_nextClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    int n = st_count();
    if (n <= 0) return;
    app->_play((app->_last_station + 1) % n);
}

void AppRadio::_vol_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(app->_vol_slider);
    lv_label_set_text_fmt(app->_vol_lbl, "%d%%", v);
    if (app->_output_sel > 0 && app->_output_sel < kOutputCount) {
        int sel = app->_output_sel;
        GetHAL()->tryRunDetached([app, sel, v]() {   // 外部: 音量转发给 MA
            app->_extSimple(kOutputs[sel].pid, "volume_set", v);
        });
    } else {
#ifdef PLATFORM_BUILD_WEB
        webui::web_media_volume(v);
#else
        xiaozhi_set_speaker_volume(v);
#endif
    }
}

// ── 外部播放命令(relay :8769 → Music Assistant; 阻塞 HTTP) ────────────
// relay JSON 应答成功判定: {"ok":true} / {"ok": true} 均认——2026-11-01
// 实锤: 旧判定只匹配无空格形态, relay 一度返回带空格的 "ok": true, 每次
// play_on 成功都被误判失败(ext_dev 不记录 → 切本机不发 stop, 音箱停不下)
static bool ext_ok(const hal::HalBase::HttpResponse_t& r)
{
    return r.status == 200 &&
           (r.body.find("\"ok\":true") != std::string::npos ||
            r.body.find("\"ok\": true") != std::string::npos);
}

bool AppRadio::_extPlay(const char* pid, const char* path)
{
    char url[192];
    snprintf(url, sizeof(url), "%s%s/play_on?p=%s", kRelayBase, path, pid);
    auto r = GetHAL()->httpGet(url);
    bool ok = ext_ok(r);
    if (!ok) mclog::tagWarn(TAG, "ext play_on failed: sc={} body={}",
                            r.status, r.body.substr(0, 80));
    return ok;
}

bool AppRadio::_extSimple(const char* pid, const char* cmd, int v)
{
    char url[160];
    if (v >= 0) snprintf(url, sizeof(url), "%scmd?p=%s&c=%s&v=%d",
                         kRelayBase, pid, cmd, v);
    else        snprintf(url, sizeof(url), "%scmd?p=%s&c=%s", kRelayBase, pid, cmd);
    auto r = GetHAL()->httpGet(url);
    return ext_ok(r);
}

void AppRadio::_extStart(int station)
{
    int sel = _output_sel;
    StationDyn st;
    if (sel <= 0 || sel >= kOutputCount || !st_at(station, st))
        return;
    _ext_on.store(true);        // 立即抑制本机拉流(流任务下一轮 teardown)
    _state.store(1);            // 连接中
    _np_kick.store(true);       // 清旧台歌名 + 立即拉新台
    GetHAL()->tryRunDetached([this, sel, st]() {
        int old = _ext_dev.exchange(-1);          // 先摘牌: 线程内独占设备状态
        if (old >= 0 && old != sel)
            _extSimple(kOutputs[old].pid, "stop");   // 换设备: 旧的停
        bool ok = _extPlay(kOutputs[sel].pid, st.path.c_str());
        if (ok) {
            _ext_dev.store(sel);
            mclog::tagInfo(TAG, "ext play: {} -> {}",
                           st.name, kOutputs[sel].full);
        } else {
            // 失败不接播本机(外放失败≠改本机): want 置 -1 防流任务自动拉流,
            // 回待机态, 播放键重试(仍走外部)
            _ext_on.store(false);
            _want_station.store(-1);
        }
        _state.store(ok ? 2 : 3);
    });
}

void AppRadio::_extStop()
{
    _ext_on.store(false);
    // 外部暂停死锁修复(2026-11-01): 外部播放中流任务 cur=-1, 暂停后 want=-1
    // 与 cur 相等 → 「want!=cur 才置待机」分支不触发, state 卡 2(播放中),
    // 播放键停在 PAUSE 图标, 再点全走暂停分支。这里显式归待机。
    _state.store(0);
    int dev = _ext_dev.exchange(-1);
    if (dev >= 0 && dev < kOutputCount)
        GetHAL()->tryRunDetached([this, dev]() { _extSimple(kOutputs[dev].pid, "stop"); });
}

void AppRadio::_extVolSync()
{
    int sel = _output_sel;
    if (sel <= 0 || sel >= kOutputCount) return;
    if (_ext_vol_wait.exchange(true)) return;   // 已有查询在途
    GetHAL()->tryRunDetached([this, sel]() {
        // {"ok":true,"state":"..","volume":35} → 取 volume
        char url[160];
        snprintf(url, sizeof(url), "%scmd?p=%s&c=volume_get",
                 kRelayBase, kOutputs[sel].pid);
        auto r = GetHAL()->httpGet(url);
        if (r.status == 200) {
            auto pos = r.body.find("\"volume\":");
            if (pos != std::string::npos) {
                int v = atoi(r.body.c_str() + pos + 9);
                if (v >= 0 && v <= 100) _ext_vol.store(v);
            }
        }
        _ext_vol_wait.store(false);
    });
}

// ── 输出选择 chip / 弹窗 ────────────────────────────────────────────────
void AppRadio::_refreshOutputUi()
{
    if (!_out_chip) return;
    auto* tl = static_cast<lv_obj_t*>(lv_obj_get_user_data(_out_chip));
    if (tl && _output_sel >= 0 && _output_sel < kOutputCount)
        lv_label_set_text(tl, kOutputs[_output_sel].name);
}

void AppRadio::_setOutput(int i)
{
    if (i < 0 || i >= kOutputCount || i == _output_sel) return;
    _output_sel = i;
    GetHAL()->setConfig("radio_out", std::to_string(i));
    _refreshOutputUi();
    bool playing = _want_station.load() >= 0;   // 本机在播/外部在播/连接中
    if (i == 0) {
        // 切回本机: 停外部(异步); want>=0 时流任务见 ext_on=false 自动接播
        _extStop();
    } else if (playing) {
        _extStart(_last_station);               // 外部接播(先停旧设备)
    } else {
        _extVolSync();                          // 待机切换: 仅同步外部音量显示
    }
}

void AppRadio::_openOutputModal()
{
    if (_out_modal) return;
    lv_obj_t* bg = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_size(bg, 1280, 720);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);           // 点背景=关
    lv_obj_add_event_cb(bg, _modalBg_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(bg, _modalDel_cb, LV_EVENT_DELETE, this);  // 任何路径删除都清指针

    lv_obj_t* card = lv_obj_create(bg);
    lv_obj_set_size(card, 560, 500);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_style_text_font(title, zh_font_md(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_label_set_text(title, "播放输出");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    for (int i = 0; i < kOutputCount; i++) {
        lv_obj_t* row = lv_obj_create(card);
        lv_obj_set_pos(row, 40, 84 + i * 128);
        lv_obj_set_size(row, 480, 108);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(
            i == _output_sel ? C_CARD_ON : C_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, i == _output_sel ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(
            i == _output_sel ? C_ACCENT : C_BORDER), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)i);
        lv_obj_add_event_cb(row, _outRow_cb, LV_EVENT_CLICKED, this);

        lv_obj_t* ic = lv_label_create(row);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(
            i == _output_sel ? C_ACCENT : C_TEXT2), 0);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t* nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, zh_font_lg(), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(nm, kOutputs[i].full);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 76, 0);

        if (i == _output_sel) {   // 当前项: 尾部勾
            lv_obj_t* ck = lv_label_create(row);
            lv_obj_set_style_text_font(ck, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(ck, lv_color_hex(C_OK), 0);
            lv_label_set_text(ck, LV_SYMBOL_OK);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -28, 0);
        }
    }
    _out_modal = bg;
}

void AppRadio::_closeOutputModal()
{
    // delete_async: 回调上下文里删祖先 modal 必须异步; DELETE 回调清 _out_modal
    if (!_out_modal) return;
    lv_obj_delete_async(_out_modal);   // _out_modal 由 _modalDel_cb 置空
}

void AppRadio::_outChip_cb(lv_event_t* e)
{
    static_cast<AppRadio*>(lv_event_get_user_data(e))->_openOutputModal();
}

void AppRadio::_outRow_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    app->_closeOutputModal();
    app->_setOutput((int)(intptr_t)lv_obj_get_user_data(
        static_cast<lv_obj_t*>(lv_event_get_target(e))));
}

void AppRadio::_modalBg_cb(lv_event_t* e)
{
    static_cast<AppRadio*>(lv_event_get_user_data(e))->_closeOutputModal();
}

void AppRadio::_modalDel_cb(lv_event_t* e)
{
    // modal 被 delete_async/任何路径删除时清指针, 防 onClose 后悬空判断
    static_cast<AppRadio*>(lv_event_get_user_data(e))->_out_modal = nullptr;
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
                    // 输出弹窗开着: 上拨只收弹窗, 不关 app(弹窗在 layer_top,
                    // 不关的话 app 删除后它仍悬在屏幕上)
                    if (ctx->app->_out_modal) {
                        ctx->app->_closeOutputModal();
                        return;
                    }
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
