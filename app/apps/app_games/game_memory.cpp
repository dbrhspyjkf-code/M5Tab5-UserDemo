// game_memory — 翻牌配对 (car-dash games/memory 移植, 2026-11-05)
//
// 三档难度 (横屏友好: 列>行): 轻松 4×3 / 标准 6×4 / 挑战 8×5;
// 卡面原版用 emoji — Tab5 无 emoji 字体, 改 GB2312 单汉字 (车/猫/狗…);
// 最佳 = 各难度最少步数 (NVS gm_mem_best_<lvl>)。
// 交互: 点卡翻面, 两张判定 — 配对 300ms 后定格, 不配 700ms 翻回; 判定期间锁输入。
#include "app_games.h"
#include "gmu.h"
#include "gm_audio.h"
#include <cstdio>
#include <cstring>

// 24 个卡面 ≥ 最大 20 对 (挑战 8×5); 全部 GB2312 常用字
const char* AppGames::_MEM_FACES[24] = {
    "车", "猫", "狗", "虎", "熊", "鱼", "鸟", "花",
    "果", "茶", "书", "琴", "星", "月", "日", "山",
    "河", "云", "雨", "雪", "风", "林", "海", "冰",
};

// ── 页面骨架 ──
void AppGames::_buildMemoryPage()
{
    lv_obj_t* pg = _pages[0];
    if (!pg) return;

    // 难度 chips + 重开 (页头行)
    for (int i = 0; i < 3; i++) {
        _mem_chips[i] = gmu::chip_btn(pg, _MEM_LV[i].nm, 24 + i * 130, 6, 120, 44,
                                      C_ACC(), _memChipCb, this);
        lv_obj_set_user_data(_mem_chips[i], (void*)(intptr_t)i);
    }
    gmu::action_btn(pg, "重开", SCREEN_W - 24 - 120, 6, 120, 44, _memNewCb, this);

    _mem_status = gmu::soft_label(pg, "", 24, 58, C_DIM());
    lv_obj_set_width(_mem_status, 700);

    // chips 选中态
    for (int i = 0; i < 3; i++) {
        if (i == _mem_lvl) lv_obj_add_state(_mem_chips[i], LV_STATE_CHECKED);
    }

    _memNew();
}

void AppGames::_memChipCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    int lvl = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (!self || !self->_scr || lvl == self->_mem_lvl) return;
    self->_mem_lvl = lvl;
    for (int i = 0; i < 3; i++) {
        if (i == lvl) lv_obj_add_state(self->_mem_chips[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(self->_mem_chips[i], LV_STATE_CHECKED);
    }
    self->_memNew();
}

void AppGames::_memNewCb(lv_event_t* e)
{
    static_cast<AppGames*>(lv_event_get_user_data(e))->_memNew();
}

// ── 建局 ──
void AppGames::_memNew()
{
    const MemLevel& lv = _MEM_LV[_mem_lvl];
    const int n = lv.cols * lv.rows;
    const int pairs = n / 2;

    // 洗牌: pairs 种图案 ×2
    int deck[40];
    for (int i = 0; i < pairs; i++) {
        deck[i] = i;
        deck[i + pairs] = i;
    }
    for (int i = n - 1; i > 0; i--) {
        int j = gmu::rng_range(i + 1);
        int t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    for (int i = 0; i < n; i++) _mem_deck[i] = deck[i];
    for (int i = 0; i < 40; i++) _mem_state[i] = 0;
    _mem_up0 = _mem_up1 = -1;
    _mem_moves = 0;
    _mem_left = pairs;
    _mem_lock = false;

    if (_mem_timer) { lv_timer_del(_mem_timer); _mem_timer = nullptr; }
    if (_mem_status) lv_obj_add_flag(_mem_status, LV_OBJ_FLAG_HIDDEN);

    // 整卡重建 (切难度/重开)
    const int gx = 24, gy = 76;
    const int gw = SCREEN_W - 48, gh = SCREEN_H - TAB_H - gy - 16;
    const int gap = 10;
    int cell = std::min((gw - (lv.cols - 1) * gap) / lv.cols,
                        (gh - (lv.rows - 1) * gap) / lv.rows);
    if (cell > 220) cell = 220;
    const int bw = lv.cols * cell + (lv.cols - 1) * gap;
    const int bh = lv.rows * cell + (lv.rows - 1) * gap;
    const int x0 = gx + (gw - bw) / 2, y0 = gy + (gh - bh) / 2;

    for (int i = 0; i < 40; i++) {
        if (_mem_cards[i]) { lv_obj_delete(_mem_cards[i]); _mem_cards[i] = nullptr; }
    }
    for (int i = 0; i < n; i++) {
        lv_obj_t* card = lv_obj_create(_pages[0]);
        if (!card) return;
        const int r = i / lv.cols, c = i % lv.cols;
        lv_obj_set_size(card, cell, cell);
        lv_obj_set_pos(card, x0 + c * (cell + gap), y0 + r * (cell + gap));
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, (void*)(intptr_t)i);
        lv_obj_add_event_cb(card, _memClickCb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(card);
        if (l) {
            lv_label_set_text(l, _MEM_FACES[_mem_deck[i]]);
            lv_obj_set_style_text_font(l, gmu::zh30(), 0);
            lv_obj_center(l);
        }
        _mem_cards[i] = card;
        _memRender(i);
    }
    _memRefreshStatus();
}

// ── 卡面渲染 (0 盖 / 1 翻开 / 2 已配) ──
void AppGames::_memRender(int i)
{
    lv_obj_t* card = _mem_cards[i];
    if (!card) return;
    lv_obj_t* l = lv_obj_get_child(card, 0);   // 0 号子对象=卡面字
    if (!l) return;
    switch (_mem_state[i]) {
        case 0:   // 盖着
            lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD()), 0);
            lv_obj_set_style_border_width(card, 0, 0);
            lv_label_set_text(l, "");
            break;
        case 1:   // 翻开待判
            lv_obj_set_style_bg_color(card, lv_color_hex(C_TRACK()), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_ACC()), 0);
            lv_label_set_text(l, _MEM_FACES[_mem_deck[i]]);
            break;
        default:  // 已配对 — 暗格 + ok 色描边
            lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD0()), 0);
            lv_obj_set_style_border_width(card, 2, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_OK()), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(C_DIM()), 0);
            lv_label_set_text(l, _MEM_FACES[_mem_deck[i]]);
            break;
    }
    if (_mem_state[i] == 1) {
        lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT()), 0);
    }
}

void AppGames::_memRefreshStatus()
{
    if (!_mem_status) return;
    char best_key[24];
    snprintf(best_key, sizeof(best_key), "gm_mem_best_%d", _mem_lvl);
    const int best = _nvsInt(best_key);
    char buf[96];
    if (best > 0) snprintf(buf, sizeof(buf), "剩余 %d 对 · 步数 %d · 最佳 %d 步",
                           _mem_left, _mem_moves, best);
    else snprintf(buf, sizeof(buf), "剩余 %d 对 · 步数 %d · 最佳 -",
                  _mem_left, _mem_moves);
    lv_label_set_text(_mem_status, buf);
    lv_obj_clear_flag(_mem_status, LV_OBJ_FLAG_HIDDEN);
}

// ── 点卡 ──
void AppGames::_memClickCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr) return;
    const int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (self->_mem_lock || self->_mem_state[i] != 0) return;
    gm_audio::tone(620, 0.05);

    // 放入翻开对
    if (self->_mem_up0 < 0) self->_mem_up0 = i;
    else if (self->_mem_up1 < 0) self->_mem_up1 = i;
    self->_mem_state[i] = 1;
    self->_memRender(i);

    if (self->_mem_up1 < 0) return;   // 才翻一张

    // 两张齐 — 锁盘 + 延迟判定 (one-shot timer)
    self->_mem_lock = true;
    self->_mem_moves++;
    if (self->_mem_timer) lv_timer_del(self->_mem_timer);
    self->_mem_timer = lv_timer_create(_memFlipTimerCb,
                                       self->_mem_deck[self->_mem_up0] == self->_mem_deck[self->_mem_up1] ? 300 : 700,
                                       self);
    lv_timer_set_repeat_count(self->_mem_timer, 1);
}

void AppGames::_memFlipTimerCb(lv_timer_t* t)
{
    auto* self = static_cast<AppGames*>(lv_timer_get_user_data(t));
    self->_mem_timer = nullptr;
    lv_timer_del(t);
    if (!self || !self->_scr) return;   // 失焦已拆屏

    const int a = self->_mem_up0, b = self->_mem_up1;
    self->_mem_up0 = self->_mem_up1 = -1;
    self->_mem_lock = false;
    if (a < 0 || b < 0) return;

    if (self->_mem_deck[a] == self->_mem_deck[b]) {   // 配对: 定格 + 音效
        self->_mem_state[a] = self->_mem_state[b] = 2;
        self->_mem_left--;
        self->_memRender(a);
        self->_memRender(b);
        gm_audio::tone(784, 0.12);
        self->_memRefreshStatus();
        if (self->_mem_left == 0) {
            if (self->_mem_timer) lv_timer_del(self->_mem_timer);
            self->_mem_timer = lv_timer_create([](lv_timer_t* t2) {
                auto* s = static_cast<AppGames*>(lv_timer_get_user_data(t2));
                s->_mem_timer = nullptr;
                lv_timer_del(t2);
                if (s && s->_scr) s->_memWin();
            }, 450, self);
            lv_timer_set_repeat_count(self->_mem_timer, 1);
        }
    } else {                                          // 不配: 翻回
        self->_mem_state[a] = self->_mem_state[b] = 0;
        self->_memRender(a);
        self->_memRender(b);
        gm_audio::tone(260, 0.1);
        self->_memRefreshStatus();
    }
}

// ── 通关 ──
void AppGames::_memWin()
{
    gm_audio::jingle((const float[]){523.25f, 659.25f, 783.99f, 1046.5f},
                     (const double[]){0.16, 0.16, 0.16, 0.28}, 4);
    char best_key[24];
    snprintf(best_key, sizeof(best_key), "gm_mem_best_%d", _mem_lvl);
    const int best = _nvsInt(best_key);
    const bool is_best = best <= 0 || _mem_moves < best;
    if (is_best) _nvsSet(best_key, _mem_moves);
    _memRefreshStatus();

    char body[96];
    if (is_best) snprintf(body, sizeof(body), "%d 步 · 新纪录! · %s", _mem_moves,
                          _MEM_LV[_mem_lvl].nm);
    else snprintf(body, sizeof(body), "%d 步 · 最佳 %d 步 · %s", _mem_moves, best,
                  _MEM_LV[_mem_lvl].nm);
    _showModal("通关!", body, "再来一局", "继续看看", nullptr);
}
