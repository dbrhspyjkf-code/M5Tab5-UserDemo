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
#include "../utils/weather/open_meteo.h"   // LOCATE_URL_1: IP 归属地(与主页天气同源)
#include <hal/hal.h>
#include <mooncake_log.h>
#include <xiaozhi_ctl.h>
#include <nlohmann/json.hpp>
#include "../utils/psram_json.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>
#include <algorithm>
#include <utility>
#include <cctype>

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

// 电台表(与 oneapp radio_stations.py 同源; freq=真实 FM 频点 0.1MHz, 0=纯网络台)
// grp/prov/city = 分组与 IP 归属地锚点(oneapp 同款: 省级/地方组本地置顶+「本」徽标)
struct RadioStation {
    const char* name;
    const char* path;
    int freq;
    const char* grp;
    const char* prov;
    const char* city;
};

// ── 动态电台表(2026-11-02): 列表以 relay /radio/stations 下发为准, 终端
// 增删台零改动; 内置表仅作离线兜底。shared_ptr + mutex 换表, 读者局部
// 持有快照(流任务遍历期间旧表存活, 无迭代器失效)。
// 2026-11-15: relay 表换 oneapp 全量 204 台(四组), grp/prov/city 随下发。
struct StationDyn {
    std::string name;
    std::string path;
    int freq = 0;
    std::string grp, prov, city;
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

// 分组页签(oneapp radio_tabs: ★ 收藏在最前, 后接四组); relay 下发 groups
// 时整表替换(顺序与 oneapp GROUPS 一致), 内置为离线兜底。
static const char* kGroupsDef[] = {"国家", "省级", "地方", "国际"};
static std::vector<std::string> s_groups(kGroupsDef, kGroupsDef + 4);

static const RadioStation kStations[] = {
    {"深圳交通广播",     "szjt",  1062, "地方", "广东", "深圳"},
    {"深圳新闻广播",     "szxw",  898,  "地方", "广东", "深圳"},
    {"深圳飞扬971",      "szfy",  971,  "地方", "广东", "深圳"},
    {"深圳生活广播",     "szsh",  942,  "地方", "广东", "深圳"},
    {"深圳综艺广播",     "szzy",  0,    "地方", "广东", "深圳"},
    {"央广中国之声",     "zgzs",  0,    "国家", "",     ""},
    {"央广经济之声",     "jjzs",  0,    "国家", "",     ""},
    {"广东音乐FM99.3",   "gdyy",  993,  "省级", "广东", ""},
    {"羊城交通FM105.2",  "gdyz",  1052, "省级", "广东", ""},
    {"广东新闻FM91.4",   "gdxw",  914,  "省级", "广东", ""},
    {"珠江经济FM97.4",   "zjjs",  974,  "省级", "广东", ""},
    {"BBC World Service", "bbcws", 0,   "国际", "",     ""},
    {"NPR News",         "npr",   0,    "国际", "",     ""},
    {"CNN 新闻",         "cnn",   0,    "国际", "",     ""},
    {"CNN 国际台",       "cnni",  0,    "国际", "",     ""},
    {"MSNBC 新闻",       "msnbc", 0,    "国际", "",     ""},
    {"WNYC 纽约公共",    "wnyc",  0,    "国际", "",     ""},
    {"WBEZ 芝加哥公共",  "wbez",  0,    "国际", "",     ""},
    {"KEXP 独立音乐",    "kexp",  0,    "国际", "",     ""},
    {"Radio Paradise",   "rp",    0,    "国际", "",     ""},
};
static const int kStationCount = sizeof(kStations) / sizeof(kStations[0]);

// 快照访问(懒加载内置种子; 读者局部持有 shared_ptr, 换表不影响在用快照)
static std::shared_ptr<const std::vector<StationDyn>> st_snapshot()
{
    {
        std::lock_guard<std::mutex> g(s_st_mu);
        if (!s_stations) {   // 懒加载: 内置表作离线兜底种子
            auto v = std::make_shared<std::vector<StationDyn>>();
            for (const auto& s : kStations)
                v->push_back({s.name, s.path, s.freq, s.grp, s.prov, s.city});
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

// ── IP 归属地归一(oneapp radio_stations.py 同款移植) ──────────────────
static const char* kProvs[] = {
    "北京", "天津", "上海", "重庆", "河北", "山西", "辽宁", "吉林",
    "黑龙江", "江苏", "浙江", "安徽", "福建", "江西", "山东", "河南",
    "湖北", "湖南", "广东", "海南", "四川", "贵州", "云南", "陕西",
    "甘肃", "青海", "内蒙古", "广西", "西藏", "宁夏", "新疆", "台湾",
};
static const char* kProvSuffix[] = {
    "特别行政区", "维吾尔自治区", "壮族自治区", "回族自治区",
    "自治区", "省", "市",
};
static const std::pair<const char*, const char*> kProvAlias[] = {
    {"beijing", "北京"}, {"tianjin", "天津"}, {"shanghai", "上海"},
    {"chongqing", "重庆"}, {"hebei", "河北"}, {"shanxi", "山西"},
    {"liaoning", "辽宁"}, {"jilin", "吉林"}, {"heilongjiang", "黑龙江"},
    {"jiangsu", "江苏"}, {"zhejiang", "浙江"}, {"anhui", "安徽"},
    {"fujian", "福建"}, {"jiangxi", "江西"}, {"shandong", "山东"},
    {"henan", "河南"}, {"hubei", "湖北"}, {"hunan", "湖南"},
    {"guangdong", "广东"}, {"hainan", "海南"}, {"sichuan", "四川"},
    {"guizhou", "贵州"}, {"yunnan", "云南"}, {"shaanxi", "陕西"},
    {"gansu", "甘肃"}, {"qinghai", "青海"}, {"nei mongol", "内蒙古"},
    {"inner mongolia", "内蒙古"}, {"guangxi", "广西"}, {"tibet", "西藏"},
    {"ningxia", "宁夏"}, {"xinjiang", "新疆"}, {"taiwan", "台湾"},
};

// 归属地字符串 → 省短名(剥行政区后缀; 中英通吃); 不匹配返回空
static std::string norm_prov(std::string t)
{
    if (t.empty()) return "";
    while (t.size() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    for (const char* suf : kProvSuffix) {
        size_t n = strlen(suf);
        if (t.size() > n && t.compare(t.size() - n, n, suf) == 0) {
            t.erase(t.size() - n);
            break;
        }
    }
    for (const char* p : kProvs) if (t == p) return t;
    for (const auto& a : kProvAlias) {
        std::string low = t;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (low == a.first) return a.second;
    }
    return "";
}

// 城市名剥 市/区 后缀('深圳市'→'深圳'), 对齐台表 city 锚点
static std::string norm_city(std::string t)
{
    if (t.empty()) return "";
    while (t.size() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    for (const char* suf : {"市", "区"}) {
        size_t n = strlen(suf);
        if (t.size() > n && t.compare(t.size() - n, n, suf) == 0) {
            t.erase(t.size() - n);
            break;
        }
    }
    return t;
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
                C_AMBER, C_BORDER, C_TRACK,
                C_OK, C_WARN, C_DANGER;

static void snap_theme()
{
    const theme::Palette& T = theme::cur();
    C_BG = T.page;      // 屏底随主题
    C_CARD = T.card[1];  C_CARD_ON = T.press;  C_ACCENT = T.accent;
    C_TEXT = T.text_hi;  C_TEXT2 = T.text_dim; C_AMBER = T.accent;
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

// ── oneapp 同款列表(2026-11-15) ────────────────────────────────────────
// 行不走对象: 单容器绘制事件直绘(VU 条带同技法)。204 台一行一个对象会
// 压垮内部 RAM(碎片化惯犯); 直绘只有容器+撑高 spacer 两个对象。
// 交互: 点台=播/停(oneapp Enter 语义), 长按=收藏/取消(oneapp 5 键)。
static constexpr int ROW_H = 56;
static std::mutex s_loc_mu;   // _loc_prov/_loc_city 锁(worker 写, LVGL 读)

// ── 分组页签(oneapp radio_tabs: ★ 收藏在最前, 后接四组) ───────────────
void AppRadio::_rebuildTabs()
{
    if (!_tabs) return;
    lv_obj_clean(_tabs);
    for (auto*& b : _tab_btns) b = nullptr;
    int n = 1 + (int)s_groups.size();
    const int cap = (int)(sizeof(_tab_btns) / sizeof(_tab_btns[0]));
    if (n > cap) n = cap;
    int x = 0;
    for (int i = 0; i < n; i++) {
        bool sel = (i == _radio_grp);
        lv_obj_t* b = lv_obj_create(_tabs);
        _tab_btns[i] = b;
        lv_obj_set_size(b, LV_SIZE_CONTENT, 46);
        lv_obj_set_style_radius(b, 23, 0);
        lv_obj_set_style_pad_left(b, 22, 0);
        lv_obj_set_style_pad_right(b, 22, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(sel ? C_CARD_ON : C_BG), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(sel ? C_ACCENT : C_TRACK), 0);
        lv_obj_set_style_border_opa(b, sel ? LV_OPA_70 : LV_OPA_40, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), LV_STATE_PRESSED);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(b, (void*)(intptr_t)i);
        lv_obj_add_event_cb(b, _tab_cb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_obj_set_style_text_font(l, zh_font_md(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(sel ? C_TEXT : C_TEXT2), 0);
        lv_label_set_text(l, i == 0 ? "★" : s_groups[i - 1].c_str());
        lv_obj_center(l);
        lv_obj_update_layout(b);
        lv_obj_set_pos(b, x, 0);
        x += lv_obj_get_width(b) + 14;
    }
}

void AppRadio::_setGroup(int g)
{
    if (g == _radio_grp) return;
    int n = 1 + (int)s_groups.size();
    if (g < 0 || g >= n) return;
    _radio_grp = g;
    _rebuildTabs();
    _rebuildRows();
    _scrollToPlaying();   // 播放中的台在本组则滚到可见
}

// ── 当前组行序(_rows = 绝对下标序列) ──────────────────────────────────
void AppRadio::_rebuildRows()
{
    if (!_list) return;
    _rows.clear();
    if (auto tbl = st_snapshot()) {
        if (_radio_grp == 0) {
            // ★ 收藏组: key 顺序, 缺台剔除(oneapp radio_list 同款)
            for (const auto& k : _favs)
                for (size_t i = 0; i < tbl->size(); i++)
                    if ((*tbl)[i].path == k) { _rows.push_back((int)i); break; }
        } else {
            const std::string& g = s_groups[_radio_grp - 1];
            std::vector<std::pair<int, int>> tmp;   // (本地序, 绝对下标)
            for (size_t i = 0; i < tbl->size(); i++) {
                if ((*tbl)[i].grp != g) continue;
                tmp.push_back({_localRank((int)i), (int)i});
            }
            // 稳定排序: 同城>同省>其他, 组内保原序(oneapp local_rank 同款)
            std::stable_sort(tmp.begin(), tmp.end(),
                [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                    return a.first < b.first;
                });
            for (auto& p : tmp) _rows.push_back(p.second);
        }
    }
    if (_list_spacer)
        lv_obj_set_size(_list_spacer, 1, (int)_rows.size() * ROW_H);
    lv_obj_invalidate(_list);
}

void AppRadio::_scrollToPlaying()
{
    if (!_list) return;
    int w = _want_station.load();
    if (w < 0) return;
    for (size_t i = 0; i < _rows.size(); i++) {
        if (_rows[i] != w) continue;
        int y = (int)i * ROW_H;
        int cur = (int)lv_obj_get_scroll_y(_list);
        int h   = lv_obj_get_height(_list);
        if (y < cur || y + ROW_H > cur + h)   // 不可见才滚, 居中
            lv_obj_scroll_to_y(_list, y - h / 2 + ROW_H / 2, LV_ANIM_ON);
        break;
    }
}

int AppRadio::_rowAt(int py) const
{
    if (!_list || _rows.empty()) return -1;
    lv_area_t a;
    lv_obj_get_coords(_list, &a);
    int rel = py - a.y1 + (int)lv_obj_get_scroll_y(_list);
    if (rel < 0) return -1;
    int row = rel / ROW_H;
    return row < (int)_rows.size() ? row : -1;
}

// 列表直绘(oneapp draw_radio 同构): ▶/★ 标记 + 台名 + 右列「本」徽标/FM 频点
void AppRadio::_list_draw_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    auto tbl = st_snapshot();
    if (!tbl || !app) return;
    const int scroll_y = (int)lv_obj_get_scroll_y(obj);
    const int w = lv_obj_get_width(obj);
    int first = scroll_y / ROW_H;
    if (first < 0) first = 0;
    int vis = lv_obj_get_height(obj) / ROW_H + 2;
    int want = app->_want_station.load();

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    char buf[24];

    for (int i = first; i < first + vis && i < (int)app->_rows.size(); i++) {
        int abs = app->_rows[i];
        const StationDyn& st = (*tbl)[abs];
        int y = i * ROW_H - scroll_y;                       // 对象内坐标
        bool on  = (want >= 0 && abs == want);
        bool fav = app->_favHas(st.path);
        int  rank = app->_localRank(abs);

        if (on) {   // 播放行: track 底 + 左侧 accent 竖条(oneapp 光标行同款)
            rd.bg_opa = LV_OPA_70;
            rd.bg_color = lv_color_hex(C_CARD_ON);
            rd.radius = 12;
            rd.border_width = 0;
            lv_area_t ra{oc.x1, oc.y1 + y, oc.x1 + w - 1, oc.y1 + y + ROW_H - 2};
            lv_draw_rect(layer, &rd, &ra);
            rd.bg_opa = LV_OPA_COVER;
            rd.bg_color = lv_color_hex(C_ACCENT);
            rd.radius = 2;
            lv_area_t rb{oc.x1, oc.y1 + y, oc.x1 + 2, oc.y1 + y + ROW_H - 2};
            lv_draw_rect(layer, &rd, &rb);
        }

        int tx = 16;
        ld.opa = LV_OPA_COVER;
        if (on) {          // ▶ 播放标记(oneapp 三角; FA PLAY 同形)
            ld.font = &lv_font_montserrat_24;
            ld.text = LV_SYMBOL_PLAY;
            ld.color = lv_color_hex(C_ACCENT);
            lv_area_t ta{oc.x1 + tx, oc.y1 + y + 16, oc.x1 + tx + 30, oc.y1 + y + ROW_H - 2};
            lv_draw_label(layer, &ld, &ta);
            tx = 56;
        } else if (fav) {  // ★ 收藏标记
            ld.font = zh_font_md();
            ld.text = "★";
            ld.color = lv_color_hex(C_ACCENT);
            lv_area_t ta{oc.x1 + tx, oc.y1 + y + 13, oc.x1 + tx + 30, oc.y1 + y + ROW_H - 2};
            lv_draw_label(layer, &ld, &ta);
            tx = 56;
        }

        // 台名: 播放/收藏行提亮, 其余 soft(oneapp name_col 同款)
        ld.font = zh_font_lg();
        ld.text = st.name.c_str();
        ld.color = lv_color_hex(on || fav ? C_TEXT : C_TEXT2);
        int name_w = w - tx - 100;
        if (name_w > 40) {
            lv_area_t ta{oc.x1 + tx, oc.y1 + y + 10, oc.x1 + tx + name_w, oc.y1 + y + ROW_H - 2};
            lv_draw_label(layer, &ld, &ta);
        }

        // 右列: 本地徽标「本」accent > FM 频点 dim(oneapp 同款互换)
        if (rank < 2) {
            ld.font = zh_font_md();
            ld.text = "本";
            ld.color = lv_color_hex(C_ACCENT);
        } else if (st.freq > 0) {
            snprintf(buf, sizeof(buf), "FM%d.%d", st.freq / 10, st.freq % 10);
            ld.font = zh_font_md();
            ld.text = buf;
            ld.color = lv_color_hex(C_TEXT2);
        } else {
            continue;
        }
        lv_point_t sz;
        lv_txt_get_size(&sz, ld.text, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t ta{oc.x1 + w - 14 - sz.x, oc.y1 + y + 14, oc.x1 + w - 14, oc.y1 + y + ROW_H - 2};
        lv_draw_label(layer, &ld, &ta);
    }

    // 空收藏组提示(oneapp 同款文案变体)
    if (app->_rows.empty() && app->_radio_grp == 0) {
        int cx = w / 2;
        ld.font = zh_font_lg();
        ld.text = "还没有收藏";
        ld.color = lv_color_hex(C_TEXT2);
        lv_point_t sz;
        lv_txt_get_size(&sz, ld.text, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t t1{oc.x1 + cx - sz.x / 2, oc.y1 + 170, oc.x1 + cx + sz.x / 2, oc.y1 + 214};
        lv_draw_label(layer, &ld, &t1);
        ld.font = zh_font_sm();
        ld.text = "切到电台分组, 长按喜欢的电台收藏";
        lv_txt_get_size(&sz, ld.text, ld.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t t2{oc.x1 + cx - sz.x / 2, oc.y1 + 232, oc.x1 + cx + sz.x / 2, oc.y1 + 262};
        lv_draw_label(layer, &ld, &t2);
    }
}

// 点台=播/停, 长按=收藏(oneapp Enter/5 键的触摸对应)
void AppRadio::_list_event_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    lv_indev_t* ind = lv_indev_get_act();
    if (!ind) return;
    lv_point_t pt;
    lv_indev_get_point(ind, &pt);
    int row = app->_rowAt(pt.y);
    if (row < 0) return;
    if (code == LV_EVENT_LONG_PRESSED) {
        app->_favToggle(app->_rows[row]);
        return;
    }
    int abs = app->_rows[row];
    if (app->_want_station.load() >= 0 && abs == app->_want_station.load()) {
        app->_stopPlayback();   // oneapp Enter 语义: 再点播放中的台 = 停
    } else {
        app->_play(abs);
    }
}

void AppRadio::_tab_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    app->_setGroup((int)(intptr_t)lv_obj_get_user_data(
        static_cast<lv_obj_t*>(lv_event_get_target(e))));
}

// ── 收藏(NVS radio_favs, 逗号分隔 key 表; oneapp radio_fav 同款) ──────
void AppRadio::_favLoad()
{
    _favs.clear();
    std::string v = GetHAL()->getConfig("radio_favs", "");
    size_t p = 0;
    while (p < v.size()) {
        size_t c = v.find(',', p);
        if (c == std::string::npos) c = v.size();
        if (c > p) _favs.push_back(v.substr(p, c - p));
        p = c + 1;
    }
}

void AppRadio::_favSave()
{
    std::string v;
    for (size_t i = 0; i < _favs.size(); i++) {
        if (i) v += ',';
        v += _favs[i];
    }
    GetHAL()->setConfig("radio_favs", v);
}

bool AppRadio::_favHas(const std::string& key) const
{
    return std::find(_favs.begin(), _favs.end(), key) != _favs.end();
}

void AppRadio::_favToggle(int idx)
{
    StationDyn st;
    if (!st_at(idx, st)) return;
    auto it = std::find(_favs.begin(), _favs.end(), st.path);
    if (it != _favs.end()) {
        _favs.erase(it);
        _showToast("已取消收藏");
    } else {
        _favs.push_back(st.path);
        _showToast(("已收藏 · " + st.name).c_str());
    }
    _favSave();
    if (_radio_grp == 0) _rebuildRows();   // ★ 组成员变了
    else lv_obj_invalidate(_list);         // 星标即时显隐
}

// ── IP 归属地(oneapp radio_local 同款) ────────────────────────────────
void AppRadio::_locLoad()
{
    std::lock_guard<std::mutex> g(s_loc_mu);
    _loc_prov = _loc_city = "";
    std::string v = GetHAL()->getConfig("radio_loc", "");
    auto bar = v.find('|');
    if (bar != std::string::npos) {
        _loc_prov = v.substr(0, bar);
        _loc_city = v.substr(bar + 1);
        return;
    }
    // 无缓存: 种子用主页天气的 IP 定位(只有城市, 同城置顶已够用)
    std::string c = norm_city(GetHAL()->getConfig("wx_city", ""));
    if (!c.empty()) _loc_city = c;
}

// worker: ip-api 中文 → ip.sb(英文, prov 走别名表) 兜底 → NVS radio_loc
void AppRadio::_fetchLocation()
{
    for (int src = 0; src < 2; src++) {
        auto r = GetHAL()->httpGet(src == 0 ? open_meteo::LOCATE_URL_1
                                            : open_meteo::LOCATE_URL_2);
        if (!r.ok) continue;
        try {
            auto j = PsramJson::parse(r.body);
            if (src == 0 && j.value("status", std::string()) != "success") continue;
            std::string prov = norm_prov(
                j.value(src == 0 ? "regionName" : "region", std::string()));
            std::string city = norm_city(j.value("city", std::string()));
            if (prov.empty() && city.empty()) continue;
            {
                std::lock_guard<std::mutex> g(s_loc_mu);
                _loc_prov = prov;
                _loc_city = city;
            }
            std::string v = prov + "|" + city;
            if (GetHAL()->getConfig("radio_loc", "") != v)
                GetHAL()->setConfig("radio_loc", v);
            _loc_dirty.store(true);   // onRunning(LVGL 线程)重建列表+徽标
            mclog::tagInfo(TAG, "loc: {}·{}", prov, city);
        } catch (const std::exception& e) {
            mclog::tagWarn(TAG, "loc parse fail: {}", e.what());
        }
        return;   // 任一源成功/解析完即止
    }
}

std::string AppRadio::_locText() const
{
    std::lock_guard<std::mutex> g(s_loc_mu);
    std::string s = _loc_prov;
    if (!_loc_city.empty()) {
        if (!s.empty()) s += "·";
        s += _loc_city;
    }
    return s;
}

// 本地置顶排序键: 0=同城 1=同省 2=其他(oneapp local_rank 同款)
int AppRadio::_localRank(int idx) const
{
    StationDyn st;
    if (!st_at(idx, st)) return 2;
    std::lock_guard<std::mutex> g(s_loc_mu);
    if (!_loc_city.empty() && st.city == _loc_city) return 0;
    if (!_loc_prov.empty() && st.prov == _loc_prov) return 1;
    return 2;
}

// ── 动态电台表(relay /radio/stations 下发; 2026-11-15 起带分组/归属地) ─
void AppRadio::_fetchStations()
{
    // 后台线程: 只碰 HTTP 与原子量, 不触 LVGL。失败静默(内置表兜底)。
    auto r = GetHAL()->httpGet(std::string(kRelayBase) + "stations");
    if (r.status != 200) return;
    try {
        auto j = PsramJson::parse(r.body);
        auto arr = j.at("stations");
        if (!arr.is_array() || arr.empty()) return;
        auto v = std::make_shared<std::vector<StationDyn>>();
        for (auto& e : arr) {
            StationDyn st;
            st.path = e.value("key", std::string());
            st.name = e.value("name", std::string());
            st.freq = e.value("freq", 0);
            st.grp  = e.value("grp", std::string());
            st.prov = e.value("prov", std::string());
            st.city = e.value("city", std::string());
            if (!st.path.empty() && !st.name.empty())
                v->push_back(std::move(st));
        }
        // 分组顺序表(oneapp GROUPS; 缺失/异常保内置)
        std::vector<std::string> groups = s_groups;
        if (j.contains("groups") && j["groups"].is_array()) {
            std::vector<std::string> g2;
            for (auto& g : j["groups"]) {
                std::string s = g.get<std::string>();
                if (!s.empty()) g2.push_back(s);
            }
            if (g2.size() >= 2 && g2.size() <= 4) groups = g2;
        }
        if (v->empty()) return;
        auto cur = st_snapshot();
        bool same = cur && cur->size() == v->size();
        if (same)
            for (size_t i = 0; i < v->size(); i++)
                if ((*cur)[i].path != (*v)[i].path) { same = false; break; }
        bool groups_same = groups.size() == s_groups.size() &&
                           std::equal(groups.begin(), groups.end(), s_groups.begin());
        if (same && groups_same) return;   // 表没变, 不折腾
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
        s_groups = groups;
        _stations_dirty.store(true);   // onRunning(LVGL 线程)重建页签+列表
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
    _favLoad();   // 收藏 + 归属地先同步载入(NVS), 首帧即有分组/排序
    _locLoad();

    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── 左: 头部 + 分组页签 + 电台列表(oneapp draw_radio 同构) ──
    lv_obj_t* hdr = lv_label_create(_scr);
    lv_label_set_text(hdr, "电台");
    lv_obj_set_style_text_font(hdr, zh_font_md(), 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(hdr, 40, 16);

    _hdr_count = lv_label_create(_scr);
    lv_obj_set_style_text_font(_hdr_count, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_hdr_count, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(_hdr_count, 96, 26);

    _hdr_loc = lv_label_create(_scr);   // 归属地徽标, 右缘对齐列表右边界 x=680
    lv_obj_set_style_text_font(_hdr_loc, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_hdr_loc, lv_color_hex(C_ACCENT), 0);
    lv_obj_align(_hdr_loc, LV_ALIGN_TOP_RIGHT, -600, 26);

    _tabs = lv_obj_create(_scr);
    lv_obj_set_pos(_tabs, 40, 58);
    lv_obj_set_size(_tabs, 640, 48);
    lv_obj_set_style_bg_opa(_tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tabs, 0, 0);
    lv_obj_set_style_pad_all(_tabs, 0, 0);
    lv_obj_clear_flag(_tabs, LV_OBJ_FLAG_SCROLLABLE);

    // 列表(直绘, 零行对象): 高度收到 y640, 底部 650+ 留给上拨手势起手区
    // (滚动容器会吃掉其区域内的手势, 2026-11-02 教训)。
    _list = lv_obj_create(_scr);
    lv_obj_set_pos(_list, 40, 112);
    lv_obj_set_size(_list, 640, 528);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_clip_corner(_list, true, 0);   // 直绘内容按容器裁剪(overflow:hidden)
    lv_obj_set_scrollbar_mode(_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(_list, LV_DIR_VER);
    lv_obj_add_event_cb(_list, _list_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(_list, _list_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_list, _list_event_cb, LV_EVENT_LONG_PRESSED, this);
    _list_spacer = lv_obj_create(_list);   // 撑起内容高度(直绘无行对象)
    lv_obj_set_style_bg_opa(_list_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list_spacer, 0, 0);
    lv_obj_clear_flag(_list_spacer, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    _rebuildTabs();
    _rebuildRows();
    lv_label_set_text_fmt(_hdr_count, "· %d 台", st_count());
    {
        std::string loc = _locText();
        if (loc.empty()) lv_obj_add_flag(_hdr_loc, LV_OBJ_FLAG_HIDDEN);
        else lv_label_set_text(_hdr_loc, ("本:" + loc).c_str());
    }

    // ── 右: 播放面板(oneapp VU 卡片同款, 主题 card 底 + 描边) ──
    lv_obj_t* panel = lv_obj_create(_scr);
    lv_obj_set_pos(panel, 720, 28);
    lv_obj_set_size(panel, 520, 664);
    lv_obj_set_style_bg_color(panel, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    _status_lbl = lv_label_create(panel);   // 状态行(未播放/连接中/播放中/失败)
    lv_label_set_text(_status_lbl, "未播放");
    lv_obj_set_style_text_font(_status_lbl, zh_font_md(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(_status_lbl, 24, 18);

    // 输出 chip(右上, oneapp「输出:xxx」同款文案): 点开输出选择弹窗
    _out_chip = lv_obj_create(panel);
    lv_obj_set_pos(_out_chip, 330, 14);
    lv_obj_set_size(_out_chip, 166, 40);
    lv_obj_set_style_radius(_out_chip, 20, 0);
    lv_obj_set_style_bg_color(_out_chip, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_bg_opa(_out_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_out_chip, 1, 0);
    lv_obj_set_style_border_color(_out_chip, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_opa(_out_chip, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(_out_chip, 0, 0);
    lv_obj_clear_flag(_out_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_out_chip, LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t* tl = lv_label_create(_out_chip);   // 文字部分(_refreshOutputUi 刷新)
        lv_obj_set_style_text_font(tl, zh_font_md(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT), 0);
        lv_obj_center(tl);
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
    lv_obj_set_pos(_now_name, 24, 64);
    lv_label_set_long_mode(_now_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(_now_name, 472);

    // ①ICY 正在播放歌名(20px 副行; 国内台 HLS 无 ICY = 恒空不占眼)
    _now_title = lv_label_create(panel);
    lv_label_set_text(_now_title, "");
    lv_obj_set_style_text_font(_now_title, zh_font_sm(), 0);
    lv_obj_set_style_text_color(_now_title, lv_color_hex(C_TEXT2), 0);
    lv_label_set_long_mode(_now_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_now_title, 472);
    lv_obj_set_pos(_now_title, 24, 104);
    if (uint32_t sq = s_np_seq.load()) {   // 后台播放中重进: 恢复已取到的歌名
        _np_seen = sq;
        lv_label_set_text(_now_title, s_np_title);
    }

    // ── 电平图(oneapp VU 面板同款): 单条带对象绘制事件直绘 ──
    // 20 根条: track 底 + 琥珀填充 + 峰值帽; 面板内 (24,146) 472×300
    _vu_strip = lv_obj_create(panel);
    lv_obj_set_pos(_vu_strip, 24, 146);
    lv_obj_set_size(_vu_strip, 472, 300);
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
    lv_obj_t* prev = mk_btn(105, 482, 70, &lv_font_montserrat_28, LV_SYMBOL_PREV, nullptr);
    lv_obj_add_event_cb(prev, _prevClick_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* play = mk_btn(205, 464, 110, &lv_font_montserrat_32, LV_SYMBOL_PLAY, &_play_icon);
    lv_obj_add_event_cb(play, _playClick_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* next = mk_btn(345, 482, 70, &lv_font_montserrat_28, LV_SYMBOL_NEXT, nullptr);
    lv_obj_add_event_cb(next, _nextClick_cb, LV_EVENT_CLICKED, this);

    lv_obj_t* vl = lv_label_create(panel);
    lv_label_set_text(vl, "音量");
    lv_obj_set_style_text_font(vl, zh_font_md(), 0);
    lv_obj_set_style_text_color(vl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(vl, 24, 612);

    int vol = xiaozhi_get_speaker_volume();
    _vol_slider = lv_slider_create(panel);
    lv_obj_set_size(_vol_slider, 300, 14);
    lv_obj_set_pos(_vol_slider, 100, 622);
    lv_slider_set_range(_vol_slider, 0, 100);
    lv_slider_set_value(_vol_slider, vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_vol_slider, lv_color_hex(C_ACCENT), LV_PART_KNOB);
    lv_obj_add_event_cb(_vol_slider, _vol_cb, LV_EVENT_VALUE_CHANGED, this);

    _vol_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(_vol_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_vol_lbl, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(_vol_lbl, 416, 616);
    lv_label_set_text_fmt(_vol_lbl, "%d%%", vol);

    // OSD 提示(oneapp osd 同款: 已收藏·xx / 输出:xx, 1.6s 自隐; onRunning 收)
    _toast = lv_obj_create(panel);
    lv_obj_set_size(_toast, 400, 60);
    lv_obj_align(_toast, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_set_style_bg_color(_toast, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_bg_opa(_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_width(_toast, 1, 0);
    lv_obj_set_style_border_color(_toast, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_opa(_toast, LV_OPA_70, 0);
    lv_obj_set_style_radius(_toast, 30, 0);
    lv_obj_set_style_pad_all(_toast, 0, 0);
    lv_obj_clear_flag(_toast, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    {
        lv_obj_t* tl = lv_label_create(_toast);
        lv_obj_set_style_text_font(tl, zh_font_md(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(C_TEXT), 0);
        lv_label_set_text(tl, "");
        lv_obj_center(tl);
        lv_obj_set_user_data(_toast, tl);
    }
    lv_obj_add_flag(_toast, LV_OBJ_FLAG_HIDDEN);

    // 手势 alive 旗标重建 (app_stocks 同款): onClose 里 *_alive=false 后,
    // 旧 shared_ptr 永久作废 —— 不重建的话第二次进收音机上拨手势在 alive
    // 检查处静默失效 (2026-10-29 实锤: 后台播放重进后无法上拨回主页)
    _alive = std::make_shared<bool>(true);
    _installSwipeGesture();
    lv_screen_load(_scr);

    // 外部输出在播中重进: 音量滑条同步外部设备音量(异步拉取, onRunning 应用)
    if (_output_sel > 0 && _ext_on.load()) _extVolSync();

    // 动态电台表 + 归属地: 进 app 后台各拉一次(relay 增删台对终端零改动)
    GetHAL()->tryRunDetached([this]() { _fetchStations(); });
    GetHAL()->tryRunDetached([this]() { _fetchLocation(); });

    // 几何版本标记: 从串口确证当前固件的布局参数(排查改了没生效)
    mclog::tagInfo(TAG, "ui-geom: oneapp list=40,112,640x528 row56 tabs_y=58 "
                        "panel=720,28,520x664 vu=24,146,472x300 btn_y=464 toast_y=250");

    _startTask();
}

void AppRadio::_showToast(const char* msg)
{
    if (!_toast) return;
    if (auto* tl = static_cast<lv_obj_t*>(lv_obj_get_user_data(_toast)))
        lv_label_set_text(tl, msg);
    lv_obj_clear_flag(_toast, LV_OBJ_FLAG_HIDDEN);
    _toast_at = lv_tick_get();
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

    // 动态表更新落地: 后台线程拉到新表后在这里(LVGL 线程)重建页签+列表
    if (_stations_dirty.exchange(false)) {
        _rebuildTabs();
        _rebuildRows();
        if (_hdr_count) lv_label_set_text_fmt(_hdr_count, "· %d 台", st_count());
        _ui_station = -1;              // 表变了, 强制整套刷新(高亮/台名)
        _refreshStationUi();
    }

    // 归属地更新落地: 重建列表(本地置顶) + 徽标
    if (_loc_dirty.exchange(false)) {
        std::string loc = _locText();
        if (_hdr_loc) {
            if (loc.empty()) lv_obj_add_flag(_hdr_loc, LV_OBJ_FLAG_HIDDEN);
            else {
                lv_label_set_text(_hdr_loc, ("本:" + loc).c_str());
                lv_obj_clear_flag(_hdr_loc, LV_OBJ_FLAG_HIDDEN);
            }
        }
        _rebuildRows();
    }

    // OSD 提示 1.6s 自隐(oneapp osd 同款)
    if (_toast && _toast_at != 0 && lv_tick_elaps(_toast_at) > 1600) {
        lv_obj_add_flag(_toast, LV_OBJ_FLAG_HIDDEN);
        _toast_at = 0;
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
        float target = 4 + v * 2.74f;                 // 4..278px(条带 300 高)
        if (target > 278) target = 278;
        float k = target > _vu_cur[i] ? 0.45f : 0.10f + (i % 5) * 0.04f;
        _vu_cur[i] += (target - _vu_cur[i]) * k;
        int h = (int)(_vu_cur[i] + 0.5f);
        if (h < 3) h = 3;
        if (h > 278) h = 278;
        // ④峰值帽: 超峰即记, 8fps 每帧 ×0.92 缓降, 永不低于条顶(0.92^8≈0.5/s)
        float pk = s_vu_pk[i];
        if ((float)h >= pk) pk = (float)h;
        else                pk *= 0.92f;
        if (pk < (float)h) pk = (float)h;
        int ph = (int)(pk + 0.5f);
        if (ph < h) ph = h;
        if (ph > 278) ph = 278;
        bool pchg = ((uint16_t)ph != s_vu_pk[i]);
        s_vu_pk[i] = (uint16_t)ph;
        if ((uint16_t)h != s_vu_h[i] || pchg) { s_vu_h[i] = h; changed = true; } // 变化才重绘
    }
    if (changed) lv_obj_invalidate(_vu_strip);
}

// 绘制事件(oneapp VU 同构): track 底柱 + 琥珀填充自底向上 + 峰值帽亮线
// 几何: 条带 472×300, 20 根条 宽16 间隙6, x0=19, 条底 y=290, 最高 278
void AppRadio::_vu_draw_cb(lv_event_t* e)
{
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* layer = lv_event_get_layer(e);
    lv_area_t oc;
    lv_obj_get_coords(obj, &oc);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 2;
    for (int i = 0; i < 20; i++) {
        int x = 19 + i * 22;
        // track 底柱(oneapp: 未跳动的柱也占位)
        dsc.bg_opa = LV_OPA_60;
        dsc.bg_color = lv_color_hex(C_TRACK);
        lv_area_t ta;
        ta.x1 = oc.x1 + x;
        ta.x2 = ta.x1 + 15;
        ta.y1 = oc.y1 + 12;
        ta.y2 = oc.y1 + 290;
        lv_draw_rect(layer, &dsc, &ta);
        // 琥珀填充(自底向上)
        int h = s_vu_h[i];
        dsc.bg_opa = LV_OPA_90;
        dsc.bg_color = lv_color_hex(C_AMBER);
        lv_area_t a;
        a.x1 = oc.x1 + x;
        a.x2 = a.x1 + 15;
        a.y2 = oc.y1 + 290;
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
        a.x1 = oc.x1 + 19 + i * 22;
        a.x2 = a.x1 + 15;
        a.y2 = oc.y1 + 290 - ph;
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
    if (_now_name) lv_label_set_text(_now_name, st.name.c_str());
    if (_list) lv_obj_invalidate(_list);   // ▶ 高亮跟随(直绘)
    _scrollToPlaying();
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

void AppRadio::_stopPlayback()
{
    if (_ext_on.load()) _extStop();   // 外部: 停投(流任务见 ext_on=false+want<0 置待机)
    _want_station.store(-1);          // 暂停(本机断流)
#ifdef PLATFORM_BUILD_WEB
    if (_output_sel == 0) webui::web_media_radio(nullptr, false);
#endif
}

void AppRadio::_playClick_cb(lv_event_t* e)
{
    auto* app = static_cast<AppRadio*>(lv_event_get_user_data(e));
    int st = app->_state.load();
    if (st == 1 || st == 2) app->_stopPlayback();
    else                    app->_play(app->_last_station); // 播放/重试
}

// 上一台/下一台: 当前组列表内循环(oneapp ↑↓ 移动光标同语义)
void AppRadio::_stepInGroup(int dir)
{
    int n = (int)_rows.size();
    if (n <= 0) return;
    int w = _want_station.load();
    if (w < 0) w = _last_station;
    int pos = 0;
    for (int i = 0; i < n; i++)
        if (_rows[i] == w) { pos = i; break; }
    _play(_rows[(pos + dir + n) % n]);
}

void AppRadio::_prevClick_cb(lv_event_t* e)
{
    static_cast<AppRadio*>(lv_event_get_user_data(e))->_stepInGroup(-1);
}

void AppRadio::_nextClick_cb(lv_event_t* e)
{
    static_cast<AppRadio*>(lv_event_get_user_data(e))->_stepInGroup(1);
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
        lv_label_set_text_fmt(tl, "输出:%s", kOutputs[_output_sel].name);
}

void AppRadio::_setOutput(int i)
{
    if (i < 0 || i >= kOutputCount || i == _output_sel) return;
    _output_sel = i;
    GetHAL()->setConfig("radio_out", std::to_string(i));
    _refreshOutputUi();
    char tb[48];
    snprintf(tb, sizeof(tb), "输出: %s", kOutputs[i].full);
    _showToast(tb);   // oneapp O 键 OSD 同款
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
