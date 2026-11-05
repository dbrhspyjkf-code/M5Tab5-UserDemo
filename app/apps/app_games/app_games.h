#pragma once
/*
 * app_games — 小游戏合集 (2026-11-05)。
 *
 * 自 car-dash 车机页 4 个触屏特化小游戏移植 (纯点按/长按、零素材):
 *   翻牌  memory  — 翻两张配对, 轻松 4×3 / 标准 6×4 / 挑战 8×5, 最佳=最少步数
 *   星星  popstar — 同色消除 14×6 五色, 首点选中再点消除, n²×5 计分, 全清 +2000
 *   扫雷  mines   — 短点翻开 / 长按插旗 (全局 indev 阈值, stocks/home 设了 500ms),
 *                   首点 3×3 必安全, 最佳=最短用时
 *   数独  sudoku  — 回溯生成+挖洞唯一解 (worker), 数字键盘带剩余计数, 笔记模式,
 *                   错格即时标红 (对照唯一解), 最佳=最短用时
 *
 * 4 tab 常驻同屏 (app_lottery 同款), 切 tab 不重建保局面; 结算/重开确认共用
 * 一个 modal。最佳成绩存 NVS (hal setConfig, gm_* 前缀, 按难度档)。
 * 上拨回工具页 (底缘 y>650 起手, lottery/stocks 同款双轨 500ms 去重 + 前台守卫;
 * 手势 alive 旗标成对铁律 b275e71)。桌面调试: TAB5_OPEN_APP=games 直开,
 * TAB5_GAMES_TAB=0..3 直落指定游戏。
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

class AppGames : public mooncake::AppAbility {
public:
    AppGames();

    // 星星棋盘尺寸 (game_popstar.cpp 文件级 helper 也要用)
    static constexpr int POP_COLS = 14;
    static constexpr int POP_ROWS = 6;

    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    void onCreate()  override {}
    void onOpen()    override;
    void onRunning() override;   // 失焦收尾 + 数独生成结果应用 (UI 线程)
    void onClose()   override;

private:
    // ── 布局常量 ──
    static constexpr int SCREEN_W = 1280;
    static constexpr int SCREEN_H = 720;
    static constexpr int TAB_H    = 64;    // 顶部 tab 栏
    static constexpr int HEAD_H   = 56;    // 各游戏页头 (难度 chips / 状态 / 重开)

    // ── 主题色 (实时读全局主题, app_lottery/stocks 同款) ──
    static uint32_t C_BG()    { return 0x000000; }
    static uint32_t C_CARD()  { return theme::cur().card[1]; }
    static uint32_t C_CARD0() { return theme::cur().card[0]; }
    static uint32_t C_TEXT()  { return theme::cur().text_hi; }
    static uint32_t C_SOFT()  { return theme::cur().text_soft; }
    static uint32_t C_DIM()   { return theme::cur().text_dim; }
    static uint32_t C_ACC()   { return theme::cur().accent; }
    static uint32_t C_TRACK() { return theme::cur().track; }
    static uint32_t C_OK()    { return theme::cur().ok; }
    static uint32_t C_WARN()  { return theme::cur().warn; }
    static uint32_t C_DANGER(){ return theme::cur().danger; }

    // ── NVS 最佳成绩 ──
    static int  _nvsInt(const char* key);
    static void _nvsSet(const char* key, int v);

    // ── UI 骨架 ──
    void _buildUi();
    void _destroyUi();
    void _nullUiPtrs();
    void _setTab(int tab);
    void _teardownTimers();   // tick/翻牌/星星动画链计时器统一清理 (失焦+关闭共用)
    lv_obj_t* _scr = nullptr;
    lv_obj_t* _tab_btns[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* _pages[4]    = {nullptr, nullptr, nullptr, nullptr};
    int _tab = 0;
    lv_timer_t* _tick = nullptr;   // 1s 计时 (扫雷/数独用时), cb 只动 UI 线程安全成员
    static void _tickCb(lv_timer_t* t);

    // ── modal (结算 / 重开确认 共用) ──
    void _showModal(const char* title, const char* body, const char* yes_txt,
                    const char* no_txt, std::function<void()> on_yes);
    void _hideModal();
    bool _modalOpen() const;
    lv_obj_t* _modal      = nullptr;   // 全屏遮罩 (黑 65% + CLICKABLE 挡穿透)
    lv_obj_t* _modal_card = nullptr;
    lv_obj_t* _modal_title = nullptr;
    lv_obj_t* _modal_body  = nullptr;
    lv_obj_t* _modal_btn1  = nullptr;
    lv_obj_t* _modal_btn2  = nullptr;
    std::function<void()> _modal_yes;

    // ── tab0 翻牌 (game_memory.cpp) ──
    struct MemLevel { const char* nm; int cols, rows; };
    static constexpr MemLevel _MEM_LV[3] = {{"轻松", 4, 3}, {"标准", 6, 4}, {"挑战", 8, 5}};
    static const char* _MEM_FACES[24];
    void _buildMemoryPage();
    void _memNew();
    void _memRender(int i);
    void _memRefreshStatus();
    void _memWin();
    static void _memClickCb(lv_event_t* e);
    static void _memChipCb(lv_event_t* e);
    static void _memNewCb(lv_event_t* e);
    static void _memFlipTimerCb(lv_timer_t* t);
    int _mem_lvl = 1;
    int _mem_deck[40]  = {};      // 卡面图案 idx
    int _mem_state[40] = {};      // 0 盖 / 1 翻开待判 / 2 已配对
    int _mem_up0 = -1, _mem_up1 = -1;
    int _mem_moves = 0, _mem_left = 0;
    bool _mem_lock = false;
    lv_obj_t* _mem_cards[40]  = {};
    lv_obj_t* _mem_chips[3]   = {};
    lv_obj_t* _mem_status     = nullptr;
    lv_timer_t* _mem_timer    = nullptr;   // 配对判定延迟 (300ms/700ms one-shot)

    // ── tab1 星星 (game_popstar.cpp) ──
    static constexpr int POP_CLEAR_BONUS = 2000;
    static const uint32_t _POP_COLORS[5];
    void _buildPopPage();
    void _popNew();
    void _popTap(int r, int c);
    int  _popGroup(int r0, int c0, std::vector<int>& out);   // BFS 同色组 (存 r*COLS+c)
    void _popSelect(const std::vector<int>& g);
    void _popDeselect();
    void _popEliminate();
    void _popGravity();
    bool _popHasMoves();
    bool _popAny();
    void _popEnd(bool cleared);
    void _popKillAnims();
    void _popRefreshScore();
    static void _popBlkCb(lv_event_t* e);
    static void _popNewCb(lv_event_t* e);
    static void _popStageCb1(lv_timer_t* t);   // 消除动画完 → 重力
    static void _popStageCb2(lv_timer_t* t);   // 重力动画完 → 解锁+判结束
    lv_obj_t* _pop_blk[POP_ROWS * POP_COLS] = {};   // nullptr=空
    int _pop_col[POP_ROWS * POP_COLS] = {};
    std::vector<lv_obj_t*> _pop_dying;   // 消除淡出中 (UI 逻辑持有生命周期, t1 统一删)
    int _pop_score = 0, _pop_best = 0;
    bool _pop_lock = false;
    std::vector<int> _pop_sel;
    lv_obj_t* _pop_score_lbl = nullptr;
    lv_obj_t* _pop_pick_lbl  = nullptr;
    lv_timer_t* _pop_t1 = nullptr;
    lv_timer_t* _pop_t2 = nullptr;

    // ── tab2 扫雷 (game_mines.cpp) ──
    struct MinLevel { const char* nm; int cols, rows, mines; };
    static constexpr MinLevel _MIN_LV[3] = {{"轻松", 10, 8, 12}, {"标准", 14, 10, 28}, {"挑战", 18, 12, 52}};
    void _buildMinesPage();
    void _minNew();
    void _minRender(int i);
    void _minRenderAll();
    void _minRefreshHead();
    void _minReveal(int i);
    void _minFlag(int i);
    void _minLose(int hit);
    void _minWin();
    void _minPlaceMines(int safe);
    static void _minCellCb(lv_event_t* e);
    static void _minChipCb(lv_event_t* e);
    static void _minNewCb(lv_event_t* e);
    bool _min_mine[216] = {};
    bool _min_rev[216]  = {};
    bool _min_flag[216] = {};
    int  _min_num[216]  = {};
    int _min_cols = 14, _min_rows = 10, _min_total = 28;
    int _min_lvl = 1;
    bool _min_started = false, _min_over = false;
    int _min_opened = 0, _min_flags = 0, _min_sec = 0;
    int64_t _min_lp_ms = 0;        // 长按插旗时刻 (300ms 内的 CLICKED 忽略, 双保险)
    lv_obj_t* _min_cells[216] = {};
    lv_obj_t* _min_chips[3]   = {};
    lv_obj_t* _min_left_lbl   = nullptr;
    lv_obj_t* _min_time_lbl   = nullptr;
    lv_obj_t* _min_best_lbl   = nullptr;

    // ── tab3 数独 (game_sudoku.cpp) ──
    struct SudLevel { const char* nm; int givens; };
    static constexpr SudLevel _SUD_LV[3] = {{"轻松", 40}, {"标准", 32}, {"挑战", 26}};
    void _buildSudokuPage();
    void _sudNew();                // 发 worker 生成 (32KB 栈, PSRAM pthread)
    void _sudApply();              // worker 结果上屏 (onRunning 消费 _sud_ready)
    void _sudRender(int i);
    void _sudRenderAll();
    void _sudHighlight();
    void _sudRenderNums();   // 数字键盘剩余计数/置灰
    void _sudRefreshStatus();
    void _sudFill(int d);          // 当前格填 d (0=擦)
    void _sudWin();
    static void _sudCellCb(lv_event_t* e);
    static void _sudNumCb(lv_event_t* e);
    static void _sudChipCb(lv_event_t* e);
    static void _sudNoteCb(lv_event_t* e);
    static void _sudEraseCb(lv_event_t* e);
    static void _sudNewCb(lv_event_t* e);
    uint8_t _sud_sol[81]   = {};
    uint8_t _sud_cur[81]   = {};
    uint8_t _sud_given[81] = {};
    uint16_t _sud_notes[81] = {};  // bit0..8 = 候选 1..9
    int  _sud_sel = -1;
    bool _sud_note = false, _sud_started = false, _sud_over = false;
    int  _sud_sec = 0, _sud_lvl = 1;
    lv_obj_t* _sud_cells[81]  = {};
    lv_obj_t* _sud_lbls[81]   = {};
    lv_obj_t* _sud_num_btns[9] = {};
    lv_obj_t* _sud_num_cnts[9] = {};
    lv_obj_t* _sud_chips[3]   = {};
    lv_obj_t* _sud_note_btn   = nullptr;
    lv_obj_t* _sud_time_lbl   = nullptr;
    lv_obj_t* _sud_best_lbl   = nullptr;
    lv_obj_t* _sud_status     = nullptr;   // 「生成中…」/失败提示
    std::mutex _sud_mu;
    std::vector<uint8_t> _sud_puz_buf, _sud_sol_buf;
    std::atomic<bool> _sud_ready{false};
    std::atomic<bool> _sud_gen_in_flight{false};

    // ── 手势 (上拨退出; GESTURE + RELEASED 位移双轨 500ms 去重) ──
    // SwipeCtx 在 cpp 匿名空间定义+分配+使用一体 (app_lottery/stocks 同款)。
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
