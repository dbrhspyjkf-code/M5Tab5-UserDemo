#pragma once
/*
 * app_gallery — 图库 (2026-11-02)。
 *
 * 浏览 OrangePi 共享相簿「壁纸」全部照片（与屏保同一照片源，hermes _gallery）:
 * GET :8766/gallery/list 拿清单（新加入的在前，上限 96 张）→ 4 列 296×166 缩略图
 * 网格（/gallery/thumb.rgb565，每张 98KB 顺序拉）→ 点卡片全屏查看
 * （/gallery/photo.rgb565，1280×720 contain 完整显示）。全屏大图 5 槽缓存
 * （当前±2）+ 按翻页方向优先预取——连翻时永远提前备好行进方向的两张。
 *
 * 全屏页: 左右滑动 / 点左右边缘翻页（循环），点中间显隐返回钮与序号 —— 对象级
 * PRESSED/RELEASED（SDL 模拟器 indev 级事件不派发，对象级可用）。
 * 底缘上拨（indev 级，app_health 同款 y>650 起手 + 前台守卫）: 全屏页 = 关全屏，
 * 网格页 = 回工具页；按下时就记下全屏页是否开着，双轨 / 对象级先关了也不会误退出；
 * 网格滚动中的上拨（滚动位移 >10px）不算退出（app_lottery 同款共存规则）。
 * 浏览期间前台独占不进屏保（setBlocker，播放器同款）。
 *
 * 线程模型同屏保: worker 只下载与发布（mutex + 会话代 _gen 丢弃过期结果），
 * 绝不碰 LVGL；UI 线程在 onRunning 消费。拆屏先删屏、再释放像素缓冲。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class AppGallery : public mooncake::AppAbility {
public:
    AppGallery();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }
    // 前台独占: 浏览期间不进屏保（installer 接 AppIdleScreen::setBlocked）
    void setBlocker(std::function<void(bool)> cb) { _block = std::move(cb); }

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;   // 失焦收尾 + 消费 worker 结果 + 调度下载 (UI 线程)
    void onClose()   override;

private:
    struct Item {
        std::string id;
        int w = 0, h = 0;
    };
    struct Slot {
        int idx = -1;
        void* buf = nullptr;
    };

    void _buildUi();
    void _teardown();             // 删屏 + 释放全部像素缓冲 + 解除屏保阻断 (幂等)
    void _setStatus(const char* txt);
    std::string _base() const;

    // ── 数据 ──
    void _fetchList();            // worker: /gallery/list (防重入)
    void _applyList();            // UI: 建卡片网格
    void _startThumbWorker();     // worker: 顺序拉全部缩略图
    void _drainThumbs();          // UI: 就绪缩略图上屏 + 按需起 worker
    void _fetchFull(int idx);     // worker: 拉一张全屏大图
    void _pumpFull();             // UI: 大图入槽 + 调度下一张 (当前→后→前)
    void _storeSlot(int idx, void* buf);
    int  _slotOf(int idx) const;

    // ── 全屏查看 ──
    void _openViewer(int idx);
    void _closeViewer();          // 幂等
    void _showIndex(int idx);     // 有缓存立即显示, 否则「加载中」
    void _step(int delta);        // 翻页 (循环)
    void _toggleChrome();
    static void _viewer_cb(lv_event_t* e);

    // ── 手势 (底缘上拨, indev 级; ctx 类型在 cpp 匿名命名空间, 防跨编译单元同名错配) ──
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    // 工具页子 app: 只发回调不调 close() (close() 的 GoClose 会与 openApp(工具页) 同拍竞争)
    void _requestClose() { if (_close_cb) _close_cb(); }
    void _requestExitOnce();      // 500ms 去重
    uint32_t _last_exit_ms = 0;

    std::function<void()> _close_cb;
    std::function<void(bool)> _block;

    // ── UI 状态 (只在 UI 线程) ──
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _scroll     = nullptr;   // 缩略图网格滚动容器
    lv_obj_t* _status_lbl = nullptr;   // 顶栏右侧「共 N 张」/ 加载状态
    std::vector<Item>      _items;
    std::vector<lv_obj_t*> _tile_img;  // 每卡 lv_image (下标对齐 _items)
    std::vector<void*>     _thumb_buf; // 已上屏缩略图缓冲
    bool _list_loaded   = false;
    bool _thumb_started = false;
    uint32_t _list_retry_ms = 0;       // 0 = 下一帧就拉
    int _list_total = 0;               // 服务端总张数 (可能 > 显示上限)
    int _pending_view = -1;            // 清单到之前请求的全屏序号 (桌面调试钩子)

    lv_obj_t* _viewer         = nullptr;
    lv_obj_t* _viewer_img     = nullptr;
    lv_obj_t* _viewer_idx     = nullptr;   // 「3 / 8」
    lv_obj_t* _viewer_back    = nullptr;   // 「返回」
    lv_obj_t* _viewer_loading = nullptr;   // 「加载中...」/ 失败提示
    int  _view_idx = -1;
    bool _chrome   = true;
    uint32_t _last_flip_ms = 0;
    lv_point_t _press{0, 0};
    Slot _slots[5];                        // 全屏大图缓存 (当前±2)
    int  _last_dir = 1;                    // 上次翻页方向 (+1 前 / -1 后), 预取优先
    int  _full_fail_idx = -1;              // 上次失败序号 (退避后重试)
    uint32_t _full_retry_ms = 0;

    // ── worker → UI (_mu 保护; worker 绝不碰 LVGL) ──
    std::mutex _mu;
    std::atomic<uint32_t> _gen{0};         // 会话代: 拆屏时 ++, 在途结果据此丢弃
    std::vector<Item> _list_pending;
    bool _list_ok = false;
    int  _list_pending_total = 0;
    std::atomic<bool> _list_ready{false};
    std::atomic<bool> _list_in_flight{false};
    std::deque<std::pair<int, void*>> _thumb_q;
    std::atomic<bool> _thumb_running{false};
    std::deque<std::pair<int, void*>> _full_q;
    int _full_failed = -1;
    std::atomic<bool> _full_in_flight{false};
};
