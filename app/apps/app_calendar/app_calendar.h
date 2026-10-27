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
    void onRunning() override {}
    void onClose()   override;

private:
    void _buildUi();
    void _refreshMonth();                 // 重建日期网格 (title + cells)
    void _shiftMonth(int delta);          // ±1 月, 夹在 1900-2100
    void _showDayDetail(int day);
    void _clearGrid();

    static void _prevClick_cb(lv_event_t* e);
    static void _nextClick_cb(lv_event_t* e);
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
    void _requestClose() { close(); }

    std::function<void()> _close_cb;
    lv_obj_t* _scr        = nullptr;
    lv_obj_t* _title      = nullptr;   // "2026年10月"
    lv_obj_t* _grid       = nullptr;   // 日期网格容器
    lv_obj_t* _detail_lbl = nullptr;   // 底部当日农历详情
    int _year  = 2026;
    int _month = 10;
    int _detail_day = 1;
    std::vector<lv_obj_t*> _day_cells;  // 42 个格 (6 行 × 7 列), 复用省重建
};
