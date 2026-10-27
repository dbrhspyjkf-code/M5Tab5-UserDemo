#pragma once
/*
 * app_calendar — 万年历 (2026-10-27)。
 *
 * 月历网格 + 每日农历, 点击日期看当日详情 (公历/农历/星期), 上一月/下一月
 * 翻页 (1900-2100, lunar_calendar.h 覆盖范围)。上拨返回工具页。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>
#include <vector>
#include <string>

class AppCalendar : public mooncake::AppAbility {
public:
    AppCalendar();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;   // 失焦检测 → 删屏收尾 (stocks 模式)
    void onClose()   override;

private:
    void _buildUi();
    void _refreshMonth();                 // 重建日期网格 (title + cells)
    void _shiftMonth(int delta);          // ±1 月, 夹在 1900-2100
    void _showDayDetail(int day);
    void _clearGrid();

    void _closeDropdown();  // 关下拉 + 删拦截层 (选择/点外部/退出共用)
    static void _prevClick_cb(lv_event_t* e);
    static void _nextClick_cb(lv_event_t* e);
    static void _prevYearClick_cb(lv_event_t* e);
    static void _nextYearClick_cb(lv_event_t* e);
    static void _yearDd_cb(lv_event_t* e);
    static void _monthDd_cb(lv_event_t* e);
    void _shiftYear(int delta);
    static void _dayClick_cb(lv_event_t* e);

    // ── 手势 (上拨退出, app_stocks 同款) ──
    struct SwipeCtx {
        AppCalendar* app;
        std::weak_ptr<bool> alive;
    };
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    // 照 app_stocks: 只发回调不调 close() —— close() 的 GoClose 会与
    // openApp(工具页) 同拍竞争, 把 tile 刚置的 GoOpen 打回去 (on open 永不
    // 执行); 失焦收尾在 onRunning 里检测 lv_screen_active() 完成。
    void _requestClose() { if (_close_cb) _close_cb(); }

    std::function<void()> _close_cb;
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _title      = nullptr;   // 旧整标题 (仅占位兼容, 不再显示)
    lv_obj_t* _year_lbl   = nullptr;   // "2026 年" (点击弹年份下拉)
    lv_obj_t* _month_lbl  = nullptr;   // "10 月" (点击弹月份下拉)
    lv_obj_t* _year_dd    = nullptr;   // 下拉 (共用 _scr 顶层, 用完删)
    lv_obj_t* _month_dd   = nullptr;
    lv_obj_t* _dd_shield  = nullptr;   // 下拉的全屏拦截层 (点外部关闭; 泄漏会吞全部点击!)
    lv_obj_t* _grid       = nullptr;   // 日期网格容器
    lv_obj_t* _detail_lbl = nullptr;   // 底部当日农历详情
    int _year  = 2026;
    int _month = 10;
    int _detail_day = 1;
    std::vector<lv_obj_t*> _day_cells;  // 42 个格 (6 行 × 7 列), 复用省重建
};
