#include "app_news.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include "../utils/psram_json.h"
#include "../utils/news/news_feeds.h"
#include "../utils/news/news_rss.h"
#include "../utils/theme/theme.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
// 30px 全字库 (详情标题)
static const lv_font_t* zh30()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
// 24px (卡片标题/详情正文)
static const lv_font_t* zh24()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
// 20px (页签/时间/简介)
static const lv_font_t* zh20()
{
    static lv_font_t* f = []() -> lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_20");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : nullptr;
    }();
    return f;
}
#elif defined(PLATFORM_BUILD_WEB)
// web: 三档 cbin 与真机逐字节一致 (资产镜像)
#include "web_fonts.h"
static const lv_font_t* zh30() { return webui::font_30(); }
static const lv_font_t* zh24() { return webui::font_24(); }
static const lv_font_t* zh20() { return webui::font_20(); }
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh30() { return &font_puhui_20_4; }
static const lv_font_t* zh24() { return &font_puhui_20_4; }
static const lv_font_t* zh20() { return &font_puhui_20_4; }
#endif

static const std::string TAG = "news";

AppNews::AppNews()
{
    setAppInfo().name = "新闻";
}

// ── 小工具 (oneapp news_feed_commit 同口径) ─────────────────────────────────
namespace {

// URL host 提取 (scheme 后到首个分隔符; 端口剥离 — 只用于 "." 合法性判)
std::string url_host(const std::string& u)
{
    size_t p = u.find("://");
    if (p == std::string::npos) return "";
    size_t b = p + 3, e = u.find_first_of("/?#:?[", b);
    if (e == std::string::npos) e = u.size();
    return u.substr(b, e - b);
}

// 自定义源 id: c_ + fnv1a(url) 前 8 hex (形状同 oneapp md5[:8]; 同址重加走
// url 去重, 哈希算法与 Zero 不同源无影响)
std::string gen_feed_id(const std::string& url)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char b[24];
    snprintf(b, sizeof(b), "c_%08llx", (unsigned long long)h);
    return b;
}

// UTF-8 边界安全截断 (resize/substr 拦腰切多字节 = 非法序列,
// nlohmann dump 严格校验直接 throw — news_rss clean_text 同款教训)
std::string utf8_cut(const std::string& s, size_t limit)
{
    if (s.size() <= limit) return s;
    size_t cut = limit;
    while (cut > 0 && (((unsigned char)s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut);
}

// 行按钮 user_data (堆分配, LV_EVENT_DELETE 释放)
struct IdCtx {
    AppNews* app;
    std::string id, name;
};

}  // namespace

// ── 生命周期 ────────────────────────────────────────────────────────────────
void AppNews::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    if (_scr) {
        lv_screen_load(_scr);   // 复用留存屏
        _rebuildTabs();         // 期间源表可能变过 (云对齐/设置弹窗)
        _renderList();          // 应用缓存
        return;
    }
    _alive = std::make_shared<bool>(true);
    _buildUi();
    lv_screen_load(_scr);
    _installSwipeGesture();
    // 打开立即拉一次 (ever_loaded 后 5min 节流在 onRunning 管)
    _fetch();
}

void AppNews::onRunning()
{
    // 失焦收尾 (app_health 模式): openApp(主页) 不关本 app, 自己发现被切走
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
        _closeAllModals();
        _removeSwipeGesture();
        _teardownUi();
        return;
    }
    if (!_scr) return;

    const uint32_t now = GetHAL()->millis();
    // toast 到期回收
    if (_toast_lbl && now >= _toast_until_ms) {
        lv_obj_delete(_toast_lbl);
        _toast_lbl = nullptr;
    }
    // 前台 5min 刷新 (每源 10min TTL, 5min 拉=中点新鲜度)
    if (_last_fetch_ms == 0 || now - _last_fetch_ms > 5u * 60 * 1000u) {
        _last_fetch_ms = now;
        _fetch();
    }
    // worker 有新数据 → 页签快照可能已换 (云对齐/隐藏集变化), 重建 + 渲染
    if (_data_ready.exchange(false)) {
        {
            std::lock_guard<std::mutex> lk(_mu);   // _srcs 是 worker 写的, 钳回也要在锁内
            if (_tab >= (int)_srcs.size()) _tab = 0;
        }
        _rebuildTabs();   // 末尾 _setTab(_tab) → _renderList
    }
}

void AppNews::onClose()
{
    mclog::tagInfo(TAG, "on close");
    _closeAllModals();
    _removeSwipeGesture();
    _teardownUi();
}

void AppNews::_teardownUi()
{
    if (_scr) {
        lv_obj_t* scr = _scr;
        _scr = nullptr;
        lv_async_call([](void* p) {
            lv_obj_delete(static_cast<lv_obj_t*>(p));
        }, scr);
    }
    _tab_btns.clear();
    _scroll = nullptr;
    _status_lbl = nullptr;
    _src_btn = nullptr;
}

void AppNews::_closeAllModals()
{
    _closeDetail();
    if (_add_modal) _finishCloseAddModal();
    _closeConfirm();
    _closeSrcSettings();
    if (_toast_lbl) {
        lv_obj_delete(_toast_lbl);   // layer_top 上的 toast 不随 _scr 走
        _toast_lbl = nullptr;
    }
}

// ── UI ──────────────────────────────────────────────────────────────────────
void AppNews::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(theme::cur().page), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部页签行 (可见源动态) + 右侧「源设置」按钮 (oneapp S 键视图入口)。
    // 2026-11-05 定稿: 一行横滚, 按钮 240x74 字 30px; 2026-11-21 行宽收到
    // 1104 给源设置钮让位 (源多时页签本就可横向拖)。
    constexpr int TAB_GAP = 8, TAB_H = 74, TAB_X = 16, TAB_Y = 8;
    constexpr int TAB_AREA_H = 90;
    constexpr int SRC_BTN_W = 130;
    lv_obj_t* tabrow = lv_obj_create(_scr);
    lv_obj_set_pos(tabrow, TAB_X, TAB_Y);
    lv_obj_set_size(tabrow, 1280 - TAB_X * 2 - SRC_BTN_W - 12, TAB_AREA_H);
    lv_obj_set_style_bg_opa(tabrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabrow, 0, 0);
    lv_obj_set_style_pad_all(tabrow, 0, 0);
    lv_obj_set_style_pad_column(tabrow, TAB_GAP, 0);
    lv_obj_set_flex_flow(tabrow, LV_FLEX_FLOW_ROW);   // 不换行, 溢出=横向滚动
    lv_obj_set_flex_align(tabrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(tabrow, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tabrow, LV_SCROLLBAR_MODE_ACTIVE);
    _tab_host = tabrow;

    lv_obj_t* srcbtn = lv_button_create(_scr);
    lv_obj_set_size(srcbtn, SRC_BTN_W, TAB_H);
    lv_obj_set_pos(srcbtn, 1280 - 16 - SRC_BTN_W, TAB_Y);
    lv_obj_set_style_bg_color(srcbtn, lv_color_hex(theme::cur().card[3]), 0);
    lv_obj_set_style_bg_color(srcbtn, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(srcbtn, 1, 0);
    lv_obj_set_style_border_color(srcbtn, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_border_opa(srcbtn, LV_OPA_70, 0);
    lv_obj_set_style_radius(srcbtn, 12, 0);
    lv_obj_set_style_shadow_width(srcbtn, 0, 0);
    lv_obj_add_event_cb(srcbtn, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_openSrcSettings();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t* sl = lv_label_create(srcbtn);
    lv_label_set_text(sl, "源设置");
    lv_obj_set_style_text_font(sl, zh30(), 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(theme::cur().text_hi), 0);
    lv_obj_center(sl);
    _src_btn = srcbtn;

    // 右下角: 更新时间 (页签行右侧被源设置钮占位)
    _status_lbl = lv_label_create(_scr);
    lv_label_set_text(_status_lbl, "");
    lv_obj_set_style_text_font(_status_lbl, zh20(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(theme::cur().text_dim), 0);
    lv_obj_align(_status_lbl, LV_ALIGN_BOTTOM_RIGHT, -16, -8);

    // 滚动列表 (app_health 写法): 页签区 8+90, y=104 起铺到屏底
    _scroll = lv_obj_create(_scr);
    lv_obj_set_size(_scroll, 1280, 720 - 104);
    lv_obj_set_pos(_scroll, 0, 104);
    lv_obj_set_style_bg_opa(_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_scroll, 0, 0);
    lv_obj_set_style_pad_all(_scroll, 0, 0);
    lv_obj_set_style_pad_row(_scroll, 12, 0);
    lv_obj_set_flex_flow(_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_scroll, LV_SCROLLBAR_MODE_ACTIVE);

    _rebuildTabs();
    _setTab(0);
}

void AppNews::_rebuildTabs()
{
    if (!_tab_host) return;
    std::vector<Src> srcs;
    {
        std::lock_guard<std::mutex> lk(_mu);
        srcs = _srcs;
    }
    if (srcs.empty()) {          // 尚未拉取: 用当前可见源先摆页签 (打开即有形)
        for (auto& s : newsfeed::visible()) srcs.push_back({s.id, s.name});
    }
    lv_obj_clean(_tab_host);
    _tab_btns.clear();
    for (int i = 0; i < (int)srcs.size(); i++) {
        lv_obj_t* btn = lv_button_create(_tab_host);
        lv_obj_set_size(btn, 240, 74);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::cur().card[3]), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::cur().accent), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(theme::cur().border), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        // 文字色挂按钮层 (状态随按钮); label 不设本地色才能继承 —
        // 上轮把 CHECKED 色设在 label 上, 但 CHECKED 态只在按钮对象上,
        // label 永远进不了选中态 → 选中后仍是未选色贴 accent 底 (看不清根因)
        lv_obj_set_style_text_color(btn, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(theme::ink_on(theme::cur().accent)),
                                    LV_STATE_CHECKED);   // 亮 accent 底自动深字
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppNews*>(lv_event_get_user_data(e));
            self->_setTab((int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, srcs[i].name.c_str());
        lv_obj_set_width(l, 240 - 16);            // 自定义源名可到 20 字, 超出省略
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_center(l);
        _tab_btns.push_back(btn);
    }
    _setTab(_tab);
}

void AppNews::_setTab(int tab)
{
    if (tab < 0 || tab >= (int)_tab_btns.size()) tab = 0;
    _tab = tab;
    for (int i = 0; i < (int)_tab_btns.size(); i++) {
        if (!_tab_btns[i]) continue;
        if (i == tab) lv_obj_add_state(_tab_btns[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(_tab_btns[i], LV_STATE_CHECKED);
    }
    _renderList();
}

void AppNews::_renderList()
{
    if (!_scroll) return;
    FeedData fd;
    std::string fetched;
    size_t src_count = 0;
    {
        std::lock_guard<std::mutex> lk(_mu);
        src_count = _srcs.size();
        if (_tab >= 0 && _tab < (int)_feeds.size() && _tab < (int)_srcs.size())
            fd = _feeds[_tab];
        fetched = _fetched;
    }
    std::string st_txt;
    if (src_count == 0) st_txt = "源已全部隐藏";
    else if (!fetched.empty()) st_txt = "更新 " + fetched;
    if (_status_lbl) lv_label_set_text(_status_lbl, st_txt.c_str());

    // 重建: 清空滚动容器再逐条建卡 (页签切换零请求, 数据来自同一次拉取)
    lv_obj_clean(_scroll);

    if (src_count == 0) {
        lv_obj_t* ph = lv_label_create(_scroll);
        lv_label_set_text(ph, "全部源已隐藏\n点右上「源设置」开启或添加源");
        lv_obj_set_style_text_font(ph, zh24(), 0);
        lv_obj_set_style_text_color(ph, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_text_align(ph, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(ph, 48, 0);
        return;
    }
    if (_tab >= (int)_feeds.size()) {   // 源表刚换、数据还是旧快照 → 等下一轮
        lv_obj_t* ph = lv_label_create(_scroll);
        lv_label_set_text(ph, "加载中…");
        lv_obj_set_style_text_font(ph, zh24(), 0);
        lv_obj_set_style_text_color(ph, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_pad_all(ph, 24, 0);
        return;
    }
    if (!fd.ok) {
        lv_obj_t* ph = lv_label_create(_scroll);
        lv_label_set_text(ph, _ever_loaded ? "该源暂不可用" : "加载中…");
        lv_obj_set_style_text_font(ph, zh24(), 0);
        lv_obj_set_style_text_color(ph, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_pad_all(ph, 24, 0);
        return;
    }
    if (fd.items.empty()) {
        lv_obj_t* ph = lv_label_create(_scroll);
        lv_label_set_text(ph, "该源暂无内容");
        lv_obj_set_style_text_font(ph, zh24(), 0);
        lv_obj_set_style_text_color(ph, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_pad_all(ph, 24, 0);
        return;
    }

    for (int i = 0; i < (int)fd.items.size(); i++) {
        const NItem& it = fd.items[i];
        lv_obj_t* card = lv_obj_create(_scroll);
        // 卡高 170 (2026-11-05 用户要求字号加大): 标题 30px 2 行区 + 简介 24px 2 行
        lv_obj_set_size(card, 1232, 170);
        lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[3]), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)i);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppNews*>(lv_event_get_user_data(e));
            self->_openDetail((int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
        }, LV_EVENT_CLICKED, this);

        // 标题 (30px, 居中 — 2026-11-05 用户要求; 宽给满卡, 极长首行与
        // 右上时间戳同层的重叠概率仅 >33 字标题, 接受)
        lv_obj_t* tl = lv_label_create(card);
        lv_label_set_text(tl, it.t.c_str());
        lv_obj_set_pos(tl, 24, 16);
        lv_obj_set_width(tl, 1184);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(tl, zh30(), 0);
        lv_obj_set_style_text_color(tl, lv_color_hex(theme::cur().text_hi), 0);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(tl, 6, 0);

        // 时间 (右上; rss ts 如 "11-05 09:30")
        if (!it.ts.empty()) {
            lv_obj_t* tsl = lv_label_create(card);
            lv_label_set_text(tsl, it.ts.c_str());
            lv_obj_align(tsl, LV_ALIGN_TOP_RIGHT, -24, 20);
            lv_obj_set_style_text_font(tsl, zh24(), 0);
            lv_obj_set_style_text_color(tsl, lv_color_hex(theme::cur().text_dim), 0);
        }

        // 简介 (24px 暗色, 居中, y=106 容 2 行标题后接排; 超出裁剪 — 详情弹窗看全文)
        if (!it.d.empty()) {
            lv_obj_t* dl = lv_label_create(card);
            lv_label_set_text(dl, it.d.c_str());
            lv_obj_set_pos(dl, 24, 106);
            lv_obj_set_width(dl, 1184);
            lv_obj_set_height(dl, 64);
            lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(dl, zh24(), 0);
            lv_obj_set_style_text_color(dl, lv_color_hex(theme::cur().text_dim), 0);
            lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(dl, 4, 0);
        }
    }
}

void AppNews::_openDetail(int idx)
{
    if (_detail_modal) return;   // 已开
    std::string cat, title, desc, ts, fetched;
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (_tab >= (int)_feeds.size()) return;
        const FeedData& fd = _feeds[_tab];
        if (idx < 0 || idx >= (int)fd.items.size()) return;
        cat    = fd.name;
        title  = fd.items[idx].t;
        desc   = fd.items[idx].d;
        ts     = fd.items[idx].ts;
        fetched = _fetched;
    }

    // 全屏遮罩 (layer_top; 点外关) + 卡片 800x620 (2026-11-05 用户要求字号
    // 加大: 标题 30/正文 30/meta 24; 装不下 → body 竖向滚动查看)
    _detail_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_detail_modal, 1280, 720);
    lv_obj_set_pos(_detail_modal, 0, 0);
    lv_obj_set_style_bg_color(_detail_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_detail_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_detail_modal, 0, 0);
    lv_obj_set_style_radius(_detail_modal, 0, 0);
    lv_obj_set_style_pad_all(_detail_modal, 0, 0);
    lv_obj_clear_flag(_detail_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_detail_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_detail_modal, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_closeDetail();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* card = lv_obj_create(_detail_modal);
    lv_obj_set_size(card, 800, 620);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[0]), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    // 竖向滚动 body (meta/标题/简介; 长简介滚动查看, scrollbar 常显提示可滚)
    lv_obj_t* body = lv_obj_create(card);
    lv_obj_set_pos(body, 24, 20);
    lv_obj_set_size(body, 800 - 48, 620 - 40);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);

    std::string meta = cat;
    if (!ts.empty()) meta += " · " + ts;
    if (!fetched.empty()) meta += " · 更新 " + fetched;
    lv_obj_t* ml = lv_label_create(body);
    lv_label_set_text(ml, meta.c_str());
    lv_obj_set_width(ml, LV_PCT(100));
    lv_obj_set_style_text_font(ml, zh24(), 0);
    lv_obj_set_style_text_color(ml, lv_color_hex(theme::cur().text_dim), 0);

    lv_obj_t* tl = lv_label_create(body);
    lv_label_set_text(tl, title.c_str());
    lv_obj_set_width(tl, LV_PCT(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(tl, zh30(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(theme::cur().text_hi), 0);
    lv_obj_set_style_text_line_space(tl, 8, 0);

    lv_obj_t* dl = lv_label_create(body);
    lv_label_set_text(dl, desc.empty() ? "（这条新闻没有简介）" : desc.c_str());
    lv_obj_set_width(dl, LV_PCT(100));
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(dl, zh30(), 0);
    lv_obj_set_style_text_color(dl, lv_color_hex(theme::cur().text_soft), 0);
    lv_obj_set_style_text_line_space(dl, 8, 0);
}

void AppNews::_closeDetail()
{
    if (_detail_modal) {
        lv_obj_delete(_detail_modal);   // layer_top 上的遮罩+卡片整树删
        _detail_modal = nullptr;
    }
}

// ── 源设置弹窗 (oneapp S 键视图同款: 全源列表 显隐/加/改/删) ────────────────
void AppNews::_openSrcSettings()
{
    if (_src_modal) return;
    _closeDetail();
    _src_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_src_modal, 1280, 720);
    lv_obj_set_pos(_src_modal, 0, 0);
    lv_obj_set_style_bg_color(_src_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_src_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_src_modal, 0, 0);
    lv_obj_set_style_radius(_src_modal, 0, 0);
    lv_obj_set_style_pad_all(_src_modal, 0, 0);
    lv_obj_clear_flag(_src_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_src_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_src_modal, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_closeSrcSettings();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* card = lv_obj_create(_src_modal);
    lv_obj_set_size(card, 1000, 620);
    lv_obj_set_pos(card, 140, 50);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[0]), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // 挡住点卡传到遮罩

    lv_obj_t* tl = lv_label_create(card);
    lv_label_set_text(tl, "源设置");
    lv_obj_set_pos(tl, 24, 20);
    lv_obj_set_style_text_font(tl, zh30(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(theme::cur().text_hi), 0);
    _src_count = lv_label_create(card);
    lv_obj_set_pos(_src_count, 130, 28);
    lv_obj_set_style_text_font(_src_count, zh24(), 0);
    lv_obj_set_style_text_color(_src_count, lv_color_hex(theme::cur().text_dim), 0);

    _src_list = lv_obj_create(card);
    lv_obj_set_pos(_src_list, 24, 80);
    lv_obj_set_size(_src_list, 1000 - 48 - 8, 620 - 80 - 96);
    lv_obj_set_style_bg_opa(_src_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_src_list, 0, 0);
    lv_obj_set_style_pad_all(_src_list, 0, 0);
    lv_obj_set_style_pad_row(_src_list, 10, 0);
    lv_obj_set_style_pad_column(_src_list, 16, 0);
    lv_obj_set_flex_flow(_src_list, LV_FLEX_FLOW_ROW_WRAP);   // 2 列 (2026-11-22 用户要求)
    lv_obj_set_flex_align(_src_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(_src_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_src_list, LV_SCROLLBAR_MODE_ACTIVE);

    auto mk_btn = [&](const char* txt, int x, uint32_t bg, uint32_t fg,
                      lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(card);
        lv_obj_set_size(b, 220, 64);
        lv_obj_set_pos(b, x, 620 - 78);
        lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh24(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_obj_center(l);
        return b;
    };
    mk_btn("+ 添加源", 24, theme::cur().accent, theme::ink_on(theme::cur().accent),
           [](lv_event_t* e) {
               static_cast<AppNews*>(lv_event_get_user_data(e))->_openAddModal("");
           });
    mk_btn("完成", 1000 - 24 - 220, theme::cur().track, theme::cur().text_hi,
           [](lv_event_t* e) {
               static_cast<AppNews*>(lv_event_get_user_data(e))->_closeSrcSettings();
           });

    _rebuildSrcRows();
}

void AppNews::_closeSrcSettings()
{
    if (_src_modal) {
        lv_obj_t* m = _src_modal;
        _src_modal = nullptr;
        _src_list = nullptr;
        _src_count = nullptr;
        // 可能从「完成」按钮(卡片子对象)回调进入 → 延迟到本轮事件后删
        lv_async_call([](void* p) {
            lv_obj_delete(static_cast<lv_obj_t*>(p));
        }, m);
    }
}

void AppNews::_rebuildSrcRows()
{
    if (!_src_list) return;
    auto table = newsfeed::loadTable();
    if (_src_count) {
        std::string t = "· " + std::to_string((int)table.size()) + " 源";
        lv_label_set_text(_src_count, t.c_str());
    }
    lv_obj_clean(_src_list);

    constexpr int ROW_W = 460, ROW_H = 83;   // 双列 460 (944 容器 2×460+16), 高 72→83 (+15%)
    for (auto& e : table) {
        bool custom = e.id.rfind("c_", 0) == 0;
        lv_obj_t* row = lv_obj_create(_src_list);
        lv_obj_set_size(row, ROW_W, ROW_H);
        lv_obj_set_style_bg_color(row, lv_color_hex(theme::cur().card[3]), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(theme::cur().border), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_50, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        auto* ctx = new IdCtx{this, e.id, e.name};
        lv_obj_add_event_cb(row, [](lv_event_t* ev) {
            delete static_cast<IdCtx*>(lv_event_get_user_data(ev));
        }, LV_EVENT_DELETE, ctx);

        // 名称 (显=hi / 隐=dim; 自定义行收窄给 徽标+改/删/显 按钮让位)
        lv_obj_t* nl = lv_label_create(row);
        lv_label_set_text(nl, e.name.c_str());
        lv_obj_set_width(nl, custom ? 176 : 316);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
        lv_obj_align(nl, LV_ALIGN_LEFT_MID, 16, 0);   // 垂直居中 (2026-11-22 用户要求)
        lv_obj_set_style_text_font(nl, zh24(), 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(
            e.on ? theme::cur().text_hi : theme::cur().text_dim), 0);

        // 「自」徽标 (自定义源; 名称 x16..192 → 徽标 x200, 删钮 x240 起 — 不重叠)
        if (custom) {
            lv_obj_t* cb = lv_label_create(row);
            lv_label_set_text(cb, "自");
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 200, 0);
            lv_obj_set_style_text_font(cb, zh20(), 0);
            lv_obj_set_style_bg_color(cb, lv_color_hex(theme::cur().track), 0);
            lv_obj_set_style_bg_opa(cb, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(cb, 6, 0);
            lv_obj_set_style_radius(cb, 8, 0);
            lv_obj_set_style_text_color(cb, lv_color_hex(theme::cur().text_dim), 0);
        }

        // 显/隐 开关 (全部源同权, oneapp news_feed_toggle 同款)
        lv_obj_t* tog = lv_button_create(row);
        lv_obj_set_size(tog, 96, 48);
        lv_obj_align(tog, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_bg_color(tog, lv_color_hex(
            e.on ? theme::cur().accent : theme::cur().track), 0);
        lv_obj_set_style_bg_color(tog, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
        lv_obj_set_style_radius(tog, 12, 0);
        lv_obj_set_style_shadow_width(tog, 0, 0);
        lv_obj_set_user_data(tog, ctx);
        lv_obj_add_event_cb(tog, [](lv_event_t* ev) {
            auto* c = static_cast<IdCtx*>(lv_event_get_user_data(ev));
            auto t = newsfeed::loadTable();
            for (auto& x : t) {
                if (x.id != c->id) continue;
                x.on = !x.on;
                newsfeed::saveTable(t);
                c->app->_showToast(
                    x.name + (x.on ? " 已显示" : " 已隐藏") +
                    (NotesAccountPanel::token().empty() ? "" : " · 已上云"));
                c->app->_refreshFromTable();
                // 行显隐配色刷新必须延迟: 本按钮在 _src_list 里,
                // _rebuildSrcRows 的 clean 会删到回调链上的对象 (app_lottery 同款)
                lv_async_call([](void* u) {
                    static_cast<AppNews*>(u)->_rebuildSrcRows();
                }, c->app);
                break;
            }
        }, LV_EVENT_CLICKED, ctx);
        lv_obj_t* tgt = lv_label_create(tog);
        lv_label_set_text(tgt, e.on ? "显" : "隐");
        lv_obj_set_style_text_font(tgt, zh24(), 0);
        lv_obj_set_style_text_color(tgt, lv_color_hex(
            e.on ? theme::ink_on(theme::cur().accent) : theme::cur().text_dim), 0);
        lv_obj_center(tgt);

        // 自定义源: 改 / 删 (内置源只可隐不可删, oneapp 同款)
        if (custom) {
            lv_obj_t* eb = lv_button_create(row);
            lv_obj_set_size(eb, 44, 48);
            lv_obj_align(eb, LV_ALIGN_RIGHT_MID, -124, 0);
            lv_obj_set_style_bg_color(eb, lv_color_hex(theme::cur().track), 0);
            lv_obj_set_style_bg_color(eb, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
            lv_obj_set_style_radius(eb, 12, 0);
            lv_obj_set_style_shadow_width(eb, 0, 0);
            lv_obj_set_user_data(eb, ctx);
            lv_obj_add_event_cb(eb, [](lv_event_t* ev) {
                auto* c = static_cast<IdCtx*>(lv_event_get_user_data(ev));
                c->app->_openAddModal(c->id);
            }, LV_EVENT_CLICKED, ctx);
            lv_obj_t* el = lv_label_create(eb);
            lv_label_set_text(el, "改");
            lv_obj_set_style_text_font(el, zh24(), 0);
            lv_obj_set_style_text_color(el, lv_color_hex(theme::cur().text_hi), 0);
            lv_obj_center(el);

            lv_obj_t* db = lv_button_create(row);
            lv_obj_set_size(db, 44, 48);
            lv_obj_align(db, LV_ALIGN_RIGHT_MID, -176, 0);
            lv_obj_set_style_bg_color(db, lv_color_hex(theme::cur().track), 0);
            lv_obj_set_style_bg_color(db, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
            lv_obj_set_style_radius(db, 12, 0);
            lv_obj_set_style_shadow_width(db, 0, 0);
            lv_obj_set_user_data(db, ctx);
            lv_obj_add_event_cb(db, [](lv_event_t* ev) {
                auto* c = static_cast<IdCtx*>(lv_event_get_user_data(ev));
                c->app->_openConfirmDel(c->id, c->name);
            }, LV_EVENT_CLICKED, ctx);
            lv_obj_t* dl2 = lv_label_create(db);
            lv_label_set_text(dl2, "删");
            lv_obj_set_style_text_font(dl2, zh24(), 0);
            lv_obj_set_style_text_color(dl2, lv_color_hex(theme::cur().text_hi), 0);
            lv_obj_center(dl2);
        }
    }
}

// 表变更共用: 页签快照即时跟进 + 清掉可能越界的页签 + 触发重拉
void AppNews::_refreshFromTable()
{
    {
        std::lock_guard<std::mutex> lk(_mu);
        _srcs.clear();
        for (auto& s : newsfeed::visible()) _srcs.push_back({s.id, s.name});
        if (_tab >= (int)_srcs.size()) _tab = 0;
    }
    _last_fetch_ms = 0;          // 下一个 onRunning tick 立即按新生效表重拉
    _rebuildTabs();              // 末尾 _setTab(_tab) → _renderList
}

// ── 加/改源弹窗 (oneapp 两相输入的功能对齐: 触屏一次给全两框) ──────────────
void AppNews::_openAddModal(const std::string& edit_id)
{
    if (_add_modal) return;
    _add_edit_id = edit_id;
    _add_closing = false;

    _add_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_add_modal, 1280, 720);
    lv_obj_set_pos(_add_modal, 0, 0);
    lv_obj_set_style_bg_color(_add_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_add_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_add_modal, 0, 0);
    lv_obj_set_style_radius(_add_modal, 0, 0);
    lv_obj_set_style_pad_all(_add_modal, 0, 0);
    lv_obj_clear_flag(_add_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_add_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_add_modal, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_closeAddModal();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* card = lv_obj_create(_add_modal);
    lv_obj_set_size(card, 900, 330);
    lv_obj_set_pos(card, 190, 24);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[0]), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    std::string old_name, old_url;
    if (!_add_edit_id.empty()) {
        for (auto& x : newsfeed::loadTable())
            if (x.id == _add_edit_id) {
                old_name = x.name;
                old_url  = x.url;
                break;
            }
    }

    auto mk_lbl = [&](const char* txt, int x, int y) {
        lv_obj_t* l = lv_label_create(card);
        lv_label_set_text(l, txt);
        lv_obj_set_pos(l, x, y + 14);
        lv_obj_set_style_text_font(l, zh24(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(theme::cur().text_dim), 0);
    };
    auto mk_ta = [&](int y) {
        lv_obj_t* ta = lv_textarea_create(card);
        lv_obj_set_pos(ta, 150, y);
        lv_obj_set_size(ta, 710, 56);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, 300);
        lv_obj_set_style_text_font(ta, zh24(), 0);
        lv_obj_set_style_bg_color(ta, lv_color_hex(theme::cur().track), 0);
        lv_obj_add_event_cb(ta, [](lv_event_t* e) {
            auto* self = static_cast<AppNews*>(lv_event_get_user_data(e));
            if (self->_add_kb) lv_keyboard_set_textarea(self->_add_kb, lv_event_get_current_target_obj(e));
        }, LV_EVENT_FOCUSED, this);
        return ta;
    };

    mk_lbl("标题", 36, 24);
    _add_title = mk_ta(24);
    lv_textarea_set_text(_add_title, old_name.c_str());
    mk_lbl("地址", 36, 100);
    _add_url = mk_ta(100);
    lv_textarea_set_text(_add_url, old_url.c_str());

    _add_hint = lv_label_create(card);
    lv_label_set_text(_add_hint, "");
    lv_obj_set_pos(_add_hint, 36, 172);
    lv_obj_set_width(_add_hint, 830);
    lv_label_set_long_mode(_add_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(_add_hint, zh20(), 0);
    lv_obj_set_style_text_color(_add_hint, lv_color_hex(theme::cur().accent), 0);

    auto mk_btn2 = [&](const char* txt, int x, uint32_t bg, uint32_t fg,
                       lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(card);
        lv_obj_set_size(b, 200, 64);
        lv_obj_set_pos(b, x, 236);
        lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh24(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_obj_center(l);
    };
    mk_btn2(_add_edit_id.empty() ? "保存" : "保存修改", 460, theme::cur().accent,
            theme::ink_on(theme::cur().accent),
            [](lv_event_t* e) {
                static_cast<AppNews*>(lv_event_get_user_data(e))->_submitAdd();
            });
    mk_btn2("取消", 680, theme::cur().track, theme::cur().text_hi,
            [](lv_event_t* e) {
                static_cast<AppNews*>(lv_event_get_user_data(e))->_closeAddModal();
            });

    // 屏键盘 (有实体键盘就不弹, app_stocks 同款; ✓=保存 ✗=收弹窗)
    _add_kb = lv_keyboard_create(_add_modal);
    lv_obj_set_size(_add_kb, 1280, 340);
    lv_obj_align(_add_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_add_kb, _add_url);
    lv_obj_add_event_cb(_add_kb, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_submitAdd();
    }, LV_EVENT_READY, this);
    lv_obj_add_event_cb(_add_kb, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_closeAddModal();
    }, LV_EVENT_CANCEL, this);
    if (GetHAL()->usbKeyboardDetect()) lv_obj_add_flag(_add_kb, LV_OBJ_FLAG_HIDDEN);
}

void AppNews::_closeAddModal()
{
    if (!_add_modal || _add_closing) return;
    _add_closing = true;
    // 从键盘/按钮自身回调里删祖先 → 必须延迟到本轮事件之后 (app_lottery 实锤)
    lv_async_call([](void* u) {
        static_cast<AppNews*>(u)->_finishCloseAddModal();
    }, this);
}

void AppNews::_finishCloseAddModal()
{
    _add_closing = false;
    if (!_add_modal) return;         // 已被 _closeAllModals 连带删过
    lv_obj_delete(_add_modal);
    _add_modal = _add_title = _add_url = _add_kb = _add_hint = nullptr;
    _add_edit_id.clear();
}

// 校验 + 落表 (oneapp news_feed_commit 同口径: scheme 补全/host 判定/
// url 去重/上限 16; 编辑态 id/on 不变, 可改标题+地址)
void AppNews::_submitAdd()
{
    if (!_add_modal || !_add_title || !_add_url || !_add_hint) return;
    auto hint = [&](const char* msg) {
        lv_label_set_text(_add_hint, msg);
    };

    std::string title = lv_textarea_get_text(_add_title);
    std::string url   = lv_textarea_get_text(_add_url);
    // strip 首尾空白
    auto trim = [](std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    };
    trim(title);
    trim(url);

    if (url.empty()) {
        hint("地址不能为空");
        return;
    }
    if (url.find("://") == std::string::npos) url = "https://" + url;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        hint("地址无效 需 http(s)://");
        return;
    }
    std::string host = url_host(url);
    if (host.find('.') == std::string::npos) {
        hint("地址无效 缺域名");
        return;
    }
    if (url.size() > 200) url = utf8_cut(url, 200);
    title = utf8_cut(title, 60);               // 20 个汉字内 (UTF-8 3B×20)
    std::string disp = title.empty()
        ? (host.rfind("www.", 0) == 0 ? host.substr(4) : host)
        : title;

    auto table = newsfeed::loadTable();
    // 关弹窗/刷行统一走延迟路径: 本函数多从弹窗按钮/键盘回调进入,
    // 直删祖先或在回调链里 clean 兄弟都会崩 (app_lottery/app_stocks 同款铁律)
    auto done = [&](const std::string& msg) {
        _closeAddModal();
        _showToast(msg + (NotesAccountPanel::token().empty() ? "" : " · 已上云"));
        _refreshFromTable();
        lv_async_call([](void* u) {
            static_cast<AppNews*>(u)->_rebuildSrcRows();
        }, this);
    };

    if (!_add_edit_id.empty()) {
        // ── 编辑: id/on 不变, 改标题/地址 ──
        newsfeed::Entry* old = nullptr;
        for (auto& x : table)
            if (x.id == _add_edit_id) { old = &x; break; }
        if (!old) {
            hint("该源已被删除 (可能云同步变更)");
            return;
        }
        for (auto& x : table) {
            if (&x == old) continue;
            if (x.id == old->id || x.url == url) {
                hint("该地址已有源");
                return;
            }
        }
        old->url  = url;
        old->name = utf8_cut(disp, 60);
        newsfeed::saveTable(table);
        done("已改 " + old->name);
        return;
    }

    // ── 新增: 同 id/同址 → 视为恢复显示 (oneapp 幂等重加) ──
    std::string fid = gen_feed_id(url);
    for (auto& x : table) {
        if (x.id == fid || x.url == url) {
            bool was_off = !x.on;
            x.on = true;
            newsfeed::saveTable(table);
            done(x.name + (was_off ? " 已存在 已显示" : " 已存在"));
            return;
        }
    }
    if ((int)table.size() >= newsfeed::NEWS_FEEDS_MAX) {
        char b[48];
        snprintf(b, sizeof(b), "最多 %d 个源", newsfeed::NEWS_FEEDS_MAX);
        hint(b);
        return;
    }
    table.push_back({fid, utf8_cut(disp, 60), url, true});
    newsfeed::saveTable(table);
    done("已加 " + disp);
}

// ── 删自定义源确认 (oneapp dialog delnfeed 同款) ────────────────────────────
void AppNews::_openConfirmDel(const std::string& id, const std::string& name)
{
    if (_confirm_modal) return;
    _del_id = id;
    _confirm_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_confirm_modal, 1280, 720);
    lv_obj_set_pos(_confirm_modal, 0, 0);
    lv_obj_set_style_bg_color(_confirm_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_confirm_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_confirm_modal, 0, 0);
    lv_obj_set_style_radius(_confirm_modal, 0, 0);
    lv_obj_set_style_pad_all(_confirm_modal, 0, 0);
    lv_obj_clear_flag(_confirm_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_confirm_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_confirm_modal, [](lv_event_t* e) {
        static_cast<AppNews*>(lv_event_get_user_data(e))->_closeConfirm();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* card = lv_obj_create(_confirm_modal);
    lv_obj_set_size(card, 640, 280);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[0]), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::cur().border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* tl = lv_label_create(card);
    lv_label_set_text(tl, "删除源?");
    lv_obj_set_pos(tl, 32, 28);
    lv_obj_set_style_text_font(tl, zh30(), 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(theme::cur().text_hi), 0);

    lv_obj_t* nl = lv_label_create(card);
    lv_label_set_text(nl, name.c_str());
    lv_obj_set_pos(nl, 32, 90);
    lv_obj_set_width(nl, 640 - 64);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(nl, zh24(), 0);
    lv_obj_set_style_text_color(nl, lv_color_hex(theme::cur().text_soft), 0);

    auto mk_btn3 = [&](const char* txt, int x, uint32_t bg, uint32_t fg,
                       lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(card);
        lv_obj_set_size(b, 200, 64);
        lv_obj_set_pos(b, x, 180);
        lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(theme::cur().press), LV_STATE_PRESSED);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, zh24(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
        lv_obj_center(l);
    };
    mk_btn3("删除", 180, theme::cur().accent, theme::ink_on(theme::cur().accent),
            [](lv_event_t* e) {
                auto* self = static_cast<AppNews*>(lv_event_get_user_data(e));
                auto t = newsfeed::loadTable();
                for (auto it = t.begin(); it != t.end(); ++it) {
                    if (it->id != self->_del_id) continue;
                    if (it->id.rfind("c_", 0) != 0) break;   // 内置只可隐不可删
                    std::string nm = it->name;
                    t.erase(it);
                    newsfeed::saveTable(t);
                    self->_closeConfirm();   // 延迟删 (回调链铁律)
                    self->_showToast("已删 " + nm +
                                     (NotesAccountPanel::token().empty() ? "" : " · 已上云"));
                    self->_refreshFromTable();
                    self->_rebuildSrcRows();   // 行对象不在回调链上 (确认框独立), 可同步刷
                    return;
                }
                self->_closeConfirm();
            });
    mk_btn3("取消", 420, theme::cur().track, theme::cur().text_hi,
            [](lv_event_t* e) {
                static_cast<AppNews*>(lv_event_get_user_data(e))->_closeConfirm();
            });
}

void AppNews::_closeConfirm()
{
    if (_confirm_modal) {
        lv_obj_t* m = _confirm_modal;
        _confirm_modal = nullptr;
        _del_id.clear();
        // 可能从「删除」按钮(卡片子对象)回调进入 → 延迟到本轮事件后删
        lv_async_call([](void* p) {
            lv_obj_delete(static_cast<lv_obj_t*>(p));
        }, m);
    }
}

// ── toast (oneapp osd 同款反馈; onRunning 到期回收) ─────────────────────────
// app_radio 同款模式: 不 remove_style_all (样式删光后 content-size 行为
// 不可靠 → 文本被裁半, 2026-11-21 真机实锤), 显式 CONTENT 尺寸 + 显式
// 白字; 位置放弹窗卡片 (y50..670) 下方的屏底空条, 不盖任何行/按钮。
void AppNews::_showToast(const std::string& txt)
{
    if (_toast_lbl) {
        lv_obj_delete(_toast_lbl);
        _toast_lbl = nullptr;
    }
    _toast_lbl = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_toast_lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(_toast_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_toast_lbl, LV_OPA_80, 0);
    lv_obj_set_style_border_width(_toast_lbl, 0, 0);
    lv_obj_set_style_radius(_toast_lbl, 12, 0);
    lv_obj_set_style_shadow_width(_toast_lbl, 0, 0);
    lv_obj_set_style_pad_left(_toast_lbl, 28, 0);
    lv_obj_set_style_pad_right(_toast_lbl, 28, 0);
    lv_obj_set_style_pad_top(_toast_lbl, 12, 0);
    lv_obj_set_style_pad_bottom(_toast_lbl, 12, 0);
    lv_obj_clear_flag(_toast_lbl,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(_toast_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t* l = lv_label_create(_toast_lbl);
    lv_label_set_text(l, txt.c_str());
    lv_obj_set_style_text_font(l, zh24(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    _toast_until_ms = GetHAL()->millis() + 2200;
}

// ── 数据拉取 (worker 只写缓存, 绝不碰 LVGL — app_health 模式) ────────────────
void AppNews::_fetch()
{
    if (_fetch_in_flight.exchange(true)) return;   // 防重入
    const bool ok = GetHAL()->tryRunDetached([this]() {
        std::vector<Src> srcs;
        std::vector<FeedData> out;
        std::string fetched;
        do {
            // 生效表喂抓取层 (首次) + 与 Zero oneapp 源表云对齐 (60s 节流,
            // 表可能被云端换 → visible() 在对齐之后取)
            newsfeed::ensureActive();
            newsfeed::kvPullMaybe();
            for (auto& s : newsfeed::visible()) srcs.push_back({s.id, s.name});
            out.resize(srcs.size());
            nlohmann::json j;
            try {
                j = PsramJson::parse(news_rss::fetchAllJson());
            } catch (...) {
                mclog::tagWarn(TAG, "json parse failed");
                fetched = "--";   // 诚实信号: _ever_loaded 置位 → 页签显
                                  // "该源暂不可用", 不再永远"加载中…"
                break;
            }
            if (!j.value("ok", false) || !j.contains("feeds") || !j["feeds"].is_object()) {
                break;
            }
            const auto& feeds = j["feeds"];
            for (size_t i = 0; i < srcs.size(); i++) {
                if (!feeds.contains(srcs[i].id) || !feeds[srcs[i].id].is_object()) continue;
                const auto& f = feeds[srcs[i].id];
                out[i].ok   = f.value("ok", false);
                out[i].name = f.value("name", srcs[i].name);
                if (!out[i].ok || !f.contains("items") || !f["items"].is_array()) continue;
                for (auto& it : f["items"]) {
                    if (!it.is_object()) continue;
                    NItem n;
                    n.t  = it.value("t", std::string());
                    n.d  = it.value("d", std::string());
                    n.ts = it.value("ts", std::string());
                    if (n.t.empty()) continue;
                    out[i].items.push_back(std::move(n));
                    if (out[i].items.size() >= 30) break;   // 每源上限
                }
            }
            fetched = j.value("fetched", std::string());
        } while (false);
        {
            std::lock_guard<std::mutex> lk(_mu);
            _srcs    = std::move(srcs);
            _feeds   = std::move(out);
            _fetched = fetched;
        }
        if (!fetched.empty()) _ever_loaded = true;
        _data_ready.store(true);
        _fetch_in_flight.store(false);
    });
    if (!ok) {
        // 线程起不来 (低内存) 也必须给 UI 出口, 否则旗标永卡 true
        _fetch_in_flight.store(false);
        mclog::tagWarn(TAG, "tryRunDetached failed");
    }
}

// ── 上拨退出 (app_health 同款: GESTURE + 位移兜底, 弹窗开着逐层收) ──────────
namespace {
struct NwSwipeCtx {
    AppNews* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
};
}

void AppNews::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new NwSwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<NwSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (!ctx->app->_scr || lv_screen_active() != ctx->app->_scr) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    // 位置守卫: 只认屏幕最下缘 (y>650) 起手的上拨
                    if (ctx->press_y <= 650) return;
                    auto* defer = new NwSwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* c = static_cast<NwSwipeCtx*>(u);
                        auto a = c->alive.lock();
                        if (a && *a && c->app->_scr
                            && lv_screen_active() == c->app->_scr) {
                            // 弹窗开着: 逐层收, 不退出 app
                            if (c->app->_add_modal) {
                                c->app->_closeAddModal();
                            } else if (c->app->_confirm_modal) {
                                c->app->_closeConfirm();
                            } else if (c->app->_src_modal) {
                                c->app->_closeSrcSettings();
                            } else if (c->app->_detail_modal) {
                                c->app->_closeDetail();
                            } else {
                                c->app->_requestClose();
                            }
                        }
                        delete c;
                    }, defer);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            // 位移兜底 (PRESSING 不达 indev 回调, PRESSED 记起点)
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<NwSwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                const lv_event_code_t code = lv_event_get_code(e);
                if (code == LV_EVENT_PRESSED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x;
                    ctx->press_y = p.y;
                } else if (code == LV_EVENT_RELEASED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    const bool from_bottom = ctx->press_y > 650;
                    if (from_bottom && dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* defer = new NwSwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* c = static_cast<NwSwipeCtx*>(u);
                            auto a = c->alive.lock();
                            if (a && *a) {
                                if (c->app->_add_modal) {
                                    c->app->_closeAddModal();
                                } else if (c->app->_confirm_modal) {
                                    c->app->_closeConfirm();
                                } else if (c->app->_src_modal) {
                                    c->app->_closeSrcSettings();
                                } else if (c->app->_detail_modal) {
                                    c->app->_closeDetail();
                                } else {
                                    c->app->_requestClose();
                                }
                            }
                            delete c;
                        }, defer);
                    }
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppNews::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
}
