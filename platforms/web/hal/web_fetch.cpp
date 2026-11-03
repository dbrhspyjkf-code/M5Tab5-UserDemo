/*
 * web_fetch — 二进制拉取通道 (S4 终案 v2, 2026-11-03)
 *
 * 通信两端:
 *   请求方 = tryRunDetached 的 pthread (图库缩略图/大图) — 填命令区 + 自旋
 *   服务方 = 浏览器页面 (shell.js) — 轮询命令区 → 原生 fetch → 直写 wasm 堆
 *
 * 为什么命令区是 C++ 静态全局: pthread 构建下 wasm 线性内存是 shared
 * WebAssembly.Memory (SAB), 页面持有同一 buffer 引用 → 页面可直读直写,
 * 全程零 worker 消息。前版教训: 跑 main/LVGL 的 em-pthread worker 主循环
 * 是连续 JS + futex 阻塞式 sleep, 事件循环永不泵动 —— 页面→worker 的
 * postMessage 永远不派发 (页→worker 消息通道是死路; worker→page 只要
 * 对方泵, 反而一直通, uart/layout 均如此)。
 *
 * 命令区布局 (字节偏移; shell.js 按此访问, 改动须两侧同步):
 *   +0   want   u32 请求号 (请求方发布, 1 起单调递增)
 *   +4   status i32 0=处理中 1=ok 2=err 3=已弃单(超时) 4=页面写入中
 *   +8   len    i32 回包字节数
 *   +12  out    u32 wasm 堆目标地址 (wasm32 指针)
 *   +16  cap    i32 目标缓冲容量
 *   +20  rsv
 *   +24  url[192] 同源网关路径
 *
 * 时序协议 (防迟到回包写脏已释放缓冲):
 *   页面 fetch 完成 → CAS status 0→4 抢写权; 失败说明请求方已弃单
 *   (status=3), 缓冲可能已释放, 绝不落笔。请求方超时走 CAS 0→3:
 *   抢到=干净弃单; 抢不到=页面正在写 (status=4), 继续等结果直接返回。
 */
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <mooncake_log.h>
#include "hal/hal.h"
#include "hal/hal_web.h"

static const std::string _tag = "binfetch";

struct BinCmd {
    std::atomic<uint32_t> want;    // +0
    std::atomic<int32_t>  status;  // +4
    std::atomic<int32_t>  len;     // +8
    std::atomic<uint32_t> out;     // +12
    std::atomic<int32_t>  cap;     // +16
    uint32_t              rsv;     // +20
    char                  url[192];// +24
};                                 // 共 216 字节
static BinCmd g_cmd;               // 静态存储零初始化

static constexpr int32_t ST_PENDING = 0, ST_OK = 1, ST_ERR = 2, ST_DROP = 3, ST_CLAIM = 4;

// init 时向页面广播: wasm 堆 (SAB) 引用 + 命令区地址。
// 在 em-pthread (main 所在) 执行, postMessage 到 parent=页面, 即达。
EM_JS(void, bin_heap_announce_js, (int cmd_ptr), {
    postMessage({__tab5heap: {sab: wasmMemory.buffer, cmd: cmd_ptr}});
})

extern "C" int web_fetch_bin(const char* url, uint8_t* out, int cap)
{
    if (!url || !out || cap <= 0) return -1;
    // 主运行线程 (LVGL) 调用会冻结渲染整个 fetch 时长 —— 现有调用方都在
    // pthread, 出现即说明有新调用点漏了 tryRunDetached, 拒绝并留痕
    if (emscripten_is_main_runtime_thread()) {
        mclog::tagWarn(_tag, "reject main-runtime call: {}", url);
        return -2;
    }

    // 设备绝对 URL → 同源网关路径 (页面 fetch 必须同源 + 带会话 cookie)
    const std::string ru = webui::gateway_url(url);
    if (ru.size() >= sizeof(g_cmd.url)) {
        mclog::tagWarn(_tag, "url too long: {}", ru);
        return -1;
    }

    // 命令区单槽 —— 图库缩略图/大图/预取线程可能并发, 串行化
    static std::mutex s_mu;
    std::lock_guard<std::mutex> lk(s_mu);

    const uint32_t my = g_cmd.want.load(std::memory_order_relaxed) + 1;
    std::memcpy(g_cmd.url, ru.c_str(), ru.size() + 1);
    g_cmd.out.store((uint32_t)(uintptr_t)out, std::memory_order_relaxed);
    g_cmd.cap.store(cap, std::memory_order_relaxed);
    g_cmd.len.store(0, std::memory_order_relaxed);
    g_cmd.status.store(ST_PENDING, std::memory_order_relaxed);
    g_cmd.want.store(my, std::memory_order_release);   // 发布 (最后写)

    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const int32_t st = g_cmd.status.load(std::memory_order_acquire);
        if (st == ST_OK) return g_cmd.len.load();
        if (st == ST_ERR) return -2;
        if (st == ST_PENDING) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) {
                int32_t expected = ST_PENDING;
                if (g_cmd.status.compare_exchange_strong(expected, ST_DROP)) {
                    mclog::tagWarn(_tag, "timeout: {}", ru);
                    return -1;   // 干净弃单 (页面写入权被本 CAS 挡掉)
                }
                // 页面已在写 (status=4): 等它收尾按真实结果返回
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

namespace webui {

// hal_web.cpp 在 lvgl_init 里调用 (wasmMemory 已就绪)
void web_fetch_bin_init()
{
    bin_heap_announce_js((int)(uintptr_t)&g_cmd);
    mclog::tagInfo(_tag, "bin fetch channel ready, cmd@0x{:x}", (uintptr_t)&g_cmd);
}

}  // namespace webui
