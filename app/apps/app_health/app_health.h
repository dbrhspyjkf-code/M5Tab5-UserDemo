#pragma once
/*
 * app_health — 健康 (2026-10-31)。
 *
 * 深色苹果风 (iOS 健康 App 深色模式): 黑底 + #1C1C1E 圆角卡 + 彩色圆角图标
 * + 大数字。数据源 hermes GET :8766/api/health (iPhone 快捷指令 POST 推送,
 * 服务端聚合摘要), 60s 轮询, 11 类指标 2 列卡网格纵向滚动。上拨返回工具页
 * (底缘 y>650 起手守卫, 滚动页与手势共存, app_stocks 同款)。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

class AppHealth : public mooncake::AppAbility {
public:
    AppHealth();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;   // 失焦收尾 + 轮询节流 + 数据应用 (UI 线程)
    void onClose()   override;

private:
    void _buildUi();
    void _refresh();               // 把 worker 缓存刷到 label (UI 线程)
    void _fetch();                 // 起 worker 拉 /api/health (防重入)

    // ── 手势 (上拨退出, app_calendar 同款: GESTURE + 位移兜底双轨) ──
    struct SwipeCtx {
        AppHealth* app;
        std::weak_ptr<bool> alive;
    };
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    // 照 app_calendar/app_stocks: 只发回调不调 close() —— close() 的 GoClose
    // 会与 openApp(工具页) 同拍竞争; 失焦收尾在 onRunning 里检测完成。
    void _requestClose() { if (_close_cb) _close_cb(); }

    // ── 数据 (worker 写 / UI 读, mutex 保护; worker 绝不碰 LVGL) ──
    struct MetricCache {
        std::string value;   // 已格式化 "14,437" / "9.72" / "—"
        std::string time;    // "06:59" (样本 at 的 HH:MM)
        bool has = false;    // 本次快照里该类型有无数据
    };
    std::mutex _mu;
    std::vector<MetricCache> _cache;   // 11 项, 下标与 kMetrics 对齐
    std::string _updated_txt;          // "更新 07:11"
    std::atomic<bool> _data_ready{false};
    std::atomic<bool> _fetch_in_flight{false};
    uint64_t _last_fetch_ms = 0;

    std::function<void()> _close_cb;
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _scroll     = nullptr;   // 滚动容器 (2 列卡网格)
    lv_obj_t* _updated_lbl = nullptr;  // 顶栏右侧 "更新 HH:MM"
    std::vector<lv_obj_t*> _val_lbl;   // 每卡大数值
    std::vector<lv_obj_t*> _unit_lbl;  // 每卡单位 (数值变宽后须重排)
    std::vector<lv_obj_t*> _time_lbl;  // 每卡时间
};
