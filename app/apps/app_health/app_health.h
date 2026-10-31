#pragma once
/*
 * app_health — 健康 (2026-10-31)。
 *
 * 深色苹果风 (iOS 健康 App 深色模式): 黑底 + #1C1C1E 圆角卡 + 彩色圆角图标
 * + 大数字。数据源 hermes GET :8766/api/health (iPhone 快捷指令 POST 推送,
 * 服务端聚合摘要), 60s 轮询, 12 类指标 3 列卡网格。上拨返回工具页
 * (底缘 y>650 起手守卫, 滚动页与手势共存, app_stocks 同款)。
 * 长按卡片 → 全屏每日走势详情 (canvas 柱状图 + 7 日均线, 数据源
 * GET :8766/api/health/history 经 hermes 转发云端按日聚合; 详情页内
 * 底缘上拨 = 关详情不退 app, app_stocks 走势图同款骨架)。
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

    // ── 指标详情: 长按卡片 → 全屏每日走势 (app_stocks 走势图同款骨架) ──
    void _openDetail(int idx);     // idx = kMetrics 下标
    void _closeDetail();           // 幂等: 删弹窗/释放画布/作废在飞 worker
    void _applyHistory();          // worker 结果 → 画图 (UI 线程)
    void _drawDetailChart();       // 整幅重画 (调用方须在 LVGL 上下文)
    void _zoomDetail(int delta);   // ＋/－ 调整可视天数窗口
    void _fetchHistory();          // 起 worker 拉 /api/health/history (防重入)

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

    // 详情页状态 (全部只在大屏 _scr 子树, 随屏共生死; 像素缓冲单独释放)
    lv_obj_t* _detail_modal  = nullptr;
    lv_obj_t* _detail_canvas = nullptr;
    void*     _detail_pixels = nullptr;   // 1280*656*4, malloc 自动落 PSRAM
    lv_obj_t* _detail_status = nullptr;   // 加载中 / 失败提示
    lv_obj_t* _detail_hilo   = nullptr;   // 窗口内高/低值 (右上角)
    lv_obj_t* _detail_leg    = nullptr;   // 「7日均」图例 (左上角)
    lv_obj_t* _detail_val    = nullptr;   // 顶栏当前值 (数据晚到时补填)
    std::vector<lv_obj_t*> _detail_xt;    // x 轴日期 label (modal 子对象)

    // 历史数据 (worker 写 / UI 读, _hmu 保护; worker 绝不碰 LVGL)
    struct DayPoint {
        int day;      // civil 日序 (连续序列, 缺日已补 0)
        int md;       // 月日压缩 mmdd (画 "M/D" 轴)
        double value;
    };
    std::mutex _hmu;
    std::vector<DayPoint> _hist;         // 升序
    bool _hist_ok = false;
    std::string _hist_err;
    std::string _detail_type;            // 当前详情 hermes type (空=无详情)
    int  _detail_idx = -1;               // kMetrics 下标
    int  _detail_win = 30;               // 可视窗口天数
    std::atomic<bool> _hist_ready{false};
    std::atomic<bool> _hist_in_flight{false};
};
