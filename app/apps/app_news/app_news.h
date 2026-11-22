#pragma once
/*
 * app_news — 新闻 (2026-11-05, car-dash 新闻 App 同款; 2026-11-21 对齐
 * oneapp 新闻源设置)。
 *
 * 数据源 = 设备直连 RSS (utils/news/news_rss.h, 生效表由 news_feeds 源表
 * 模块动态给: 隐藏源不抓)。顶部页签 = 可见源动态生成 (oneapp news_srcs
 * 同款), 内容 = 该源新闻卡片竖向滚动列表, 点卡片开详情弹窗。打开时拉
 * 一次, 前台期间 5min 刷新; 换页签零请求 (同一次拉取覆盖全部页签)。
 * 拉取前 kvPullMaybe() 与 Zero oneapp 做源表云对齐 (想法云 kv, 60s 节流)。
 *
 * 「源设置」(右上按钮, oneapp S 键视图同款): 全源列表 显/隐 开关,
 * 自定义源加/改/删 (两输入框+屏键盘; 校验/去重/上限 16 与 oneapp 同口径),
 * 内置源只可隐不可删。源表落 NVS news_feeds [{id,name,url,on}] 并后台
 * 推云 — 与 Zero 双向同步。
 *
 * 主页入口 (底部「新闻」按钮), 上拨/返回回主页 (底缘 y>650 起手守卫,
 * app_health 同款双轨手势); 弹窗 (详情/源设置/加改源/删确认) 开着时
 * 上拨逐层收掉。主页「设置」弹窗新闻 Tab 的 chips 与本 app 共用同一张表。
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
    struct Src {                  // 可见源快照 (页签与 _feeds 平行下标)
        std::string id, name;
    };
    std::mutex _mu;
    std::vector<Src> _srcs;            // fetch 时可见源快照 (顺序=页签序)
    std::vector<FeedData> _feeds;      // 下标与 _srcs 平行
    std::string _fetched;              // "MM-DD HH:MM"
    std::atomic<bool> _data_ready{false};
    std::atomic<bool> _fetch_in_flight{false};
    uint64_t _last_fetch_ms = 0;
    bool _ever_loaded = false;         // 首拉成功过 (重开 app 不必再等)

    void _fetch();                     // 起 worker 拉取 (防重入; 含 kv 对齐)

    // ── UI ──
    void _buildUi();
    void _rebuildTabs();               // 按 _srcs 快照重建页签行
    void _setTab(int tab);
    void _renderList();                // 按当前页签重建列表 (UI 线程)
    void _openDetail(int idx);         // 点卡片 → 详情弹窗 (layer_top)
    void _closeDetail();               // 幂等
    void _detailFill();                // 详情正文填充 (换条重建 body 内容)
    void _detailSwipe(int dir);        // 详情左右拨: +1 下一条 / -1 上一条
    void _teardownUi();                // 删屏 + 清指针 (onClose/失焦共用)

    lv_obj_t* _scr = nullptr;
    lv_obj_t* _tab_host = nullptr;     // 页签行容器 (动态重建)
    std::vector<lv_obj_t*> _tab_btns;
    lv_obj_t* _scroll = nullptr;
    lv_obj_t* _status_lbl = nullptr;   // 右下 "更新 MM-DD HH:MM" / 加载中
    lv_obj_t* _src_btn = nullptr;      // 右上「源设置」
    int _tab = 0;

    // 详情弹窗 (lv_layer_top 上, 随 _closeDetail 显式删除)
    lv_obj_t* _detail_modal = nullptr;
    lv_obj_t* _detail_body = nullptr;  // 竖向滚动容器 (左右拨时 clean 重建内容)
    int       _detail_idx = 0;         // 当前条目下标 (同源内上下条切换)

    // ── 源设置 (2026-11-21 oneapp 对齐) ──
    void _openSrcSettings();
    void _closeSrcSettings();          // 直删 (非回调路径)
    void _rebuildSrcRows();
    void _refreshFromTable();          // 表变更共用: 页签+列表即时跟进+重拉
    void _openAddModal(const std::string& edit_id);   // ""=新增
    void _closeAddModal();             // 异步延迟删 (按钮回调路径, app_stocks 同款)
    void _finishCloseAddModal();       // 直删 (teardown 路径)
    void _submitAdd();                 // 校验+落表 (oneapp news_feed_commit 同口径)
    void _openConfirmDel(const std::string& id, const std::string& name);
    void _closeConfirm();
    void _showToast(const std::string& txt);
    void _closeAllModals();            // onClose/失焦收尾共用

    lv_obj_t* _src_modal = nullptr;
    lv_obj_t* _src_list = nullptr;
    lv_obj_t* _src_count = nullptr;
    lv_obj_t* _add_modal = nullptr;
    lv_obj_t* _add_title = nullptr;
    lv_obj_t* _add_url = nullptr;
    lv_obj_t* _add_kb = nullptr;
    lv_obj_t* _add_hint = nullptr;
    bool _add_closing = false;
    std::string _add_edit_id;          // ""=新增, 否则编辑目标源 id
    lv_obj_t* _confirm_modal = nullptr;
    std::string _del_id;
    lv_obj_t* _toast_lbl = nullptr;
    uint64_t _toast_until_ms = 0;

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
