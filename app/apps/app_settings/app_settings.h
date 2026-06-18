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

class AppSettings : public mooncake::AppAbility {
public:
    AppSettings();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

    // Called when the user backs out, so the launcher can restore its screen.
    void setCloseCallback(std::function<void()> cb) { _close_cb = std::move(cb); }

private:
    std::function<void()> _close_cb;

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

    void _installSwipeGesture();
    void _removeSwipeGesture();

    static void _toolBtn_cb(lv_event_t* e);  // tools tile → open calculator
    static void _calcKey_cb(lv_event_t* e);  // a calculator key
    static void _back_cb(lv_event_t* e);     // calculator → tools page

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
};
