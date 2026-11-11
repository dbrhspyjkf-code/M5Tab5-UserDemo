#include "app_gallery.h"
#include "../utils/rgb565_fetch.h"
#include "../utils/theme/theme.h"   // 2026-11-09 屏底接全局主题
#include <mooncake_log.h>
#include <hal/hal.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef PLATFORM_BUILD_DESKTOP
#include "../utils/assets/assets_partition.h"
#include <cbin_font.h>
extern const uint8_t font_puhui_common_30_4_bin_start[]
    asm("_binary_font_puhui_common_30_4_bin_start");
// 30px 全字库 (标题)
static const lv_font_t* zh30()
{
    static lv_font_t* f = cbin_font_create((uint8_t*)font_puhui_common_30_4_bin_start);
    return f;
}
// 24px (状态/按钮/序号; res 分区缺失时退回 30px)
static const lv_font_t* zh24()
{
    static const lv_font_t* f = []() -> const lv_font_t* {
        const uint8_t* p = assets_part::find("font_zh_24");
        return p ? cbin_font_create(const_cast<uint8_t*>(p)) : zh30();
    }();
    return f;
}
#elif defined(PLATFORM_BUILD_WEB)
// web: 三档 cbin 与真机逐字节一致 (资产镜像)
#include "web_fonts.h"
static const lv_font_t* zh30() { return webui::font_30(); }
static const lv_font_t* zh24() { return webui::font_24(); }
#else
extern "C" const lv_font_t font_puhui_20_4;
static const lv_font_t* zh30() { return &font_puhui_20_4; }
static const lv_font_t* zh24() { return &font_puhui_20_4; }
#endif

static const std::string TAG = "gallery";

// 深色看图风 (app_health 同款: 黑底 + #1C1C1E 占位卡)
static constexpr uint32_t COL_BG    = 0x000000;
static constexpr uint32_t COL_CARD  = 0x1C1C1E;
static constexpr uint32_t COL_WHITE = 0xFFFFFF;
static constexpr uint32_t COL_GRAY  = 0x98989F;

// 布局: 顶栏 64 + 网格区 656; 4 列 296×166 (16:9, 与工具页同列宽), 左右边距 24
static constexpr int TOP_H = 64;
static constexpr int THUMB_W = 296, THUMB_H = 166;
static constexpr int GRID_COLS = 4, GRID_X0 = 24, GRID_Y0 = 8, GRID_GAP = 16;
static constexpr int FULL_W = 1280, FULL_H = 720;
static constexpr int MAX_ITEMS = 96;   // 缩略图全量常驻上限 (96 × 98KB ≈ 9.4MB PSRAM)
static constexpr int EDGE_TAP = 200;   // 全屏页左右边缘点按翻页区宽

// 全屏页占位: 1×1 黑像素。无缓存时换上它, 保证 lv_image 从不引用将被淘汰释放的缓冲。
static const lv_image_dsc_t* empty_dsc()
{
    static uint16_t px = 0;
    static lv_image_dsc_t d = [] {
        lv_image_dsc_t x;
        memset(&x, 0, sizeof(x));
        x.header.magic = LV_IMAGE_HEADER_MAGIC;
        x.header.cf = LV_COLOR_FORMAT_RGB565;
        x.header.w = 1;
        x.header.h = 1;
        x.header.stride = 2;
        x.data_size = 2;
        x.data = reinterpret_cast<const uint8_t*>(&px);
        return x;
    }();
    return &d;
}

namespace {
// 底缘上拨手势上下文 (匿名命名空间: 防与其他 app 同名 ctx 跨编译单元错配)
struct GlSwipeCtx {
    AppGallery* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
    int scroll_at_press = 0;
    bool viewer_at_press = false;
};
}  // namespace

// ── 生命周期 ────────────────────────────────────────────────────────────────
AppGallery::AppGallery()
{
    setAppInfo().name = "照片";
}

void AppGallery::onOpen()
{
#ifdef PLATFORM_BUILD_WEB
    GetHAL()->uartMonitorSend("gal-open");
#endif

    mclog::tagInfo(TAG, "on open");
    if (_scr) _teardown();                   // 残留屏 (失焦收尾未及执行) 先拆, 幂等重建
    _alive = std::make_shared<bool>(true);   // 成对铁律: 拆屏置 false 后此处必须重建
    _buildUi();
    lv_screen_load(_scr);                    // 必须: 不 load 则 onRunning 误判失焦
    _installSwipeGesture();
    if (_block) _block(true);
    _list_retry_ms = 0;                      // onRunning 下一帧拉清单
#ifdef PLATFORM_BUILD_DESKTOP
    // 桌面调试: TAB5_GALLERY_VIEW=<序号> 清单到达后直开全屏 (SDL 合成点击不可靠)
    if (const char* v = getenv("TAB5_GALLERY_VIEW")) _pending_view = atoi(v);
#endif
}

void AppGallery::onRunning()
{
    // 失焦收尾 (app_health 模式): openApp(工具页) 不关本 app, 自己发现被切走
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down");
        _teardown();
        return;
    }
    if (!_scr) return;
    if (_list_ready.exchange(false)) _applyList();
    if (!_list_loaded && !_list_in_flight.load() && GetHAL()->millis() >= _list_retry_ms) {
        _fetchList();
    }
    _drainThumbs();
    _pumpFull();
}

void AppGallery::onClose()
{
    mclog::tagInfo(TAG, "on close");
    _teardown();
}

void AppGallery::_teardown()
{
    _gen.fetch_add(1);                       // 在途 worker 结果作废 (由 worker 自行释放)
    _alive = std::make_shared<bool>(false);  // 作废手势/延迟回调
    _removeSwipeGesture();
    if (_block) _block(false);

    // 收拢全部像素缓冲, 待删屏后再释放 (先删引用它们的 lv_image)
    auto* bufs = new std::vector<void*>();
    for (void* b : _thumb_buf)
        if (b) bufs->push_back(b);
    for (Slot& s : _slots) {
        if (s.buf) bufs->push_back(s.buf);
        s = Slot{};
    }
    {
        std::lock_guard<std::mutex> lk(_mu);
        for (auto& r : _thumb_q) bufs->push_back(r.second);
        for (auto& r : _full_q) bufs->push_back(r.second);
        _thumb_q.clear();
        _full_q.clear();
        _full_failed = -1;
        _list_pending.clear();
        _list_ready = false;
    }
    _items.clear();
    _tile_img.clear();
    _thumb_buf.clear();
    _list_loaded = false;
    _thumb_started = false;
    _pending_view = -1;
    _viewer = _viewer_img = _viewer_idx = _viewer_back = _viewer_loading = nullptr;
    _view_idx = -1;
    _full_fail_idx = -1;

    struct Doomed {
        lv_obj_t* scr;
        std::vector<void*>* bufs;
    };
    auto* d = new Doomed{_scr, bufs};
    _scr = nullptr;
    _scroll = nullptr;
    _status_lbl = nullptr;
    // 异步: 可能在本屏事件派发中被调用, 且不能删活动屏
    lv_async_call([](void* p) {
        auto* d = static_cast<Doomed*>(p);
        if (d->scr) lv_obj_delete(d->scr);
        for (void* b : *d->bufs) rgb565::release(b);
        delete d->bufs;
        delete d;
    }, d);
}

// ── UI ──────────────────────────────────────────────────────────────────────
void AppGallery::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(theme::cur().page), 0);
    lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(_scr);
    lv_label_set_text(title, "照片");
    lv_obj_set_style_text_font(title, zh30(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(theme::cur().text_hi), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 14);

    _status_lbl = lv_label_create(_scr);
    lv_label_set_text(_status_lbl, "加载中...");
    lv_obj_set_style_text_font(_status_lbl, zh24(), 0);
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(theme::cur().text_soft), 0);
    lv_obj_align(_status_lbl, LV_ALIGN_TOP_RIGHT, -24, 20);

    // 网格滚动容器 (app_health 同款写法)
    _scroll = lv_obj_create(_scr);
    lv_obj_set_size(_scroll, FULL_W, FULL_H - TOP_H);
    lv_obj_set_pos(_scroll, 0, TOP_H);
    lv_obj_set_style_bg_opa(_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_scroll, 0, 0);
    lv_obj_set_style_radius(_scroll, 0, 0);
    lv_obj_set_style_pad_all(_scroll, 0, 0);
    lv_obj_set_style_pad_bottom(_scroll, 24, 0);   // 末行不贴屏底
    lv_obj_set_scroll_dir(_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_scroll, LV_SCROLLBAR_MODE_ACTIVE);
    // 关橡皮筋 (app_lora_chat 同款): 照片少时内容比容器短, 弹性拖动会让
    // scroll_y 位移 >10px, 底缘上拨被误判成滚动而不退出 (2026-11-02 真机实锤)
    lv_obj_clear_flag(_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
}

void AppGallery::_setStatus(const char* txt)
{
    if (_status_lbl) lv_label_set_text(_status_lbl, txt);
}

std::string AppGallery::_base() const
{
    return "http://" + GetHAL()->getConfig("svc_host", "192.168.1.200") + ":8766";
}

// ── 清单 ────────────────────────────────────────────────────────────────────
void AppGallery::_fetchList()
{
    if (_list_in_flight.exchange(true)) return;
    _list_retry_ms = GetHAL()->millis() + 10000;   // 没成功则 10s 后重试
    const uint32_t gen = _gen.load();
    const std::string url = _base() + "/gallery/list";
    if (!GetHAL()->tryRunDetached([this, gen, url]() {
            std::vector<Item> items;
            bool ok = false;
            int total = 0;
            auto resp = GetHAL()->httpGet(url);
#ifdef PLATFORM_BUILD_WEB
            GetHAL()->uartMonitorSend("gal-list sc=" + std::to_string(resp.status) +
                " len=" + std::to_string(resp.body.size()));
#endif
            if (resp.ok && resp.status == 200) {
                try {
                    auto j = nlohmann::json::parse(resp.body);
                    if (j.value("ok", false) && j.contains("items") && j["items"].is_array()) {
                        for (const auto& it : j["items"]) {
                            if (!it.is_object()) continue;
                            total++;
                            if ((int)items.size() >= MAX_ITEMS) continue;
                            Item x;
                            x.id = it.value("id", std::string());
                            x.w = it.value("w", 0);
                            x.h = it.value("h", 0);
                            if (!x.id.empty()) items.push_back(std::move(x));
                        }
                        ok = true;
                    }
                } catch (...) {
                    mclog::tagWarn(TAG, "list json parse failed");
                }
            } else {
                mclog::tagWarn(TAG, "list fetch failed: HTTP {}", resp.status);
            }
            {
                std::lock_guard<std::mutex> lk(_mu);
                if (gen == _gen.load()) {
                    _list_pending = std::move(items);
                    _list_ok = ok;
                    _list_pending_total = total;
                    _list_ready = true;
                }
            }
            _list_in_flight = false;
        })) {
        _list_in_flight = false;   // 起线程失败 (内存紧) 必须回滚, 否则永不再拉
    }
}

void AppGallery::_applyList()
{
    std::vector<Item> items;
    bool ok;
    int total;
    {
        std::lock_guard<std::mutex> lk(_mu);
        items.swap(_list_pending);
        ok = _list_ok;
        total = _list_pending_total;
    }
    if (_list_loaded || !_scroll) return;
    if (!ok) {
        _setStatus("连接失败, 稍后自动重试");
        return;   // _list_retry_ms 已在发起时设为 +10s
    }
    _list_loaded = true;
    _items = std::move(items);
    _list_total = total;
    const int n = (int)_items.size();
    if (n == 0) {
        _setStatus("相簿里还没有照片");
        return;
    }
    char buf[64];
    if (_list_total > n) snprintf(buf, sizeof(buf), "共 %d 张, 显示最新 %d 张", _list_total, n);
    else snprintf(buf, sizeof(buf), "共 %d 张", n);
    _setStatus(buf);

    _tile_img.assign(n, nullptr);
    _thumb_buf.assign(n, nullptr);
    for (int i = 0; i < n; i++) {
        lv_obj_t* card = lv_obj_create(_scroll);
        lv_obj_set_size(card, THUMB_W, THUMB_H);
        lv_obj_set_pos(card, GRID_X0 + (i % GRID_COLS) * (THUMB_W + GRID_GAP),
                       GRID_Y0 + (i / GRID_COLS) * (THUMB_H + GRID_GAP));
        lv_obj_set_style_bg_color(card, lv_color_hex(theme::cur().card[1]), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        // 按下反馈: 白色半透描边外扩 (工具页 tile 同观感)
        lv_obj_set_style_outline_width(card, 4, LV_STATE_PRESSED);
        lv_obj_set_style_outline_color(card, lv_color_hex(theme::cur().accent), LV_STATE_PRESSED);
        lv_obj_set_style_outline_opa(card, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)i);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* self = static_cast<AppGallery*>(lv_event_get_user_data(e));
            lv_obj_t* c = lv_event_get_current_target_obj(e);
            const int idx = (int)(intptr_t)lv_obj_get_user_data(c);
            if (!self->_viewer) self->_openViewer(idx);
        }, LV_EVENT_CLICKED, this);

        lv_obj_t* img = lv_image_create(card);
        lv_obj_set_pos(img, 0, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        _tile_img[i] = img;
    }
    if (_pending_view >= 0) {
        const int v = _pending_view;
        _pending_view = -1;
        if (v < n) _openViewer(v);
    }
}

// ── 缩略图 ──────────────────────────────────────────────────────────────────
void AppGallery::_startThumbWorker()
{
    if (_thumb_running.exchange(true)) return;
    const uint32_t gen = _gen.load();
    std::vector<std::string> ids;
    ids.reserve(_items.size());
    for (const Item& it : _items) ids.push_back(it.id);
    const std::string base = _base();
    if (!GetHAL()->tryRunDetached([this, gen, ids, base]() {
#ifdef PLATFORM_BUILD_WEB
        GetHAL()->uartMonitorSend("gal-thumbs-start n=" + std::to_string(ids.size()));
#endif
            std::vector<int> todo;
            for (int i = 0; i < (int)ids.size(); i++) todo.push_back(i);
            // 顺序拉 (SDIO 友好); 失败的隔 3s 补拉, 最多 3 轮
            for (int round = 0; round < 3 && !todo.empty() && gen == _gen.load(); round++) {
                if (round > 0) GetHAL()->delay(3000);
                std::vector<int> failed;
                for (int i : todo) {
                    if (gen != _gen.load()) break;
                    char q[128];
                    snprintf(q, sizeof(q), "/gallery/thumb.rgb565?id=%s&w=%d&h=%d",
                             ids[i].c_str(), THUMB_W, THUMB_H);
                    void* buf = rgb565::alloc(THUMB_W, THUMB_H);
                    if (!buf || !rgb565::download(base + q, buf)) {
                        rgb565::release(buf);
                        failed.push_back(i);
                        continue;
                    }
                    std::lock_guard<std::mutex> lk(_mu);
                    if (gen == _gen.load()) _thumb_q.emplace_back(i, buf);
                    else rgb565::release(buf);
                }
                todo.swap(failed);
            }
            _thumb_running = false;
        })) {
        _thumb_running = false;
        _thumb_started = false;   // 起线程失败: 下一帧 _drainThumbs 重试
    }
}

void AppGallery::_drainThumbs()
{
    std::deque<std::pair<int, void*>> ready;
    {
        std::lock_guard<std::mutex> lk(_mu);
        ready.swap(_thumb_q);
    }
    for (auto& r : ready) {
        const int i = r.first;
        if (i >= 0 && i < (int)_tile_img.size() && _tile_img[i] && !_thumb_buf[i]) {
            _thumb_buf[i] = r.second;
            lv_image_set_src(_tile_img[i], r.second);
        } else {
            rgb565::release(r.second);
        }
    }
    // 上一会话的 worker 可能还没退出 (_thumb_running), 等它收工再起新的
    if (_list_loaded && !_items.empty() && !_thumb_started && !_thumb_running.load()) {
        _thumb_started = true;
        _startThumbWorker();
    }
}

// ── 全屏大图 ────────────────────────────────────────────────────────────────
int AppGallery::_slotOf(int idx) const
{
    for (const Slot& s : _slots)
        if (s.buf && s.idx == idx) return &s - _slots;
    return -1;
}

void AppGallery::_storeSlot(int idx, void* buf)
{
    const int n = (int)_items.size();
    // 保留集 = 当前 ±2 (翻页行进方向上永远有 2 张热图)
    auto keep = [&](int i) {
        for (int d = -2; d <= 2; d++)
            if (i == ((_view_idx + d) % n + n) % n) return true;
        return false;
    };
    int victim = -1;
    for (int s = 0; s < 5 && victim < 0; s++)
        if (!_slots[s].buf) victim = s;
    for (int s = 0; s < 5 && victim < 0; s++)
        if (!keep(_slots[s].idx)) victim = s;
    if (victim < 0) {   // 理论不达: 想要的序号最多 5 个且 idx 尚无缓存
        rgb565::release(buf);
        return;
    }
    // 淘汰的必不在保留集 (当前显示的序号恒在其中; 未缓存时显示的是 empty_dsc)
    rgb565::release(_slots[victim].buf);
    _slots[victim] = Slot{idx, buf};
}

void AppGallery::_fetchFull(int idx)
{
    if (_full_in_flight.exchange(true)) return;
    const uint32_t gen = _gen.load();
    const std::string url = _base() + "/gallery/photo.rgb565?id=" + _items[idx].id;
    if (!GetHAL()->tryRunDetached([this, gen, idx, url]() {
            void* buf = rgb565::alloc(FULL_W, FULL_H);
            if (buf && !rgb565::download(url, buf)) {
                rgb565::release(buf);
                buf = nullptr;
            }
            {
                std::lock_guard<std::mutex> lk(_mu);
                if (gen == _gen.load()) {
                    if (buf) _full_q.emplace_back(idx, buf);
                    else _full_failed = idx;
                    buf = nullptr;
                }
            }
            rgb565::release(buf);   // 会话已换代: 丢弃
            _full_in_flight = false;
        })) {
        _full_in_flight = false;   // 起线程失败 (内存紧): 退避后重试
        _full_fail_idx = idx;
        _full_retry_ms = GetHAL()->millis() + 3000;
    }
}

void AppGallery::_pumpFull()
{
    std::deque<std::pair<int, void*>> got;
    int failed;
    {
        std::lock_guard<std::mutex> lk(_mu);
        got.swap(_full_q);
        failed = _full_failed;
        _full_failed = -1;
    }
    const uint32_t now = GetHAL()->millis();
    if (failed >= 0) {
        _full_fail_idx = failed;
        _full_retry_ms = now + 3000;
        if (_viewer && failed == _view_idx)
            lv_label_set_text(_viewer_loading, "加载失败, 稍后自动重试");
    }
    for (auto& r : got) {
        if (_viewer && r.first >= 0 && r.first < (int)_items.size() && _slotOf(r.first) < 0) {
            _storeSlot(r.first, r.second);
            if (r.first == _view_idx) _showIndex(_view_idx);
        } else {
            rgb565::release(r.second);
        }
    }
    // 调度下一张: 按翻页方向优先 (行进方向 2 张 → 反方向 2 张), 一次一张 (SDIO 友好)
    if (!_viewer || _full_in_flight.load() || _items.empty()) return;
    const int n = (int)_items.size();
    const int dir = _last_dir >= 0 ? 1 : -1;
    const int want[5] = {
        _view_idx,
        ((_view_idx + dir) % n + n) % n,
        ((_view_idx + 2 * dir) % n + n) % n,
        ((_view_idx - dir) % n + n) % n,
        ((_view_idx - 2 * dir) % n + n) % n,
    };
    for (int idx : want) {
        if (_slotOf(idx) >= 0) continue;
        // 失败退避只挡它自己, 其他候选照常预取
        if (idx == _full_fail_idx && now < _full_retry_ms) continue;
        _fetchFull(idx);
        return;
    }
}

// ── 全屏查看 ────────────────────────────────────────────────────────────────
void AppGallery::_openViewer(int idx)
{
    if (!_scr || _viewer || idx < 0 || idx >= (int)_items.size()) return;
    _viewer = lv_obj_create(_scr);
    lv_obj_set_size(_viewer, FULL_W, FULL_H);
    lv_obj_set_pos(_viewer, 0, 0);
    lv_obj_set_style_bg_color(_viewer, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_viewer, 0, 0);
    lv_obj_set_style_border_width(_viewer, 0, 0);
    lv_obj_set_style_pad_all(_viewer, 0, 0);
    lv_obj_clear_flag(_viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_viewer, LV_OBJ_FLAG_CLICKABLE);
    // 对象级 (SDL 模拟器 indev 级 PRESSED/RELEASED 不派发, 对象级可用)
    lv_obj_add_event_cb(_viewer, _viewer_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(_viewer, _viewer_cb, LV_EVENT_RELEASED, this);

    _viewer_img = lv_image_create(_viewer);
    lv_obj_set_pos(_viewer_img, 0, 0);
    lv_obj_clear_flag(_viewer_img, LV_OBJ_FLAG_CLICKABLE);

    _viewer_loading = lv_label_create(_viewer);
    lv_obj_set_style_text_font(_viewer_loading, zh24(), 0);
    lv_obj_set_style_text_color(_viewer_loading, lv_color_hex(COL_GRAY), 0);
    lv_obj_center(_viewer_loading);
    lv_obj_clear_flag(_viewer_loading, LV_OBJ_FLAG_CLICKABLE);

    // 「3 / 8」半透明药丸 (右上)
    _viewer_idx = lv_label_create(_viewer);
    lv_obj_set_style_text_font(_viewer_idx, zh24(), 0);
    lv_obj_set_style_text_color(_viewer_idx, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_bg_color(_viewer_idx, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_viewer_idx, LV_OPA_50, 0);
    lv_obj_set_style_radius(_viewer_idx, 20, 0);
    lv_obj_set_style_pad_hor(_viewer_idx, 18, 0);
    lv_obj_set_style_pad_ver(_viewer_idx, 8, 0);
    lv_obj_align(_viewer_idx, LV_ALIGN_TOP_RIGHT, -24, 24);
    lv_obj_clear_flag(_viewer_idx, LV_OBJ_FLAG_CLICKABLE);

    // 「返回」半透明药丸按钮 (左上; 模拟器无 indev 级上拨, 靠它关全屏)
    _viewer_back = lv_obj_create(_viewer);
    lv_obj_set_size(_viewer_back, 120, 52);
    lv_obj_set_pos(_viewer_back, 24, 20);
    lv_obj_set_style_bg_color(_viewer_back, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(_viewer_back, LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(_viewer_back, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_set_style_radius(_viewer_back, 26, 0);
    lv_obj_set_style_border_width(_viewer_back, 0, 0);
    lv_obj_set_style_shadow_width(_viewer_back, 0, 0);
    lv_obj_set_style_pad_all(_viewer_back, 0, 0);
    lv_obj_clear_flag(_viewer_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_viewer_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* back_lbl = lv_label_create(_viewer_back);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_set_style_text_font(back_lbl, zh24(), 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(COL_WHITE), 0);
    lv_obj_center(back_lbl);
    lv_obj_clear_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);
    // 回调里删祖先 (全屏页) 必须异步, 不能删正在派发事件的对象
    lv_obj_add_event_cb(_viewer_back, [](lv_event_t* e) {
        lv_async_call([](void* p) { static_cast<AppGallery*>(p)->_closeViewer(); },
                      lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, this);

    _chrome = true;
    _full_fail_idx = -1;
    _showIndex(idx);
}

void AppGallery::_closeViewer()
{
    if (!_viewer) return;
    lv_obj_delete(_viewer);   // 大图槽保留, 再开秒显
    _viewer = _viewer_img = _viewer_idx = _viewer_back = _viewer_loading = nullptr;
    _view_idx = -1;
}

void AppGallery::_showIndex(int idx)
{
    if (!_viewer) return;
    _view_idx = idx;
    const int s = _slotOf(idx);
    if (s >= 0) {
        lv_image_set_src(_viewer_img, _slots[s].buf);
        lv_obj_add_flag(_viewer_loading, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(_viewer_img, empty_dsc());
        lv_label_set_text(_viewer_loading,
                          idx == _full_fail_idx ? "加载失败, 稍后自动重试" : "加载中...");
        lv_obj_clear_flag(_viewer_loading, LV_OBJ_FLAG_HIDDEN);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%d / %d", idx + 1, (int)_items.size());
    lv_label_set_text(_viewer_idx, buf);
}

void AppGallery::_step(int delta)
{
    const int n = (int)_items.size();
    if (!_viewer || n <= 1) return;
    const uint32_t now = GetHAL()->millis();
    if (now - _last_flip_ms < 250) return;   // 去重 (抖动/连点)
    _last_flip_ms = now;
    _last_dir = delta >= 0 ? 1 : -1;         // 记住行进方向, 预取优先
    _showIndex(((_view_idx + delta) % n + n) % n);
}

void AppGallery::_toggleChrome()
{
    if (!_viewer) return;
    _chrome = !_chrome;
    for (lv_obj_t* o : {_viewer_idx, _viewer_back}) {
        if (_chrome) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppGallery::_viewer_cb(lv_event_t* e)
{
    auto* self = static_cast<AppGallery*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (!self->_viewer || !indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->_press = p;
        return;
    }
    const int dx = p.x - self->_press.x, dy = p.y - self->_press.y;
    if (self->_press.y > 650 && dy < -60) return;      // 底缘上拨: 交给 indev 级手势关全屏
    if (abs(dx) > 90 && abs(dx) > abs(dy) * 2) {        // 横滑翻页: 左滑 = 下一张
        self->_step(dx < 0 ? 1 : -1);
    } else if (abs(dx) < 24 && abs(dy) < 24) {          // 点按: 左右边缘翻页, 中间显隐
        if (p.x < EDGE_TAP) self->_step(-1);
        else if (p.x > FULL_W - EDGE_TAP) self->_step(1);
        else self->_toggleChrome();
    }
}

// ── 手势: 底缘上拨 (全屏页 = 关全屏, 网格页 = 回工具页) ─────────────────────
void AppGallery::_requestExitOnce()
{
    const uint32_t now = GetHAL()->millis();
    if (_last_exit_ms && now - _last_exit_ms < 500) return;   // GESTURE + RELEASED 双轨去重
    _last_exit_ms = now;
    _requestClose();
}

void AppGallery::_installSwipeGesture()
{
    _removeSwipeGesture();
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        _gesture_ctx = new GlSwipeCtx{this, _alive};
        // 单回调收 PRESSED (记起点/全屏页是否开着/滚动位) + GESTURE (快滑) +
        // RELEASED (位移兜底慢滑; PRESSING 根本不达 indev 回调)
        lv_indev_add_event_cb(indev, [](lv_event_t* e) {
            auto* ctx = static_cast<GlSwipeCtx*>(lv_event_get_user_data(e));
            auto alive = ctx->alive.lock();
            if (!alive || !*alive) return;
            AppGallery* app = ctx->app;
            if (!app->_scr || lv_screen_active() != app->_scr) return;   // 前台守卫
            lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
            const lv_event_code_t code = lv_event_get_code(e);
            lv_point_t p;
            if (code == LV_EVENT_PRESSED) {
                lv_indev_get_point(dev, &p);
                ctx->press_x = p.x;
                ctx->press_y = p.y;
                ctx->viewer_at_press = app->_viewer != nullptr;
                ctx->scroll_at_press = app->_scroll ? lv_obj_get_scroll_y(app->_scroll) : 0;
                return;
            }
            bool hit = false;
            if (code == LV_EVENT_GESTURE) {
                hit = lv_indev_get_gesture_dir(dev) == LV_DIR_TOP && ctx->press_y > 650;
            } else if (code == LV_EVENT_RELEASED) {
                lv_indev_get_point(dev, &p);
                const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                hit = ctx->press_y > 650 && dy < -120 && abs(dy) > abs(dx) * 2;
            }
            if (!hit) return;
            // 网格滚动中的上拨不算退出 (app_lottery 同款: 滚动位移 >10px)
            if (!ctx->viewer_at_press && app->_scroll &&
                abs((int)lv_obj_get_scroll_y(app->_scroll) - ctx->scroll_at_press) > 10) return;
            auto* defer = new GlSwipeCtx{app, ctx->alive, 0, 0, 0, ctx->viewer_at_press};
            lv_async_call([](void* u) {
                auto* c = static_cast<GlSwipeCtx*>(u);
                auto a = c->alive.lock();
                if (a && *a && c->app->_scr && lv_screen_active() == c->app->_scr) {
                    // 按下时全屏页开着 → 只关全屏页 (另一轨/对象级先关了也不会误退出 app)
                    if (c->viewer_at_press) c->app->_closeViewer();
                    else c->app->_requestExitOnce();
                }
                delete c;
            }, defer);
        }, LV_EVENT_ALL, _gesture_ctx);
        _gesture_indev = indev;
        break;
    }
}

void AppGallery::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
    }
    // ctx 不 free: 同 app_health/app_stocks 惯例 (几十字节/次)
}
