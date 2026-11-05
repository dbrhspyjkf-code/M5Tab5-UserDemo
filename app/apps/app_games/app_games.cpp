#include "app_games.h"

#include <mooncake_log.h>
#include <hal/hal.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "gmu.h"
#include "gm_audio.h"

static const char* TAG = "games";

// SwipeCtx: 定义+分配+使用一体 (app_lottery/stocks 同款, 跨编译单元强转别名
// = 定时炸弹, 万年历 7fa88f3 教训)
struct SwipeCtx {
    AppGames* app;
    std::weak_ptr<bool> alive;
    int press_x = -1, press_y = -1;
};

// ════════════════════════════════════════════════════════════════════════════
//  生命周期
// ════════════════════════════════════════════════════════════════════════════
AppGames::AppGames()
{
    setAppInfo().name = "游戏";
}

void AppGames::onOpen()
{
    mclog::tagInfo(TAG, "opened");
    if (_scr) _destroyUi();   // 幂等: 清掉上次遗留 UI
    // 手势 alive 旗标成对铁律 (b275e71): onClose 置 false 后 onOpen 必须重建,
    // 否则第二次进入上拨静默失效。
    _alive = std::make_shared<bool>(true);
    _buildUi();
    lv_screen_load(_scr);   // 必须: 不 load 则活动屏还是旧屏, onRunning 会误判失焦
    _installSwipeGesture();

    _tick = lv_timer_create(_tickCb, 1000, this);

    // 桌面调试: TAB5_GAMES_TAB=0..3 直落指定游戏 (SDL 合成点击不可靠, 同
    // TAB5_LOTTERY_TAB 惯例)
#ifdef PLATFORM_BUILD_DESKTOP
    if (const char* t = getenv("TAB5_GAMES_TAB")) {
        int tab = atoi(t);
        if (tab >= 0 && tab <= 3) _setTab(tab);
    }
#endif
}

void AppGames::onRunning()
{
    // 失焦收尾 (mooncake openApp 其他 app 不触发 onClose, 自己检测, lottery 同款;
    // async 删屏防「删除活动屏」崩溃, app_calendar 模式)
    if (_scr && lv_screen_active() != _scr) {
        mclog::tagInfo(TAG, "lost foreground, tearing down screen");
        _removeSwipeGesture();
        _teardownTimers();
        _popKillAnims();
        lv_obj_t* scr = _scr;
        _nullUiPtrs();
        _scr = nullptr;
        lv_async_call([](void* p) { lv_obj_delete(static_cast<lv_obj_t*>(p)); }, scr);
        return;
    }
    if (!_scr) return;

    // 数独 worker 完成 → 上屏 (LVGL 全在 UI 线程)
    if (_sud_ready.exchange(false)) _sudApply();
}

void AppGames::onClose()
{
    mclog::tagInfo(TAG, "closed");
    _destroyUi();
    _alive = std::make_shared<bool>(false);   // 作废在途 worker
}

void AppGames::_requestExitOnce()
{
    int64_t now = gmu::mono_ms();
    if (_last_exit_ms != 0 && now - _last_exit_ms < 500) return;   // 双轨去重
    _last_exit_ms = now;
    _requestClose();
}

// ════════════════════════════════════════════════════════════════════════════
//  骨架 / tab / 计时
// ════════════════════════════════════════════════════════════════════════════
void AppGames::_buildUi()
{
    _scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG()), 0);

    // 顶部 tab 栏: 4 个 CHECKABLE 按钮 (lottery 3 tab 同款)
    const char* names[4] = {"翻牌", "星星", "扫雷", "数独"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = gmu::chip_btn(_scr, names[i], 40 + i * 196, 8, 180, 48,
                                      C_ACC(),
                                      [](lv_event_t* e) {
                                          auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
                                          self->_setTab((int)(intptr_t)lv_obj_get_user_data(
                                              lv_event_get_current_target_obj(e)));
                                      },
                                      this, gmu::zh30());
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        _tab_btns[i] = btn;
    }

    // 4 个全宽页面容器 (y=64 起), 常驻不销毁 — 切 tab 保局面
    for (int i = 0; i < 4; i++) {
        lv_obj_t* pg = lv_obj_create(_scr);
        if (!pg) return;   // coredump 实锤: LVGL 低内存返回 NULL 不可解引用
        lv_obj_set_size(pg, SCREEN_W, SCREEN_H - TAB_H);
        lv_obj_set_pos(pg, 0, TAB_H);
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
        _pages[i] = pg;
    }

    _buildMemoryPage();
    _buildPopPage();
    _buildMinesPage();
    _buildSudokuPage();

    _setTab(0);
}

void AppGames::_setTab(int tab)
{
    _tab = tab;
    if (tab < 0 || tab > 3 || !_pages[tab]) return;
    for (int i = 0; i < 4; i++) {
        if (_tab_btns[i]) {
            if (i == tab) lv_obj_add_state(_tab_btns[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(_tab_btns[i], LV_STATE_CHECKED);
        }
        if (_pages[i]) lv_obj_add_flag(_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (_pages[tab]) lv_obj_clear_flag(_pages[tab], LV_OBJ_FLAG_HIDDEN);
}

void AppGames::_tickCb(lv_timer_t* t)
{
    auto* self = static_cast<AppGames*>(lv_timer_get_user_data(t));
    if (!self || !self->_scr) return;
    char buf[16];

    // 扫雷用时 (只在扫雷 tab 前台 + 已开局未结束 + 无 modal 时走秒)
    if (self->_tab == 2 && self->_min_started && !self->_min_over && !self->_modalOpen() &&
        self->_min_time_lbl) {
        self->_min_sec++;
        snprintf(buf, sizeof(buf), "%d:%02d", self->_min_sec / 60, self->_min_sec % 60);
        lv_label_set_text(self->_min_time_lbl, buf);
    }
    // 数独用时
    if (self->_tab == 3 && self->_sud_started && !self->_sud_over && !self->_modalOpen() &&
        self->_sud_time_lbl) {
        self->_sud_sec++;
        snprintf(buf, sizeof(buf), "%d:%02d", self->_sud_sec / 60, self->_sud_sec % 60);
        lv_label_set_text(self->_sud_time_lbl, buf);
    }
}

void AppGames::_teardownTimers()
{
    if (_tick) { lv_timer_del(_tick); _tick = nullptr; }
    if (_mem_timer) { lv_timer_del(_mem_timer); _mem_timer = nullptr; }
    if (_pop_t1) { lv_timer_del(_pop_t1); _pop_t1 = nullptr; }
    if (_pop_t2) { lv_timer_del(_pop_t2); _pop_t2 = nullptr; }
}

// ════════════════════════════════════════════════════════════════════════════
//  modal (结算 / 重开确认 共用)
// ════════════════════════════════════════════════════════════════════════════
void AppGames::_showModal(const char* title, const char* body, const char* yes_txt,
                          const char* no_txt, std::function<void()> on_yes)
{
    if (!_scr) return;
    _hideModal();
    _modal_yes = std::move(on_yes);

    // 全屏遮罩: 暗化 + CLICKABLE 吃掉穿透点击 (点遮罩=取消)
    _modal = lv_obj_create(_scr);
    if (!_modal) return;
    lv_obj_set_size(_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(_modal, 0, 0);
    lv_obj_set_style_bg_color(_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_modal, 0, 0);
    lv_obj_set_style_pad_all(_modal, 0, 0);
    lv_obj_clear_flag(_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_modal, [](lv_event_t* e) {
        static_cast<AppGames*>(lv_event_get_user_data(e))->_hideModal();
    }, LV_EVENT_CLICKED, this);

    _modal_card = lv_obj_create(_modal);
    if (!_modal_card) return;
    lv_obj_set_size(_modal_card, 640, 380);
    lv_obj_align(_modal_card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_modal_card, lv_color_hex(C_CARD()), 0);
    lv_obj_set_style_radius(_modal_card, 24, 0);
    lv_obj_set_style_border_width(_modal_card, 0, 0);
    lv_obj_set_style_pad_all(_modal_card, 0, 0);
    lv_obj_clear_flag(_modal_card, LV_OBJ_FLAG_SCROLLABLE);
    // 卡面点击吞事件不冒泡 (否则透到遮罩=秒关)
    lv_obj_add_flag(_modal_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_modal_card, [](lv_event_t*) {}, LV_EVENT_CLICKED, nullptr);

    _modal_title = lv_label_create(_modal_card);
    lv_label_set_text(_modal_title, title);
    lv_obj_set_style_text_font(_modal_title, gmu::zh30(), 0);
    lv_obj_set_style_text_color(_modal_title, lv_color_hex(C_TEXT()), 0);
    lv_obj_align(_modal_title, LV_ALIGN_TOP_MID, 0, 56);

    _modal_body = lv_label_create(_modal_card);
    lv_label_set_text(_modal_body, body);
    lv_obj_set_style_text_font(_modal_body, gmu::zh20(), 0);
    lv_obj_set_style_text_color(_modal_body, lv_color_hex(C_SOFT()), 0);
    lv_label_set_long_mode(_modal_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_modal_body, 560);
    lv_obj_align(_modal_body, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_text_align(_modal_body, LV_TEXT_ALIGN_CENTER, 0);

    // 主按钮 (确认/再来一局)
    _modal_btn1 = lv_button_create(_modal_card);
    if (_modal_btn1) {
        lv_obj_set_size(_modal_btn1, 220, 72);
        lv_obj_align(_modal_btn1, LV_ALIGN_BOTTOM_MID, -130, -40);
        lv_obj_set_style_bg_color(_modal_btn1, lv_color_hex(C_ACC()), 0);
        lv_obj_set_style_radius(_modal_btn1, 16, 0);
        lv_obj_add_event_cb(_modal_btn1, [](lv_event_t* e) {
            auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
            auto cb = self->_modal_yes;   // 拷贝: cb 内可能 _hideModal/重建
            self->_hideModal();
            if (cb) cb();
        }, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(_modal_btn1);
        lv_label_set_text(l, yes_txt);
        lv_obj_set_style_text_font(l, gmu::zh30(), 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
        lv_obj_center(l);
    }

    // 次按钮 (继续玩/取消), 无则不建
    if (no_txt && no_txt[0]) {
        _modal_btn2 = lv_button_create(_modal_card);
        if (_modal_btn2) {
            lv_obj_set_size(_modal_btn2, 220, 72);
            lv_obj_align(_modal_btn2, LV_ALIGN_BOTTOM_MID, 130, -40);
            lv_obj_set_style_bg_color(_modal_btn2, lv_color_hex(0x2A2C30), 0);
            lv_obj_set_style_radius(_modal_btn2, 16, 0);
            lv_obj_add_event_cb(_modal_btn2, [](lv_event_t* e) {
                static_cast<AppGames*>(lv_event_get_user_data(e))->_hideModal();
            }, LV_EVENT_CLICKED, this);
            lv_obj_t* l = lv_label_create(_modal_btn2);
            lv_label_set_text(l, no_txt);
            lv_obj_set_style_text_font(l, gmu::zh30(), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT()), 0);
            lv_obj_center(l);
        }
    }
}

void AppGames::_hideModal()
{
    if (_modal) {
        lv_obj_delete(_modal);   // 连带 card/labels/btns
        _modal = nullptr;
    }
    _modal_card = _modal_title = _modal_body = _modal_btn1 = _modal_btn2 = nullptr;
    _modal_yes = nullptr;
}

bool AppGames::_modalOpen() const
{
    return _modal != nullptr && !lv_obj_has_flag(_modal, LV_OBJ_FLAG_HIDDEN);
}

// ════════════════════════════════════════════════════════════════════════════
//  NVS 最佳成绩
// ════════════════════════════════════════════════════════════════════════════
int AppGames::_nvsInt(const char* key)
{
    auto v = GetHAL()->getConfig(key, "");
    return v.empty() ? 0 : atoi(v.c_str());
}

void AppGames::_nvsSet(const char* key, int v)
{
    GetHAL()->setConfig(key, std::to_string(v));
}

// ════════════════════════════════════════════════════════════════════════════
//  手势 (上拨退出回工具页)
// ════════════════════════════════════════════════════════════════════════════
void AppGames::_installSwipeGesture()
{
    _removeSwipeGesture();
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            _gesture_ctx = new SwipeCtx{this, _alive};
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    ctx->press_x = p.x;
                    ctx->press_y = p.y;   // 起点只在 PRESSED 记一次 (铁律①)
                    return;
                }
                if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
                    if (ctx->app->_modalOpen()) return;                    // 弹窗期间屏蔽
                    if (lv_screen_active() != ctx->app->_scr) return;      // 前台守卫
                    lv_point_t p;
                    lv_indev_get_point(dev, &p);
                    const int dx = p.x - ctx->press_x, dy = p.y - ctx->press_y;
                    // 上拨退出: 只认屏幕最下缘起手 (y>650)
                    if (ctx->press_y > 650 && dy < -120 && abs(dy) > abs(dx) * 2) {
                        auto* c = new SwipeCtx{ctx->app, ctx->alive};
                        lv_async_call([](void* u) {
                            auto* cc = static_cast<SwipeCtx*>(u);
                            auto a = cc->alive.lock();
                            if (a && *a) cc->app->_requestExitOnce();
                            delete cc;
                        }, c);
                    }
                    return;
                }
            }, LV_EVENT_ALL, _gesture_ctx);
            lv_indev_add_event_cb(indev, [](lv_event_t* e) {
                auto* ctx = static_cast<SwipeCtx*>(lv_event_get_user_data(e));
                auto alive = ctx->alive.lock();
                if (!alive || !*alive) return;
                if (ctx->app->_modalOpen()) return;
                if (lv_screen_active() != ctx->app->_scr) return;   // 前台守卫
                lv_indev_t* dev = static_cast<lv_indev_t*>(lv_event_get_target(e));
                if (lv_indev_get_gesture_dir(dev) == LV_DIR_TOP) {
                    if (ctx->press_y <= 650) return;
                    auto* c = new SwipeCtx{ctx->app, ctx->alive};
                    lv_async_call([](void* u) {
                        auto* cc = static_cast<SwipeCtx*>(u);
                        auto a = cc->alive.lock();
                        if (a && *a) cc->app->_requestExitOnce();
                        delete cc;
                    }, c);
                }
            }, LV_EVENT_GESTURE, _gesture_ctx);
            _gesture_indev = indev;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

void AppGames::_removeSwipeGesture()
{
    if (_gesture_indev) {
        lv_indev_remove_event_cb_with_user_data(_gesture_indev, nullptr, _gesture_ctx);
        _gesture_indev = nullptr;
        _gesture_ctx = nullptr;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  销毁
// ════════════════════════════════════════════════════════════════════════════
void AppGames::_nullUiPtrs()
{
    for (auto& b : _tab_btns) b = nullptr;
    for (auto& p : _pages) p = nullptr;

    _mem_status = nullptr;
    for (auto& c : _mem_chips) c = nullptr;
    for (auto& c : _mem_cards) c = nullptr;

    _pop_score_lbl = _pop_pick_lbl = nullptr;
    _pop_dying.clear();   // 对象随屏销毁, 只清引用

    for (auto& c : _min_chips) c = nullptr;
    _min_left_lbl = _min_time_lbl = _min_best_lbl = nullptr;
    for (auto& c : _min_cells) c = nullptr;

    for (auto& c : _sud_chips) c = nullptr;
    _sud_note_btn = _sud_time_lbl = _sud_best_lbl = _sud_status = nullptr;
    for (auto& c : _sud_cells) c = nullptr;
    for (auto& c : _sud_lbls) c = nullptr;
    for (auto& c : _sud_num_btns) c = nullptr;
    for (auto& c : _sud_num_cnts) c = nullptr;

    _modal = _modal_card = _modal_title = _modal_body = _modal_btn1 = _modal_btn2 = nullptr;
    _modal_yes = nullptr;
}

void AppGames::_destroyUi()
{
    _removeSwipeGesture();
    _teardownTimers();
    _popKillAnims();
    _modal_yes = nullptr;
    lv_obj_t* scr = _scr;
    _nullUiPtrs();
    _scr = nullptr;
    if (scr) lv_obj_delete(scr);
}
