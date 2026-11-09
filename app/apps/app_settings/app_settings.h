/**
 * @file app_settings.h
 * @brief "工具" (Tools) app. The former settings (volume/brightness/WiFi/HA)
 *        moved to the home status-bar popups; this app is now a tools launcher.
 *        First tool: a full-screen calculator.
 *
 * NOTE: the class/dir name stays AppSettings/app_settings to avoid churn in the
 * installer + build globs; only the user-facing name ("工具") and UI changed.
 */
#pragma once
#include <mooncake.h>
#include <smooth_lvgl.h>
#include <functional>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

// Single shared email cache, populated by the worker in
// AppSettings::fetchEmail() and read by both the email sub-page and the
// AppHome status bar icon. Lives at file scope so the static keyword isn't
// required on each declaration; definitions are in app_settings.cpp.
struct EmailFolderInfo {
    std::string name;
    int unread  = 0;
    int flagged = 0;
};

// 一封邮件 (8768 email-status-server 新 schema: 每文件夹最近 30 封带
// uid/seen/flagged — 与 car-dash 邮件面板同源同能力, 支持旗标操作)
struct EmailItem {
    std::string uid, from, subject, date, preview, folder;
    bool seen    = false;
    bool flagged = false;
};

class AppSettings : public mooncake::AppAbility {
public:
    AppSettings();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called when the user backs out, so the launcher can restore its screen.
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    // AppLoraChat 的 mooncake app id, 由 app_installer 注入.
    void setLoraChatAppId(int id) { _lora_chat_id = id; }

    // AppStocks (自选股) 的 mooncake app id, 由 app_installer 注入.
    void setStocksAppId(int id) { _stocks_id = id; }
    void openStocks();  // 工具页 tile → 打开自选股 app
    void setCalendarAppId(int id) { _calendar_id = id; }
    void openCalendar();  // 工具页 tile → 打开万年历 app

    // AppReachy (Reachy 仪表盘) 的 mooncake app id, 由 app_installer 注入.
    void setReachyAppId(int id) { _reachy_id = id; }
    void openReachy();  // 工具页 tile → 打开 Reachy app

    // AppRfid (Unit NFC 读写器) 的 mooncake app id, 由 app_installer 注入.
    void setRfidAppId(int id) { _rfid_id = id; }

    // AppSsh (SSH 终端) 的 mooncake app id, 由 app_installer 注入.
    void setSshAppId(int id) { _ssh_id = id; }

    // AppHealth (健康) 的 mooncake app id, 由 app_installer 注入.
    void setHealthAppId(int id) { _health_id = id; }
    void openHealth();  // 工具页 tile → 打开健康 app

    // AppLottery (双色球) 的 mooncake app id, 由 app_installer 注入.
    void setLotteryAppId(int id) { _lottery_id = id; }
    void openLottery();  // 工具页 tile → 打开双色球 app

    // AppGallery (图库) 的 mooncake app id, 由 app_installer 注入.
    void setGalleryAppId(int id) { _gallery_id = id; }
    void openGallery();  // 工具页 tile → 打开图库 app

    // AppTimer (计时器) 的 mooncake app id, 由 app_installer 注入.
    // (2026-11-05: 计时器入口从主页迁到工具页, 主页按钮位让给「想法」)
    void setTimerAppId(int id) { _timer_id = id; }
    void openTimer();    // 工具页 tile → 打开计时器 app

    // AppGames (游戏合集: 翻牌/星星/扫雷/数独) 的 mooncake app id.
    void setGamesAppId(int id) { _games_id = id; }
    void openGames();    // 工具页 tile → 打开游戏 app

    // AppYt (油管: Pi 中继音频版) 的 mooncake app id.
    void setYtAppId(int id) { _yt_id = id; }
    void openYt();       // 工具页 tile → 打开油管 app

    // ── Shared email cache (status-bar poll + sub-page both read these) ──
    // Worker in fetchEmail() writes; the AppHome status bar polls every 120s
    // and reads email_unread_total to decide whether to show the icon.
    static std::atomic<int>  email_unread_total;   // total, from j["count"]
    static std::atomic<int>  email_flagged_total;  // 红旗总数, j["flagged_total"]
    static std::atomic<bool> email_updated;        // list changed → UI rebuild
    static std::atomic<bool> email_fetching;       // in-flight guard
    static std::atomic<bool> email_error;          // last fetch failed
    static std::string       email_error_msg;
    static std::mutex        email_list_mutex;     // guards email_list/email_folders/email_act_msg
    static std::vector<EmailItem> email_list;      // parsed messages (全文件夹最近 30 封)
    static std::vector<EmailFolderInfo> email_folders;  // 文件夹行 (与 email_list 同序)

    // 旗标操作 (POST /api/email/action, car-dash 同款): worker 完成 →
    // email_act_done 置位 → onRunning 反馈; 成功后 email_refetch_at 记
    // 3.5s 重拉期限 (等服务端 _wake 秒级重轮询)。
    static std::atomic<bool> email_act_done;
    static std::atomic<bool> email_act_ok;
    static std::atomic<bool> email_act_busy;
    static std::atomic<uint32_t> email_refetch_at;  // tick 期限, 0=无
    static std::string       email_act_msg;

    // Kick off an async fetch from hermes :8768. In-flight safe (the second
    // call while one is running is a no-op). Safe to call from anywhere —
    // AppHome's status bar calls it on a 60s timer. Static (no instance state
    // — the worker only touches the static cache above).
    static void fetchEmail();

    // Public entry: jump straight to the email sub-page (used by the home
    // status-bar mail icon). No-op if the app hasn't been opened yet — caller
    // should mooncake::openApp(settings_id) first if needed.
    void openEmailPage();

private:
    std::function<void()> _close_cb;
    int _lora_chat_id = -1;
    int _stocks_id    = -1;
    int _calendar_id  = -1;
    int _reachy_id    = -1;
    int _rfid_id      = -1;
    int _ssh_id       = -1;
    int _health_id    = -1;
    int _lottery_id   = -1;
    int _gallery_id   = -1;
    int _timer_id     = -1;
    int _games_id     = -1;
    int _yt_id        = -1;

    lv_indev_t* _gesture_indev = nullptr;
    lv_obj_t*   _scr        = nullptr;
    lv_obj_t*   _tools_page = nullptr;  // tools launcher (calculator tile)
    lv_obj_t*   _calc_page  = nullptr;  // calculator (built on first open)

    // ── Calculator ─────────────────────────────────────────────────────────────
    lv_obj_t*   _calc_expr_lbl = nullptr;  // small expression line
    lv_obj_t*   _calc_res_lbl  = nullptr;  // big result line
    std::string _entry = "0";   // number currently being typed
    double      _acc   = 0.0;   // accumulated left-hand value
    char        _op    = 0;     // pending operator: '+','-','*','/'  (0 = none)
    bool        _fresh = true;  // next digit starts a new entry
    std::string _lhs_disp;      // formatted lhs for the expression line

    // ── Currency converter (汇率) ───────────────────────────────────────────────
    lv_obj_t*   _fx_page    = nullptr;
    lv_obj_t*   _fx_from_dd = nullptr;
    lv_obj_t*   _fx_to_dd   = nullptr;
    lv_obj_t*   _fx_amt_lbl = nullptr;  // amount being entered (FROM)
    lv_obj_t*   _fx_res_lbl = nullptr;  // converted result (TO)
    lv_obj_t*   _fx_rate_lbl = nullptr; // "1 美元 = 7.25 人民币"
    std::string _fx_entry   = "1";      // amount string

    // ── Unit converter (单位换算) ────────────────────────────────────────────────
    lv_obj_t*   _unit_page    = nullptr;
    lv_obj_t*   _unit_cat_dd  = nullptr;  // category dropdown
    lv_obj_t*   _unit_from_dd = nullptr;  // from unit dropdown
    lv_obj_t*   _unit_to_dd   = nullptr;  // to unit dropdown
    lv_obj_t*   _unit_amt_lbl = nullptr;  // amount being entered (FROM)
    lv_obj_t*   _unit_res_lbl = nullptr;  // converted result (TO)
    lv_obj_t*   _unit_eq_lbl  = nullptr;  // "1 米 = 100 厘米"
    std::string _unit_entry   = "1";      // amount string
    int         _unit_cat_idx = 0;        // current category

    // ── Email (邮件) — car-dash 同款: 未读/红旗 双视图 + 左右滑露操作钮 ──────────
    bool        _email_open_pending = false;  // app 未打开时点了主页邮件图标, onOpen 建屏后补开
    lv_obj_t*   _email_page   = nullptr;
    lv_obj_t*   _email_hint   = nullptr;  // 列表区居中提示 (加载中/失败/空)
    lv_obj_t*   _email_list   = nullptr;  // 卡片列 (横向滑动的行 + 文件夹组头)
    lv_obj_t*   _email_tabs[2] = {nullptr, nullptr};  // 未读 / 红旗 页签
    lv_obj_t*   _email_status_lbl = nullptr;  // "N 封未读" / 操作反馈 (car-dash tool-sub)
    int         _email_view = 0;              // 0=未读 1=红旗 (car-dash mailView)
    lv_obj_t*   _mail_swipe_open = nullptr;   // 当前滑开的行 (单开原则, car-dash mailSwipeOpen)
    uint32_t    _mail_swipe_ts = 0;           // 滑开时刻 → 5s 无操作自动合拢
    lv_timer_t* _mail_swipe_timer = nullptr;  // 500ms 巡检自动合拢

    void _buildToolsPage();
    void _openCalculator();
    void _closeCalculator();
    void _calcReset();
    void _calcInput(const char* key);
    void _calcRefresh();

    void _openFx();
    void _closeFx();
    void _fxInput(const char* key);
    void _fxRecompute();
    void _fxFetch();   // async pull of live rates from the HA server /rate proxy

    void _openUnit();
    void _closeUnit();
    void _unitInput(const char* key);
    void _unitRecompute();
    void _unitPopulateUnits();  // fill from/to dropdowns when category changes

    void _openEmail();
    void _closeEmail();
    void _emailRefresh();       // UI thread: 按当前视图重建列表
    void _emailShowError();
    void _emailSetView(int v);  // 页签切换 → 重渲染
    void _emailUpdateTabs();    // 页签 on 态高亮
    void _emailCollapseSwipe(bool anim);  // 合拢滑开的行 (切视图/操作后/5s 超时)
    lv_obj_t* _emailMakeRow(const EmailItem& m);  // 一行 = 左右操作区 + 可横滑面卡
    void _emailRefreshGo();     // 刷新钮: 先 /refresh 唤醒再拉状态
    void _emailRunAction(const std::string& act, const std::string& folder,
                         const std::string& uid, const char* tip);
    static void _emailTab_cb(lv_event_t* e);
    static void _emailRefreshBtn_cb(lv_event_t* e);
    static void _emailAct_cb(lv_event_t* e);
    static void _emailActDel_cb(lv_event_t* e);      // MailActCtx 释放
    static void _emailRowPressed_cb(lv_event_t* e);  // 按新行时合拢旧行 (单开)
    static void _emailRowScroll_cb(lv_event_t* e);   // SCROLL_END 吸合 + 单开登记
    static void _emailSwipeTimer_cb(lv_timer_t* e);

    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _toolBtn_cb(lv_event_t* e);  // tools tile → open calculator
    static void _calcKey_cb(lv_event_t* e);  // a calculator key
    static void _back_cb(lv_event_t* e);     // calculator → tools page
    static void _toolLoraChat_cb(lv_event_t* e);  // tools tile → open LoRa 聊天

    static void _toolFx_cb(lv_event_t* e);   // tools tile → open converter
    static void _fxKey_cb(lv_event_t* e);    // a converter keypad key
    static void _fxDd_cb(lv_event_t* e);     // currency dropdown changed
    static void _fxSwap_cb(lv_event_t* e);   // swap from/to
    static void _fxBack_cb(lv_event_t* e);   // converter → tools page

    static void _toolUnit_cb(lv_event_t* e); // tools tile → open unit converter
    static void _unitKey_cb(lv_event_t* e);  // a unit converter keypad key
    static void _unitCatDd_cb(lv_event_t* e); // category dropdown changed
    static void _unitFromDd_cb(lv_event_t* e); // from unit dropdown changed
    static void _unitToDd_cb(lv_event_t* e);   // to unit dropdown changed
    static void _unitSwap_cb(lv_event_t* e);   // swap from/to
    static void _unitBack_cb(lv_event_t* e);   // converter → tools page

    static void _toolMail_cb(lv_event_t* e);   // tools tile → open email page
    static void _toolStocks_cb(lv_event_t* e); // tools tile → open 自选股
    static void _toolCalendar_cb(lv_event_t* e); // tools tile → open 万年历
    static void _toolReachy_cb(lv_event_t* e); // tools tile → open Reachy app
    static void _toolRfid_cb(lv_event_t* e);   // tools tile → open RFID 读写器
    static void _toolSsh_cb(lv_event_t* e);    // tools tile → open SSH 终端
    static void _toolHealth_cb(lv_event_t* e); // tools tile → open 健康
    static void _toolLottery_cb(lv_event_t* e); // tools tile → open 双色球
    static void _toolGallery_cb(lv_event_t* e); // tools tile → open 图库
    static void _toolTimer_cb(lv_event_t* e);   // tools tile → open 计时器 (2026-11-05 自主页迁入)
    static void _toolGames_cb(lv_event_t* e);   // tools tile → open 游戏合集 (2026-11-06)
    static void _toolYt_cb(lv_event_t* e);      // tools tile → open 油管 (2026-11-06)
};
