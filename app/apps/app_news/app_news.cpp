#include "app_news.h"
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include "../utils/news/news_feeds.h"
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

// ── 生命周期 ────────────────────────────────────────────────────────────────
void AppNews::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    if (_scr) {
        lv_screen_load(_scr);   // 复用留存屏
        _renderList();          // 应用缓存 (期间可能有新数据)
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
        _closeDetail();
        _removeSwipeGesture();
        _teardownUi();
        return;
    }
    if (!_scr) return;

    const uint32_t now = GetHAL()->millis();
    // 前台 5min 刷新 (服务端逐源 10min TTL, 5min 拉=中点新鲜度)
    if (_last_fetch_ms == 0 || now - _last_fetch_ms > 5u * 60 * 1000u) {
        _last_fetch_ms = now;
        _fetch();
    }
    // worker 有新数据 → 重建当前页签列表 (UI 线程)
    if (_data_ready.exchange(false)) {
        _renderList();
    }
}

void AppNews::onClose()
{
    mclog::tagInfo(TAG, "on close");
    _closeDetail();
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
    for (auto& b : _tab_btns) b = nullptr;
    _scroll = nullptr;
    _status_lbl = nullptr;
}

// ── UI ──────────────────────────────────────────────────────────────────────
void AppNews::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部 8 源页签 (car-dash 恒显全部源; CHECKABLE)。
    // 2026-11-05 三轮定稿: 一行横滚 (用户: 不要两行, 可左右拉动), 行区高 90
    // = 两行版总高 128 的 70%; 按钮 240x74 字 30px, 总宽 1976>1248 可拖
    constexpr int TAB_W = 240, TAB_H = 74, TAB_GAP = 8, TAB_X = 16, TAB_Y = 8;
    constexpr int TAB_AREA_H = 90;
    lv_obj_t* tabrow = lv_obj_create(_scr);
    lv_obj_set_pos(tabrow, TAB_X, TAB_Y);
    lv_obj_set_size(tabrow, 1280 - TAB_X * 2, TAB_AREA_H);
    lv_obj_set_style_bg_opa(tabrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabrow, 0, 0);
    lv_obj_set_style_pad_all(tabrow, 0, 0);
    lv_obj_set_style_pad_column(tabrow, TAB_GAP, 0);
    lv_obj_set_flex_flow(tabrow, LV_FLEX_FLOW_ROW);   // 不换行, 溢出=横向滚动
    lv_obj_set_flex_align(tabrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(tabrow, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tabrow, LV_SCROLLBAR_MODE_ACTIVE);

    for (int i = 0; i < kTabs && i < newsfeed::kSourceCount; i++) {
        lv_obj_t* btn = lv_button_create(tabrow);
        lv_obj_set_size(btn, TAB_W, TAB_H);
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
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* self = static_cast<AppNews*>(lv_event_get_user_data(e));
            self->_setTab((int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, newsfeed::kSources[i].name);
        lv_obj_set_style_text_font(l, zh30(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(theme::cur().text_dim), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_STATE_CHECKED);
        lv_obj_center(l);
        _tab_btns[i] = btn;
    }

    // 右下角: 更新时间 (页签行右侧无空位 — 8 页签已铺满, 右上曾与末签重叠)
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

    _setTab(0);
}

void AppNews::_setTab(int tab)
{
    if (tab < 0 || tab >= kTabs) tab = 0;
    _tab = tab;
    for (int i = 0; i < kTabs; i++) {
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
    {
        std::lock_guard<std::mutex> lk(_mu);
        if (_tab < (int)_feeds.size()) fd = _feeds[_tab];
        fetched = _fetched;
    }
    std::string st_txt = fetched.empty() ? std::string() : "更新 " + fetched;
    if (_status_lbl) lv_label_set_text(_status_lbl, st_txt.c_str());

    // 重建: 清空滚动容器再逐条建卡 (页签切换零请求, 数据来自同一次拉取)
    lv_obj_clean(_scroll);

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

// ── 数据拉取 (worker 只写缓存, 绝不碰 LVGL — app_health 模式) ────────────────
void AppNews::_fetch()
{
    if (_fetch_in_flight.exchange(true)) return;   // 防重入
    const bool ok = GetHAL()->tryRunDetached([this]() {
        std::vector<FeedData> out(newsfeed::kSourceCount);
        std::string fetched;
        do {
            const std::string base =
                "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
            auto resp = GetHAL()->httpGet(base + "/news/feeds");
            if (!resp.ok || resp.status != 200) {
                mclog::tagWarn(TAG, "fetch failed: HTTP {}", resp.status);
                break;   // 失败保持原值
            }
            nlohmann::json j;
            try {
                j = nlohmann::json::parse(resp.body);
            } catch (...) {
                mclog::tagWarn(TAG, "json parse failed");
                break;
            }
            if (!j.value("ok", false) || !j.contains("feeds") || !j["feeds"].is_object()) {
                break;
            }
            const auto& feeds = j["feeds"];
            for (int i = 0; i < newsfeed::kSourceCount; i++) {
                const auto& src = newsfeed::kSources[i];
                if (!feeds.contains(src.id) || !feeds[src.id].is_object()) continue;
                const auto& f = feeds[src.id];
                out[i].ok   = f.value("ok", false);
                out[i].name = f.value("name", std::string(src.name));
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

// ── 上拨退出 (app_health 同款: GESTURE + 位移兜底, 详情开着只关详情) ──────────
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
                        // 详情弹窗开着: 底缘上拨只关弹窗, 不退出 app
                        if (a && *a && c->app->_detail_modal) {
                            c->app->_closeDetail();
                        } else if (a && *a && c->app->_scr
                                   && lv_screen_active() == c->app->_scr) {
                            c->app->_requestClose();
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
                                if (c->app->_detail_modal) {
                                    c->app->_closeDetail();   // 详情: 关弹窗不退 app
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
