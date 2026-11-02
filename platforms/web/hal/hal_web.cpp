/*
 * SPDX-License-Identifier: MIT
 */
#include "hal_web.h"
#include <mooncake_log.h>
#include <lvgl.h>
#include <emscripten.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

static const std::string _tag = "hal";

/* -------------------------------------------------------------------------- */
/*                          JS interop (worker 线程)                          */
/* -------------------------------------------------------------------------- */

// 同步 XHR: 仅在 worker 线程合法 (PROXY_TO_PTHREAD 下 main 即 worker)。
// 语义对齐桌面 curl 版: 超时/网络错误 → status=0, ok=false, body 空。
// 返回 HTTP status; body 写入 malloc 缓冲, 由调用方 free。
EM_JS(int, web_xhr, (const char* method, const char* url, const char* headers,
                     const char* body, int bodyLen, int timeoutMs,
                     int* outPtr, int* outLen),
{
    let p = 0, len = 0;
    try {
        const xhr = new XMLHttpRequest();
        xhr.open(UTF8ToString(method), UTF8ToString(url), false);  // sync, worker only
        xhr.responseType = 'arraybuffer';
        xhr.timeout = timeoutMs;
        const hs = UTF8ToString(headers);
        if (hs) {
            for (const line of hs.split('\n')) {
                const i = line.indexOf(':');
                if (i > 0) xhr.setRequestHeader(line.slice(0, i).trim(), line.slice(i + 1).trim());
            }
        }
        if (bodyLen > 0) {
            xhr.send(HEAPU8.subarray(body, body + bodyLen));
        } else {
            xhr.send(null);
        }
        const resp = new Uint8Array(xhr.response || 0);
        len = resp.length;
        if (len) {
            p = Module._malloc(len);
            HEAPU8.set(resp, p);
        }
        setValue(outPtr, p, 'i32');
        setValue(outLen, len, 'i32');
        return xhr.status;
    } catch (e) {
        console.warn('[tab5web] xhr failed:', UTF8ToString(url), e);
        setValue(outPtr, 0, 'i32');
        setValue(outLen, 0, 'i32');
        return 0;
    }
})

namespace webui {

void sleepMs(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t steadyMillis()
{
    static const auto start = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

}  // namespace webui

/* -------------------------------------------------------------------------- */
/*                    URL 改写: 设备侧绝对 URL → 网关相对路径                    */
/* -------------------------------------------------------------------------- */
// 设备 app 直接构造 http://<host>:<port>/...; 浏览器里必须走同源网关:
//   :8766 hermes  :8768 email  :8769 radio  :8123 HA  :8042 reachy  :8900 ios
//   →  /gw/<port><path>
//   qt.gtimg.cn / web.ifzq.gtimg.cn → /gw/ext/qt|ifzq<path> (腾讯行情, CORS 需代理)
static std::string rewrite_url(const std::string& url)
{
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return url;  // 相对路径直通

    size_t host_start = url.find("://") + 3;
    size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos) path_start = url.size();
    std::string hostport = url.substr(host_start, path_start - host_start);
    std::string path = url.substr(path_start);  // 含 '/', 可为空

    // 腾讯行情
    if (hostport.find("qt.gtimg.cn") != std::string::npos) return "/gw/ext/qt" + path;
    if (hostport.find("web.ifzq.gtimg.cn") != std::string::npos) return "/gw/ext/ifzq" + path;

    // 提取端口
    size_t colon = hostport.rfind(':');
    std::string port = (colon != std::string::npos) ? hostport.substr(colon + 1) : "";

    static const char* kGwPorts[] = {"8766", "8768", "8769", "8123", "8042", "8900"};
    for (auto p : kGwPorts) {
        if (port == p) return "/gw/" + port + path;
    }

    mclog::tagWarn(_tag, "unrouted url passes through: {}", url);
    return url;
}

static std::string flatten_headers(const std::vector<std::pair<std::string, std::string>>& headers)
{
    std::string out;
    for (auto& [k, v] : headers) {
        if (v.empty()) continue;
        out += k + ": " + v + "\n";
    }
    return out;
}

static hal::HalBase::HttpResponse_t http_request(
    const std::string& method, const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const char* body, size_t bodyLen, int timeoutMs)
{
    hal::HalBase::HttpResponse_t resp;
    const std::string rw = rewrite_url(url);

    // 同源网关请求默认带 JSON content-type (app 侧 JSON body 为主;
    // 二进制如双色球 JPEG 上行对服务端 raw 读取无影响)
    std::string hdrs = flatten_headers(headers);
    if (bodyLen > 0 && hdrs.find("Content-Type:") == std::string::npos &&
        hdrs.find("content-type:") == std::string::npos) {
        hdrs = "Content-Type: application/json\n" + hdrs;
    }
    // X-Tab5-Web: CSRF 防护标记 (正式网关校验, 非幂等请求必须带)
    if (method != "GET") hdrs += "X-Tab5-Web: 1\n";

    int p = 0, len = 0;
    int status = web_xhr(method.c_str(), rw.c_str(), hdrs.c_str(), body, (int)bodyLen, timeoutMs, &p, &len);
    resp.status = status;
    resp.ok     = (status >= 200 && status < 300);
    if (p) {
        resp.body.assign((const char*)p, (size_t)len);
        free((void*)p);
    }
    return resp;
}

hal::HalBase::HttpResponse_t HalWeb::httpGet(const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    return http_request("GET", url, headers, nullptr, 0, 30000);
}

hal::HalBase::HttpResponse_t HalWeb::httpPost(const std::string& url, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    return http_request("POST", url, headers, body.data(), body.size(), 240000);
}

hal::HalBase::HttpResponse_t HalWeb::httpPut(const std::string& url, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers)
{
    return http_request("PUT", url, headers, body.data(), body.size(), 30000);
}

/* -------------------------------------------------------------------------- */
/*              Persistent config (设备 NVS → 网关 /gw/prefs)                  */
/* -------------------------------------------------------------------------- */

static std::mutex& prefs_mutex()
{
    static std::mutex mu;
    return mu;
}

static std::unordered_map<std::string, std::string>& prefs_cache()
{
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}

std::string HalWeb::getConfig(const std::string& key, const std::string& defaultValue)
{
    std::lock_guard<std::mutex> lk(prefs_mutex());
    auto& cache = prefs_cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    // 惰性拉取单键 (网关 404 = 未设置, 用默认值)
    int p = 0, len = 0;
    std::string path = "/gw/prefs?key=" + key;
    int status = web_xhr("GET", path.c_str(), "", nullptr, 0, 5000, &p, &len);
    std::string value;
    if (status == 200 && p) {
        value.assign((const char*)p, (size_t)len);
        free((void*)p);
    } else if (p) {
        free((void*)p);
    }
    cache[key] = value.empty() ? defaultValue : value;
    return cache[key];
}

void HalWeb::setConfig(const std::string& key, const std::string& value)
{
    {
        std::lock_guard<std::mutex> lk(prefs_mutex());
        prefs_cache()[key] = value;
    }
    std::string body = "{\"key\":\"" + key + "\",\"value\":\"" + value + "\"}";
    int p = 0, len = 0;
    web_xhr("POST", "/gw/prefs", "Content-Type: application/json\n",
            body.c_str(), (int)body.size(), 5000, &p, &len);
    if (p) free((void*)p);
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
void HalWeb::init()
{
    mclog::tagInfo(_tag, "init (web)");
    lvgl_init();
}

void HalWeb::delay(uint32_t ms)
{
    webui::sleepMs(ms);
}

uint32_t HalWeb::millis()
{
    return webui::steadyMillis();
}

int HalWeb::getCpuTemp()
{
    return 45;
}

void HalWeb::setDisplayBrightness(uint8_t brightness)
{
    _current_lcd_brightness = std::clamp((int)brightness, 0, 100);
}

uint8_t HalWeb::getDisplayBrightness()
{
    return _current_lcd_brightness;
}

// web 版「串口监视器」: 日志经 postMessage 桥到浏览器主线程 (window.__tab5uart)
EM_JS(void, uart_bridge, (const char* msg, int len), {
    postMessage({__tab5uart: UTF8ToString(msg, len)});
})

void HalWeb::uartMonitorSend(std::string msg, bool newLine)
{
    uart_bridge(msg.c_str(), (int)msg.size());
}

void HalWeb::setSpeakerVolume(uint8_t volume)
{
    _current_speaker_volume = std::clamp((int)volume, 0, 100);
    mclog::tagInfo(_tag, "set speaker volume: {} (web: no-op)", _current_speaker_volume);
}

uint8_t HalWeb::getSpeakerVolume()
{
    return _current_speaker_volume;
}

/* -------------------------------------------------------------------------- */
/*                                  LVGL init                                 */
/* -------------------------------------------------------------------------- */

namespace webui {
// web_display.cpp 提供: 共享内存布局建立 + 显示/输入驱动注册
void lvgl_display_init();
}

void HalWeb::lvgl_init()
{
    mclog::tagInfo(_tag, "lvgl init (web)");
    lv_init();
    lv_group_set_default(lv_group_create());
    lv_tick_set_cb([]() -> uint32_t { return webui::steadyMillis(); });
    webui::lvgl_display_init();
}

/* -------------------------------------------------------------------------- */
/*                             LVGL 锁 (同桌面)                                */
/* -------------------------------------------------------------------------- */

// 递归锁: mooncake update 持锁调 onOpen, app 内 _buildUi 再拿锁 (同桌面注释)
static std::recursive_mutex _lvgl_mutex;

void HalWeb::lvglLock()
{
    _lvgl_mutex.lock();
}

void HalWeb::lvglUnlock()
{
    _lvgl_mutex.unlock();
}
