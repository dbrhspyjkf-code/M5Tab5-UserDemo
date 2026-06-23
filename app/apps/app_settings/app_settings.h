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
// AppHome status-bar icon. Lives at file scope so the static keyword isn't
// required on each declaration; definitions are in app_settings.cpp.
struct EmailItem { std::string from, subject, date, folder; };

class AppSettings : public mooncake::AppAbility {
public:
    AppSettings();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called when the user backs out, so the launcher can restore its screen.
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

    // AppUnitPuzzle (灯阵) 的 mooncake app id, 由 app_installer 注入.
    // _toolPuzzle_cb 用它启动外部 AppUnitPuzzle.
    void setPuzzleAppId(int id) { _puzzle_id = id; }

    // AppLoraChat 的 mooncake app id, 由 app_installer 注入.
    void setLoraChatAppId(int id) { _lora_chat_id = id; }

    // AppStocks (自选股) 的 mooncake app id, 由 app_installer 注入.
    void setStocksAppId(int id) { _stocks_id = id; }
    void openStocks();  // 工具页 tile → 打开自选股 app

    // ── Shared email cache (status-bar poll + sub-page both read these) ──
    // Worker in fetchEmail() writes; the AppHome status bar polls every 60s
    // and reads email_unread_total to decide whether to show the icon.
    static std::atomic<int>  email_unread_total;   // total, from j["count"]
    static std::atomic<bool> email_updated;        // list changed → UI rebuild
    static std::atomic<bool> email_fetching;       // in-flight guard
    static std::atomic<bool> email_error;          // last fetch failed
    static std::string       email_error_msg;
    static std::mutex        email_list_mutex;     // guards email_list
    static std::vector<EmailItem> email_list;      // parsed folder entries

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
    int _puzzle_id    = -1;
    int _lora_chat_id = -1;
    int _stocks_id    = -1;

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

    // ── Email (邮件) ────────────────────────────────────────────────────────────
    lv_obj_t*   _email_page   = nullptr;
    lv_obj_t*   _email_title  = nullptr;  // "邮件 (N 封未读)" — top title
    lv_obj_t*   _email_status = nullptr;  // "加载中…" / "拉取失败" / empty marker
    lv_obj_t*   _email_list   = nullptr;  // lv_list 主体

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
    void _emailRefresh();  // UI thread: rebuild lv_list from email_list
    void _emailShowError();

    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _toolBtn_cb(lv_event_t* e);  // tools tile → open calculator
    static void _calcKey_cb(lv_event_t* e);  // a calculator key
    static void _back_cb(lv_event_t* e);     // calculator → tools page
    static void _toolPuzzle_cb(lv_event_t* e);    // tools 第 2 行 tile → open 灯阵
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
    static void _emailRefresh_cb(lv_event_t* e); // 邮件子页刷新按钮 → _emailFetch
};
