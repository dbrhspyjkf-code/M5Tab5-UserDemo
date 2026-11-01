#pragma once
/*
 * app_lottery — 双色球 (2026-11-01)。
 *
 * 深色卡面风 (app_health/app_stocks 同款): 黑底 + 主题卡色 + 红蓝球。
 * 数据源 hermes GET :8766/api/ssq/history (官方 cwl.gov.cn, 服务端每日
 * 21:35 刷新 + /tmp 持久缓存), 打开时拉一次, worker 解析成 ssq::DrawIssue。
 *
 * 4 个 tab (顶部切换): 开奖 (最新期大球 + 近 10 期) / 走势 (canvas 遗漏
 * 打点图, 红球 33 行 / 蓝球 16 行, 每页 30 期翻页) / 统计 (canvas 频次
 * 冷热柱状图, 30/50/100 期) / 对奖 (目标期切换 + 红蓝球网格手动选号,
 * 复式 n≤20/m≤16, ssq_prize 枚举判定 → 结果弹窗; 拍照识别按钮二期接
 * /api/ssq/ticket OCR)。
 * 上拨返回工具页 (底缘 y>650 起手守卫, app_stocks/health 同款双轨)。
 */
#include <mooncake.h>
#include <lvgl.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#include "../utils/theme/theme.h"
#include "ssq_prize.h"

class AppLottery : public mooncake::AppAbility {
public:
    AppLottery();

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;   // 失焦收尾 + 数据应用 (UI 线程)
    void onClose()   override;

private:
    // ── 布局常量 ──
    static constexpr int SCREEN_W = 1280;
    static constexpr int SCREEN_H = 720;
    static constexpr int TAB_H    = 64;    // 顶部 tab 栏
    static constexpr int TOOL_H   = 56;    // 走势/统计页工具行
    static constexpr int CH_W     = 1280;  // canvas 宽
    static constexpr int CH_H     = SCREEN_H - TAB_H - TOOL_H;  // 600
    static constexpr int TREND_COLS = 30;  // 走势图每页期数
    static constexpr int SPANS[3] = {30, 50, 100};

    // ── 主题色 (实时读全局主题, app_stocks 同款) ──
    static uint32_t C_BG()   { return 0x000000; }
    static uint32_t C_CARD() { return theme::cur().card[1]; }
    static uint32_t C_TEXT() { return theme::cur().text_hi; }
    static uint32_t C_DIM()  { return theme::cur().text_dim; }
    static uint32_t C_RED()  { return 0xC62828; }  // 红球 (标准彩票红)
    static uint32_t C_BLUE() { return 0x1565C0; }  // 蓝球
    static uint32_t C_ACC()  { return theme::cur().accent; }

    // ── 数据 (worker 写 / UI 读, _mu 保护; worker 绝不碰 LVGL) ──
    std::mutex _mu;
    std::vector<ssq::DrawIssue> _issues;   // 期号倒序 (0=最新), ≤100 期
    bool        _data_ok = false;
    std::string _data_err;
    // 遗漏表: _omit[i][k] = 期 i (0=最新) 时号码 k+1 的遗漏 (100 期内从未
    // 出现 = i+1 低估); 统计频次 _freq 也在此预计算
    int _omit_red[100][33]  = {};
    int _omit_blue[100][16] = {};
    std::atomic<bool> _data_ready{false};
    std::atomic<bool> _fetch_in_flight{false};
    uint64_t _last_fetch_ms = 0;
    void _fetchAsync();
    void _fetchWorker(std::shared_ptr<bool> alive);
    void _recomputeTables();     // 数据到位后算遗漏/频次 (UI 线程, _mu 已持)
    void _applyData();           // worker 完成 → 填 UI (onRunning 消费)

    // ── UI 骨架 ──
    void _buildUi();
    void _destroyUi();
    void _nullUiPtrs();   // 全部 UI 指针置空 (除 _scr)
    void _setTab(int tab);
    lv_obj_t* _scr = nullptr;
    lv_obj_t* _tab_btns[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* _pages[4]    = {nullptr, nullptr, nullptr, nullptr};  // 开奖/走势/统计/对奖
    int _tab = 0;

    // ── 球控件 helper ──
    static lv_obj_t* _makeBall(lv_obj_t* parent, int num, bool blue,
                               int size, const lv_font_t* font);

    // ── tab1 开奖 ──
    void _buildDrawPage();
    void _updateDrawPage();      // 数据到位后填最新卡 + 近 10 期列表
    lv_obj_t* _draw_head   = nullptr;   // 最新期卡片 (整卡重建)
    lv_obj_t* _draw_list   = nullptr;   // 近 10 期滚动列表
    lv_obj_t* _draw_status = nullptr;

    // ── tab2 走势 (canvas 遗漏打点图) ──
    void _buildTrendPage();
    void _drawTrend();           // 整幅重画 (调用方须在 LVGL 上下文)
    lv_obj_t* _trend_canvas  = nullptr;
    void*     _trend_pixels  = nullptr;   // CH_W*CH_H*4, malloc 落 PSRAM
    lv_obj_t* _trend_btns[2] = {nullptr, nullptr};   // 红球/蓝球子切换
    lv_obj_t* _trend_pg_lbl  = nullptr;   // "第 1/4 页 · 2026124-2026095"
    lv_obj_t* _trend_prev    = nullptr;
    lv_obj_t* _trend_next    = nullptr;
    lv_obj_t* _trend_status  = nullptr;   // 加载中/失败提示
    int _trend_color = 0;   // 0=红球 1=蓝球
    int _trend_pg    = 0;   // 0 = 最新一页, 向后翻历史

    // ── tab3 统计 (canvas 冷热柱状图) ──
    void _buildStatPage();
    void _drawStat();
    lv_obj_t* _stat_canvas  = nullptr;
    void*     _stat_pixels  = nullptr;
    lv_obj_t* _stat_btns[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* _stat_status  = nullptr;   // 加载中/失败提示
    lv_obj_t* _stat_title_r = nullptr;   // canvas 内中文标题 (label 叠加)
    lv_obj_t* _stat_title_b = nullptr;
    int _stat_span = 50;

    // ── tab4 对奖 ──
    void _buildPickPage();
    void _updatePickUi();        // 刷新按钮选中态/已选摘要/判定钮可用态
    void _targetShift(int d);    // 目标期 ±1 (0=最新)
    void _judge();               // 校验选号 → 判定 → 结果弹窗
    void _showSingleResult(const ssq::DrawIssue& d, const ssq::Pick& p);
    void _showMultiResult(const ssq::DrawIssue& d, const ssq::Pick& p);
    void _closeResult();

    // ── 拍照识别 (真机链路; 桌面 capturePhotoJpeg 返回空) ──
    // 预览 modal (hal RGB565 直刷 canvas) → 快门 → JPEG POST /api/ssq/ticket
    // → 识别确认 modal (逐注可改/直接对奖) → 多注判定结果
    void _openCamera();
    void _closeCamera();
    void _shutter();
    void _applyRemoteBets();     // worker 完成 → 确认 modal (onRunning 消费)
    void _confirmModal(const std::vector<ssq::Pick>& bets,
                      const std::vector<ssq::Pick>& partials);   // 识别确认 (partial 行引导修正)
    void _judgeBets(const std::vector<ssq::Pick>& bets);      // 多注/复式混合判定
    void _editBet(const ssq::Pick& bet);   // 该注填入手动选号, 切对奖 tab 修正
    lv_obj_t* _cam_modal  = nullptr;
    lv_obj_t* _cam_canvas = nullptr;
    lv_obj_t* _ocr_wait_lbl = nullptr;   // 「识别中…」(上传+OCR ~10s)
    std::mutex _bets_mu;
    std::vector<ssq::Pick> _remote_bets;
    std::vector<ssq::Pick> _remote_partial;   // 识别不完整的注 (引导修正)
    std::string _remote_err;
    std::atomic<bool> _bets_ready{false};
    std::atomic<bool> _upload_in_flight{false};
    lv_obj_t* _pick_red_btns[33]  = {};
    lv_obj_t* _pick_blue_btns[16] = {};
    bool _pick_red[33]  = {};
    bool _pick_blue[16] = {};
    int  _target_idx = 0;   // 对奖目标期 (issues 下标, 0=最新)
    lv_obj_t* _target_lbl   = nullptr;   // "对奖期 2026124"
    lv_obj_t* _target_draws = nullptr;   // 目标期开奖号小行
    lv_obj_t* _pick_summary = nullptr;   // "已选 红 6 + 蓝 1"
    lv_obj_t* _judge_btn    = nullptr;
    lv_obj_t* _photo_btn    = nullptr;   // 拍照识别 (二期接 OCR)
    lv_obj_t* _result_modal = nullptr;
    bool _pick_autorun = false;   // TAB5_LOTTERY_PICK 钩子: 数据到位后自动判定
    std::string _toast;     // 短提示 (onRunning 显示 3s 后自清)
    int64_t _toast_start    = 0;
    lv_obj_t* _toast_lbl    = nullptr;

    // ── 手势 (上拨退出; GESTURE + RELEASED 位移双轨 500ms 去重) ──
    // SwipeCtx 在 cpp 匿名空间定义+分配+使用一体 (app_stocks 同款,
    // 跨编译单元强转别名 = 定时炸弹, 见万年历 7fa88f3 教训)。
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    lv_indev_t* _gesture_indev = nullptr;
    void*       _gesture_ctx   = nullptr;
    int64_t _last_exit_ms = 0;
    void _installSwipeGesture();
    void _removeSwipeGesture();
    void _requestClose() { if (_close_cb) _close_cb(); }
    void _requestExitOnce();

    std::function<void()> _close_cb;
};
