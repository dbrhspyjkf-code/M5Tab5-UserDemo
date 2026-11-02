/*
 * Web (Emscripten) platform entry — mirrors platforms/desktop/main.cpp.
 *
 * PROXY_TO_PTHREAD: 整个 main() 跑在 pthread worker 上, 阻塞 (delay/同步
 * XHR/启动动画 while 循环) 均合法; 浏览器主线程只做 canvas 绘制与输入
 * 采集, 与 worker 通过共享 wasm heap 交换帧和事件 (见 web_display.cpp)。
 */
#include "hal/hal_web.h"
#include <app.h>
#include <hal/hal.h>
#include <cstdlib>
#include <memory>

int main()
{
    // 浏览器无闲置/熄屏概念: 关掉屏保 worker (与桌面 TAB5_SS_EN 惯例一致)
    setenv("TAB5_SS_EN", "0", 1);
    setenv("TAB5_IDLE_MS", "0", 1);

    app::InitCallback_t callback;
    callback.onHalInjection = []() {
        hal::Inject(std::make_unique<HalWeb>());
    };

    app::Init(callback);
    while (!app::IsDone()) {
        app::Update();
        webui::publishFrame();  // flush 累积的脏区发布给浏览器主线程
        webui::sleepMs(5);
    }
    app::Destroy();
    return 0;
}
