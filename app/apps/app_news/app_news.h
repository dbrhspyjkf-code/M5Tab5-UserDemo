#pragma once
/*
 * app_news — 新闻 (2026-11-05, car-dash 新闻 App 同款)。
 *
 * 数据源 hermes GET :8766/news/feeds (AI 午报 + 7 个 RSS, 服务端逐源
 * 10min TTL 缓存)。顶部 8 源页签 (car-dash 恒显全部源), 内容 = 该源
 * 新闻卡片竖向滚动列表, 点卡片开详情弹窗 (meta/标题/简介, app_home
 * 新闻弹窗同款骨架)。打开时拉一次, 前台期间 5min 刷新; 换页签零请求
 * (同一次拉取覆盖全部页签)。
 *
 * 主页入口 (底部「新闻」按钮, 替换原「小智」按钮), 上拨/返回回主页
 * (底缘 y>650 起手守卫, app_health 同款双轨手势); 详情弹窗开着时
 * 上拨只关弹窗。源选择 (主页轮播卡轮哪些源) 在主页「设置」弹窗,
 * 与本 app 共用 utils/news/news_feeds.h 的源表。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

class AppNews : public mooncake::AppAbility {
public:
    AppNews();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;   // 失焦收尾 + 刷新节流 + 数据应用 (UI 线程)
    void onClose()   override;

private:
    // ── 数据 (worker 写 / UI 读, _mu 保护; worker 绝不碰 LVGL) ──
    struct NItem {
        std::string t, d, ts;
    };
    struct FeedData {
        bool ok = false;
        std::string name;
        std::vector<NItem> items;
    };
    std::mutex _mu;
    std::vector<FeedData> _feeds;      // 下标 = newsfeed::kSources 下标
    std::string _fetched;              // "MM-DD HH:MM"
    std::atomic<bool> _data_ready{false};
    std::atomic<bool> _fetch_in_flight{false};
    uint64_t _last_fetch_ms = 0;
    bool _ever_loaded = false;         // 首拉成功过 (重开 app 不必再等)

    void _fetch();                     // 起 worker 拉 /news/feeds (防重入)

    // ── UI ──
    static constexpr int kTabs = 8;    // = newsfeed::kSourceCount
    void _buildUi();
    void _setTab(int tab);
    void _renderList();                // 按当前页签重建列表 (UI 线程)
    void _openDetail(int idx);         // 点卡片 → 详情弹窗 (layer_top)
    void _closeDetail();               // 幂等
    void _teardownUi();                // 删屏 + 清指针 (onClose/失焦共用)

    lv_obj_t* _scr = nullptr;
    lv_obj_t* _tab_btns[kTabs] = {};
    lv_obj_t* _scroll = nullptr;
    lv_obj_t* _status_lbl = nullptr;   // 右上 "更新 MM-DD HH:MM" / 加载中
    int _tab = 0;

    // 详情弹窗 (lv_layer_top 上, 随 _closeDetail 显式删除)
    lv_obj_t* _detail_modal = nullptr;

    // ── 手势 (上拨退出, app_health 同款: GESTURE + 位移兜底双轨) ──
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    // 只发回调不调 close() —— 与 openApp(主页) 同拍竞争问题, app_health 同款;
    // 失焦收尾在 onRunning 里检测完成。
    void _requestClose() { if (_close_cb) _close_cb(); }

    std::function<void()> _close_cb;
};
