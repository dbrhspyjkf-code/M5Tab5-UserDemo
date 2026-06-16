#include "app_settings.h"
#include <hal/hal.h>
#include <mooncake_log.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include "screensaver/screensaver.h"

static const std::string _tag = "app-tools";

// Chinese font defined in app_ha/view/font_zh_36.c (now includes 工具计算器).
extern lv_font_t font_zh_36;

// Tool tile icons (ARGB8888) defined in app_settings/calc_icon.c & fx_icon.c.
extern const lv_image_dsc_t calc_icon;  // 80x112
extern const lv_image_dsc_t fx_icon;    // 100x100

// Calculator fonts (Arial Unicode subsets) defined in this app's font_calc_*.c.
extern const lv_font_t font_calc_big;   // 80px result line
extern const lv_font_t font_calc_op;    // 40px button labels (has × ÷)
extern const lv_font_t font_calc_expr;  // 30px expression line

// ── Palette ─────────────────────────────────────────────────────────────────
// Tools-page colors mirror the HA "灯光" dashboard cards (app_ha/view.cpp).
static constexpr uint32_t C_BG       = 0x0A1A2E;  // page background (HA C_BG)
static constexpr uint32_t C_ACCENT   = 0x4FA3FF;
static constexpr uint32_t C_CARD     = 0x13304E;  // HA card background
static constexpr uint32_t C_CARD_PR  = 0x1A3A5C;  // HA pressed fill (C_ACCENT_SOFT)
static constexpr uint32_t C_TEXT     = 0xEAF3FB;  // HA primary text

// Calculator palette (matches the reference iPad calculator).
static constexpr uint32_t CALC_BG   = 0x000000;
static constexpr uint32_t CALC_NUM  = 0x3A3A3C;  // dark grey digit keys
static constexpr uint32_t CALC_FUNC = 0x545456;  // medium grey  ⌫ / AC / %
static constexpr uint32_t CALC_OP   = 0xFF9F0A;  // orange operators
static constexpr uint32_t CALC_TXT  = 0xFFFFFF;
static constexpr uint32_t CALC_EXPR = 0x9A9AA0;  // grey expression line

// ── Currency table + live rate cache ─────────────────────────────────────────
namespace {
struct Ccy { const char* code; const char* cn; double per_usd; };
// Static fallback rates (units per 1 USD); overwritten at runtime by the HA
// server's /rate proxy (see _fxFetch). Order must match g_fx_rate below.
const Ccy FX[] = {
    {"USD", "美元",   1.00},
    {"CNY", "人民币", 7.25},
    {"EUR", "欧元",   0.92},
    {"JPY", "日元",   157.0},
    {"GBP", "英镑",   0.79},
    {"HKD", "港元",   7.81},
    {"KRW", "韩元",   1380.0},
    {"AUD", "澳元",   1.51},
};
constexpr int FX_N = (int)(sizeof(FX) / sizeof(FX[0]));

// Live per-USD rates (seeded from the static table, overwritten by /rate). The
// detached fetch worker writes under g_fx_mutex; the UI reads under it too.
std::mutex        g_fx_mutex;
double            g_fx_rate[FX_N] = {1.00, 7.25, 0.92, 157.0, 0.79, 7.81, 1380.0, 1.51};
std::atomic<bool> g_fx_updated{false};   // set by worker, consumed in onRunning
std::atomic<bool> g_fx_fetching{false};
std::string       g_fx_source = "static";
}  // namespace

AppSettings::AppSettings()
{
    setAppInfo().name = "工具";
}

void AppSettings::onCreate() {}

void AppSettings::onOpen()
{
    mclog::tagInfo(_tag, "open");
    _scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    _buildToolsPage();
    lv_screen_load(_scr);
    _installSwipeGesture();
}

void AppSettings::onRunning()
{
    // Apply freshly fetched live rates (worker thread only flips the flag; the
    // label/recompute must happen on the UI loop).
    if (g_fx_updated.exchange(false) && _fx_page) {
        _fxRecompute();
    }
}

void AppSettings::onClose()
{
    mclog::tagInfo(_tag, "close");
    _removeSwipeGesture();
    if (_scr) {
        lv_obj_delete(_scr);
        _scr = nullptr;
    }
    _tools_page = _calc_page = _fx_page = nullptr;
    _calc_expr_lbl = _calc_res_lbl = nullptr;
    _fx_from_dd = _fx_to_dd = _fx_amt_lbl = _fx_res_lbl = _fx_rate_lbl = nullptr;
    if (_close_cb) _close_cb();
}

// ─── Swipe-up gesture closes the whole app (back to home) ───────────────────
void AppSettings::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (screensaver::isActive()) return;
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    lv_async_call([](void* udata) {
                        auto* app = static_cast<AppSettings*>(udata);
                        if (app) app->close();
                    }, lv_event_get_user_data(e));
                }
            }, LV_EVENT_GESTURE, this);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppSettings::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, this);
        _gesture_indev = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Tools launcher page
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_buildToolsPage()
{
    _tools_page = lv_obj_create(_scr);
    lv_obj_set_size(_tools_page, 1280, 720);
    lv_obj_set_pos(_tools_page, 0, 0);
    lv_obj_set_style_bg_opa(_tools_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_tools_page, 0, 0);
    lv_obj_set_style_pad_all(_tools_page, 0, 0);
    lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_tools_page);
    lv_label_set_text(title, "工具");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, &font_zh_36, 0);

    // Tool card — same style as the HA "灯光" device cards: dark-blue rounded
    // card with a soft shadow, icon on the left, name on the right.
    auto make_tile = [&](int x, const lv_image_dsc_t* img, int img_w,
                         const char* label, lv_event_cb_t cb) {
        lv_obj_t* tile = lv_obj_create(_tools_page);
        lv_obj_set_size(tile, 380, 140);
        lv_obj_align(tile, LV_ALIGN_TOP_LEFT, x, 140);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);
        lv_obj_set_style_shadow_width(tile, 18, 0);
        lv_obj_set_style_shadow_color(tile, lv_color_hex(0x5A7A9C), 0);
        lv_obj_set_style_shadow_opa(tile, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(tile, 4, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD_PR), LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, this);

        lv_obj_t* icon = lv_image_create(tile);
        lv_image_set_src(icon, img);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 22, 0);

        lv_obj_t* name = lv_label_create(tile);
        lv_label_set_text(name, label);
        lv_obj_set_style_text_font(name, &font_zh_36, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 22 + img_w + 20, 0);
    };

    make_tile(80,  &calc_icon, 80,  "计算器", _toolBtn_cb);
    make_tile(490, &fx_icon,   100, "汇率",   _toolFx_cb);
}

void AppSettings::_toolBtn_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openCalculator();
}

void AppSettings::_toolFx_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_openFx();
}

// ════════════════════════════════════════════════════════════════════════════
//  Calculator
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openCalculator()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _calc_page = lv_obj_create(_scr);
    lv_obj_set_size(_calc_page, 1280, 720);
    lv_obj_set_pos(_calc_page, 0, 0);
    lv_obj_set_style_bg_color(_calc_page, lv_color_hex(CALC_BG), 0);
    lv_obj_set_style_border_width(_calc_page, 0, 0);
    lv_obj_set_style_radius(_calc_page, 0, 0);
    lv_obj_set_style_pad_all(_calc_page, 0, 0);
    lv_obj_clear_flag(_calc_page, LV_OBJ_FLAG_SCROLLABLE);

    // Back button (top-left): returns to the tools page.
    lv_obj_t* back = lv_obj_create(_calc_page);
    lv_obj_set_size(back, 64, 56);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, _back_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);

    // ── Display: expression (small, grey) over result (big, white), right-aligned.
    _calc_expr_lbl = lv_label_create(_calc_page);
    lv_label_set_text(_calc_expr_lbl, "");
    lv_obj_set_style_text_font(_calc_expr_lbl, &font_calc_expr, 0);
    lv_obj_set_style_text_color(_calc_expr_lbl, lv_color_hex(CALC_EXPR), 0);
    lv_obj_align(_calc_expr_lbl, LV_ALIGN_TOP_RIGHT, -36, 86);

    _calc_res_lbl = lv_label_create(_calc_page);
    lv_label_set_text(_calc_res_lbl, "0");
    lv_obj_set_style_text_font(_calc_res_lbl, &font_calc_big, 0);
    lv_obj_set_style_text_color(_calc_res_lbl, lv_color_hex(CALC_TXT), 0);
    lv_obj_align(_calc_res_lbl, LV_ALIGN_TOP_RIGHT, -36, 130);

    // ── Button grid: 5 columns × 4 rows ──────────────────────────────────────
    struct Key { const char* label; const char* code; uint32_t color; bool sym; };
    // code is what _calcInput receives. "DEL"/"AC"/"PCT"/"NEG" are special.
    static const Key KEYS[4][5] = {
        {{"7","7",CALC_NUM,false},{"8","8",CALC_NUM,false},{"9","9",CALC_NUM,false},
         {LV_SYMBOL_BACKSPACE,"DEL",CALC_FUNC,true},{"\xC3\xB7","/",CALC_OP,false}},
        {{"4","4",CALC_NUM,false},{"5","5",CALC_NUM,false},{"6","6",CALC_NUM,false},
         {"AC","AC",CALC_FUNC,false},{"\xC3\x97","*",CALC_OP,false}},
        {{"1","1",CALC_NUM,false},{"2","2",CALC_NUM,false},{"3","3",CALC_NUM,false},
         {"%","PCT",CALC_FUNC,false},{"-","-",CALC_OP,false}},
        {{"+/-","NEG",CALC_NUM,false},{"0","0",CALC_NUM,false},{".",".",CALC_NUM,false},
         {"=","=",CALC_OP,false},{"+","+",CALC_OP,false}},
    };

    const int SIDE = 24, GAP = 16, COLS = 5, ROWS = 4;
    const int grid_top = 270;
    const int bw = (1280 - 2 * SIDE - (COLS - 1) * GAP) / COLS;   // ~233
    const int bh = (720 - grid_top - SIDE - (ROWS - 1) * GAP) / ROWS;  // ~95

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const Key& k = KEYS[r][c];
            lv_obj_t* b = lv_obj_create(_calc_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, SIDE + c * (bw + GAP), grid_top + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(k.color), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);   // pill
            lv_obj_set_style_border_width(b, 0, 0);
            // Pressed: lighten.
            lv_obj_set_style_bg_color(b,
                lv_color_hex(k.color == CALC_OP ? 0xFFBE5C
                           : k.color == CALC_NUM ? 0x5A5A5C : 0x6E6E70),
                LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _calcKey_cb, LV_EVENT_CLICKED, this);

            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    _calcReset();
}

void AppSettings::_back_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeCalculator();
}

void AppSettings::_closeCalculator()
{
    if (_calc_page) {
        lv_obj_delete(_calc_page);
        _calc_page = nullptr;
        _calc_expr_lbl = _calc_res_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

// ── Calculator logic ─────────────────────────────────────────────────────────
namespace {
// Group the integer part of a numeric string with thousands separators,
// preserving sign and any fractional part / trailing dot as typed.
std::string group(const std::string& s)
{
    std::string sign, num = s;
    if (!num.empty() && (num[0] == '-' || num[0] == '+')) { sign = num.substr(0, 1); num = num.substr(1); }
    size_t dot = num.find('.');
    std::string ip = (dot == std::string::npos) ? num : num.substr(0, dot);
    std::string fp = (dot == std::string::npos) ? ""  : num.substr(dot);  // includes '.'
    std::string out;
    int cnt = 0;
    for (int i = (int)ip.size() - 1; i >= 0; i--) {
        out.insert(out.begin(), ip[i]);
        if (++cnt % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    if (ip.empty()) out = "0";
    return sign + out + fp;
}

// Format a double for display: up to 10 significant digits, trailing zeros
// trimmed, then thousands-grouped.
std::string fmtNum(double v)
{
    if (std::isnan(v) || std::isinf(v)) return "Error";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.10g", v);
    return group(buf);
}

// Currency-converter formatter: at most 3 decimal places, trailing zeros
// trimmed, then thousands-grouped. Keeps the result line short (full-precision
// values like 0.1476688479 otherwise overflow the card).
std::string fxFmt(double v)
{
    if (std::isnan(v) || std::isinf(v)) return "Error";
    char buf[40];
    snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        size_t e = s.size();
        while (e > 0 && s[e - 1] == '0') e--;
        if (e > 0 && s[e - 1] == '.') e--;
        s = s.substr(0, e);
    }
    return group(s);
}

double applyOp(double a, double b, char op)
{
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0.0 ? a / b : NAN;
    }
    return b;
}

const char* opSym(char op)
{
    switch (op) {
        case '+': return "+";
        case '-': return "-";
        case '*': return "\xC3\x97";  // ×
        case '/': return "\xC3\xB7";  // ÷
    }
    return "";
}
}  // namespace

void AppSettings::_calcReset()
{
    _entry = "0";
    _acc   = 0.0;
    _op    = 0;
    _fresh = true;
    _lhs_disp.clear();
    _calcRefresh();
}

void AppSettings::_calcRefresh()
{
    if (_calc_res_lbl)  lv_label_set_text(_calc_res_lbl, group(_entry).c_str());
    if (_calc_expr_lbl) {
        std::string ex;
        if (_op) ex = _lhs_disp + opSym(_op) + (_fresh ? "" : group(_entry));
        lv_label_set_text(_calc_expr_lbl, ex.c_str());
    }
}

void AppSettings::_calcInput(const char* key)
{
    std::string k = key;

    if (k == "AC") { _calcReset(); return; }

    if (k == "DEL") {
        if (!_fresh && _entry != "0") {
            _entry.pop_back();
            if (_entry.empty() || _entry == "-") _entry = "0";
        }
        _calcRefresh();
        return;
    }

    if (k == "NEG") {
        if (_entry != "0") {
            if (_entry[0] == '-') _entry = _entry.substr(1);
            else                  _entry = "-" + _entry;
        }
        _calcRefresh();
        return;
    }

    if (k == "PCT") {
        double v = atof(_entry.c_str()) / 100.0;
        _entry = fmtNum(v);
        // strip grouping commas back out so further edits stay numeric
        std::string t; for (char ch : _entry) if (ch != ',') t += ch;
        _entry = t;
        _fresh = true;
        _calcRefresh();
        return;
    }

    if (k == ".") {
        if (_fresh) { _entry = "0."; _fresh = false; }
        else if (_entry.find('.') == std::string::npos) _entry += ".";
        _calcRefresh();
        return;
    }

    // Digit 0-9
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') {
        if (_fresh) { _entry = k; _fresh = false; }
        else if (_entry == "0") _entry = k;
        else if (_entry == "-0") _entry = "-" + k;
        else _entry += k;
        _calcRefresh();
        return;
    }

    // Operators + - * /
    if (k == "+" || k == "-" || k == "*" || k == "/") {
        if (_op && !_fresh) {
            _acc = applyOp(_acc, atof(_entry.c_str()), _op);
        } else if (!_op) {
            _acc = atof(_entry.c_str());
        }
        _op = k[0];
        _lhs_disp = fmtNum(_acc);
        _entry = fmtNum(_acc);
        { std::string t; for (char ch : _entry) if (ch != ',') t += ch; _entry = t; }
        _fresh = true;
        _calcRefresh();
        return;
    }

    // Equals
    if (k == "=") {
        if (_op) {
            double rhs = atof(_entry.c_str());
            std::string rhs_disp = group(_entry);
            double res = applyOp(_acc, rhs, _op);
            // Show full "lhs op rhs" expression, result below.
            if (_calc_expr_lbl)
                lv_label_set_text(_calc_expr_lbl, (_lhs_disp + opSym(_op) + rhs_disp).c_str());
            _entry = fmtNum(res);
            { std::string t; for (char ch : _entry) if (ch != ',') t += ch; _entry = t; }
            if (_calc_res_lbl) lv_label_set_text(_calc_res_lbl, group(_entry).c_str());
            _acc = res;
            _op = 0;
            _fresh = true;
        }
        return;
    }
}

void AppSettings::_calcKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_calcInput(code);
}

// ════════════════════════════════════════════════════════════════════════════
//  Currency converter (汇率)
// ════════════════════════════════════════════════════════════════════════════
void AppSettings::_openFx()
{
    if (_tools_page) lv_obj_add_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);

    _fx_page = lv_obj_create(_scr);
    lv_obj_set_size(_fx_page, 1280, 720);
    lv_obj_set_pos(_fx_page, 0, 0);
    lv_obj_set_style_bg_color(_fx_page, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(_fx_page, 0, 0);
    lv_obj_set_style_radius(_fx_page, 0, 0);
    lv_obj_set_style_pad_all(_fx_page, 0, 0);
    lv_obj_clear_flag(_fx_page, LV_OBJ_FLAG_SCROLLABLE);

    // Back button + title.
    lv_obj_t* back = lv_obj_create(_fx_page);
    lv_obj_set_size(back, 64, 56);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, _fxBack_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* title = lv_label_create(_fx_page);
    lv_label_set_text(title, "汇率");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(title, &font_zh_36, 0);

    // Currency code options for the dropdowns (ASCII → renders in montserrat).
    std::string opts;
    for (int i = 0; i < FX_N; i++) { if (i) opts += "\n"; opts += FX[i].code; }

    // ── Conversion panel (left) ──────────────────────────────────────────────
    const int CARD_X = 40, CARD_W = 600, CARD_H = 120;

    auto make_conv_card = [&](int y, bool is_from) -> lv_obj_t* {
        lv_obj_t* c = lv_obj_create(_fx_page);
        lv_obj_set_size(c, CARD_W, CARD_H);
        lv_obj_set_pos(c, CARD_X, y);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_shadow_width(c, 18, 0);
        lv_obj_set_style_shadow_color(c, lv_color_hex(0x5A7A9C), 0);
        lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
        lv_obj_set_style_shadow_ofs_y(c, 4, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* dd = lv_dropdown_create(c);
        lv_obj_set_size(dd, 168, 60);
        lv_obj_align(dd, LV_ALIGN_LEFT_MID, 16, 0);
        lv_dropdown_set_options(dd, opts.c_str());
        lv_obj_set_style_text_font(dd, &lv_font_montserrat_28, 0);
        lv_obj_add_event_cb(dd, _fxDd_cb, LV_EVENT_VALUE_CHANGED, this);

        lv_obj_t* val = lv_label_create(c);
        lv_label_set_text(val, "0");
        lv_obj_set_style_text_font(val, &font_calc_big, 0);
        lv_obj_set_style_text_color(val,
            lv_color_hex(is_from ? CALC_TXT : C_ACCENT), 0);
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -20, 0);

        if (is_from) { _fx_from_dd = dd; _fx_amt_lbl = val; }
        else         { _fx_to_dd   = dd; _fx_res_lbl = val; }
        return c;
    };

    make_conv_card(120, true);
    make_conv_card(340, false);

    // Swap button centered in the gap between the two cards.
    // FROM card: 120..(120+CARD_H); TO card starts at 340 → gap centre = 290.
    lv_obj_t* swap = lv_obj_create(_fx_page);
    lv_obj_set_size(swap, 64, 64);
    lv_obj_set_pos(swap, CARD_X + CARD_W / 2 - 32, 290 - 32);
    lv_obj_set_style_bg_color(swap, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swap, 3, 0);
    lv_obj_set_style_border_color(swap, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(swap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swap, _fxSwap_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_center(swap_lbl);
    lv_obj_set_style_text_color(swap_lbl, lv_color_hex(0xFFFFFF), 0);

    // Rate line.
    _fx_rate_lbl = lv_label_create(_fx_page);
    lv_label_set_text(_fx_rate_lbl, "");
    lv_obj_set_pos(_fx_rate_lbl, CARD_X + 6, 480);
    lv_obj_set_style_text_color(_fx_rate_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(_fx_rate_lbl, &font_zh_36, 0);

    // ── Keypad (right) ───────────────────────────────────────────────────────
    const int KX = 700, KW = 540, GAP = 16, COLS = 3, ROWS = 4;

    // AC bar above the keypad.
    lv_obj_t* ac = lv_obj_create(_fx_page);
    lv_obj_set_size(ac, KW, 60);
    lv_obj_set_pos(ac, KX, 70);
    lv_obj_set_style_bg_color(ac, lv_color_hex(CALC_FUNC), 0);
    lv_obj_set_style_radius(ac, 16, 0);
    lv_obj_set_style_border_width(ac, 0, 0);
    lv_obj_set_style_bg_color(ac, lv_color_hex(0x6E6E70), LV_STATE_PRESSED);
    lv_obj_clear_flag(ac, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ac, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(ac, (void*)"AC");
    lv_obj_add_event_cb(ac, _fxKey_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* ac_lbl = lv_label_create(ac);
    lv_label_set_text(ac_lbl, "AC");
    lv_obj_center(ac_lbl);
    lv_obj_set_style_text_color(ac_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ac_lbl, &font_calc_op, 0);

    struct FKey { const char* label; const char* code; bool sym; };
    static const FKey FKEYS[4][3] = {
        {{"7","7",false},{"8","8",false},{"9","9",false}},
        {{"4","4",false},{"5","5",false},{"6","6",false}},
        {{"1","1",false},{"2","2",false},{"3","3",false}},
        {{".",".",false},{"0","0",false},{LV_SYMBOL_BACKSPACE,"DEL",true}},
    };
    const int ktop = 150;
    const int bw = (KW - (COLS - 1) * GAP) / COLS;
    const int bh = (720 - ktop - 30 - (ROWS - 1) * GAP) / ROWS;
    for (int r = 0; r < ROWS; r++) {
        for (int col = 0; col < COLS; col++) {
            const FKey& k = FKEYS[r][col];
            lv_obj_t* b = lv_obj_create(_fx_page);
            lv_obj_set_size(b, bw, bh);
            lv_obj_set_pos(b, KX + col * (bw + GAP), ktop + r * (bh + GAP));
            lv_obj_set_style_bg_color(b, lv_color_hex(CALC_NUM), 0);
            lv_obj_set_style_radius(b, bh / 2, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5C), LV_STATE_PRESSED);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(b, (void*)k.code);
            lv_obj_add_event_cb(b, _fxKey_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, k.label);
            lv_obj_center(l);
            lv_obj_set_style_text_color(l, lv_color_hex(CALC_TXT), 0);
            lv_obj_set_style_text_font(l, k.sym ? &lv_font_montserrat_28 : &font_calc_op, 0);
        }
    }

    // Defaults: 1 USD → CNY.
    _fx_entry = "1";
    lv_dropdown_set_selected(_fx_from_dd, 0);
    lv_dropdown_set_selected(_fx_to_dd, 1);
    _fxRecompute();

    // Pull live rates from the HA server in the background (falls back to the
    // static table if the server is unreachable).
    _fxFetch();
}

void AppSettings::_fxFetch()
{
    if (g_fx_fetching.exchange(true)) return;  // already in flight

    std::string host = GetHAL()->getConfig("ha_host", "192.168.1.142");
    std::string url  = "http://" + host + ":8766/rate";

    bool spawned = GetHAL()->tryRunDetached([url]() {
        auto resp = GetHAL()->httpGet(url);
        if (resp.ok) {
            try {
                auto j = nlohmann::json::parse(resp.body);
                if (j.value("ok", false) && j.contains("rates")) {
                    const auto& rates = j["rates"];
                    std::lock_guard<std::mutex> lk(g_fx_mutex);
                    for (int i = 0; i < FX_N; i++) {
                        auto it = rates.find(FX[i].code);
                        if (it != rates.end() && it->is_number()) {
                            double v = it->get<double>();
                            if (v > 0) g_fx_rate[i] = v;
                        }
                    }
                    g_fx_source = j.value("source", std::string("?"));
                    g_fx_updated.store(true);  // UI loop will recompute
                    mclog::tagInfo(_tag, "fx rates updated ({})", g_fx_source.c_str());
                }
            } catch (...) {
                mclog::tagWarn(_tag, "fx parse failed");
            }
        } else {
            mclog::tagWarn(_tag, "fx fetch failed (keeping static rates)");
        }
        g_fx_fetching.store(false);
    });
    if (!spawned) g_fx_fetching.store(false);
}

void AppSettings::_fxBack_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_closeFx();
}

void AppSettings::_closeFx()
{
    if (_fx_page) {
        lv_obj_delete(_fx_page);
        _fx_page = nullptr;
        _fx_from_dd = _fx_to_dd = _fx_amt_lbl = _fx_res_lbl = _fx_rate_lbl = nullptr;
    }
    if (_tools_page) lv_obj_clear_flag(_tools_page, LV_OBJ_FLAG_HIDDEN);
}

void AppSettings::_fxRecompute()
{
    if (!_fx_from_dd || !_fx_to_dd) return;
    int fi = lv_dropdown_get_selected(_fx_from_dd);
    int ti = lv_dropdown_get_selected(_fx_to_dd);
    if (fi < 0 || fi >= FX_N) fi = 0;
    if (ti < 0 || ti >= FX_N) ti = 0;

    double rf, rt;
    {
        std::lock_guard<std::mutex> lk(g_fx_mutex);
        rf = g_fx_rate[fi];
        rt = g_fx_rate[ti];
    }
    if (rf <= 0) rf = 1;
    double amt = atof(_fx_entry.c_str());
    double res = amt / rf * rt;

    if (_fx_amt_lbl) lv_label_set_text(_fx_amt_lbl, group(_fx_entry).c_str());
    if (_fx_res_lbl) lv_label_set_text(_fx_res_lbl, fxFmt(res).c_str());
    if (_fx_rate_lbl) {
        double one = rt / rf;  // 1 FROM = one TO
        char buf[96];
        snprintf(buf, sizeof(buf), "1 %s = %s %s", FX[fi].cn, fxFmt(one).c_str(), FX[ti].cn);
        lv_label_set_text(_fx_rate_lbl, buf);
    }
}

void AppSettings::_fxInput(const char* key)
{
    std::string k = key;
    if (k == "AC") {
        _fx_entry = "0";
    } else if (k == "DEL") {
        if (!_fx_entry.empty()) _fx_entry.pop_back();
        if (_fx_entry.empty()) _fx_entry = "0";
    } else if (k == ".") {
        if (_fx_entry.find('.') == std::string::npos) _fx_entry += ".";
    } else {  // digit
        if (_fx_entry == "0") _fx_entry = k;
        else if (_fx_entry.size() < 12) _fx_entry += k;
    }
    _fxRecompute();
}

void AppSettings::_fxKey_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
    const char* code = (const char*)lv_obj_get_user_data(b);
    if (code) self->_fxInput(code);
}

void AppSettings::_fxDd_cb(lv_event_t* e)
{
    static_cast<AppSettings*>(lv_event_get_user_data(e))->_fxRecompute();
}

void AppSettings::_fxSwap_cb(lv_event_t* e)
{
    auto* self = static_cast<AppSettings*>(lv_event_get_user_data(e));
    if (!self->_fx_from_dd || !self->_fx_to_dd) return;
    int fi = lv_dropdown_get_selected(self->_fx_from_dd);
    int ti = lv_dropdown_get_selected(self->_fx_to_dd);
    lv_dropdown_set_selected(self->_fx_from_dd, ti);
    lv_dropdown_set_selected(self->_fx_to_dd, fi);
    self->_fxRecompute();
}
