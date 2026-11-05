// game_mines — 扫雷 (car-dash games/mines 移植, 2026-11-05)
//
// 点开格 / 长按插旗 (全局 indev 长按阈值, stocks/home 已设 500ms, 不改全局);
// 首点必安全 (雷区在首次翻开时生成, 避开首点 3×3); 0 格 BFS 展开;
// 剩余雷数 = 雷总数 − 已插旗; 通关判据 = 非雷格全部翻开。
// 原版 💥🚩 emoji → 矢量现画 (旗=杆+面, 雷=圆盘); 数字 1-8 经典配色。
// 格子子对象按需创建 (216 格 × 常驻子对象太贵); 最佳用时按难度存 NVS。
#include "app_games.h"
#include "gmu.h"
#include "gm_audio.h"
#include <cstdio>
#include <cstring>
#include <vector>

// 数字 1-8 配色 (暗底亮字, 经典扫雷色系)
static const uint32_t kNumColors[9] = {
    0x000000, 0x7EB3FF, 0x7BD88F, 0xFF8A80, 0xB39DDB,
    0xFFD54F, 0x4DD0E1, 0xE0E0E0, 0x9E9E9E,
};

static inline int _min_idx(int r, int c, int cols) { return r * cols + c; }

// 邻居 (8 向)
static void _min_nb(int i, int cols, int rows, std::vector<int>& out)
{
    out.clear();
    const int r = i / cols, c = i % cols;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (!dr && !dc) continue;
            const int rr = r + dr, cc = c + dc;
            if (rr >= 0 && rr < rows && cc >= 0 && cc < cols) out.push_back(rr * cols + cc);
        }
}

// ── 页面骨架 ──
void AppGames::_buildMinesPage()
{
    lv_obj_t* pg = _pages[2];
    if (!pg) return;

    for (int i = 0; i < 3; i++) {
        _min_chips[i] = gmu::chip_btn(pg, _MIN_LV[i].nm, 24 + i * 130, 6, 120, 44,
                                      C_ACC(), _minChipCb, this);
        lv_obj_set_user_data(_min_chips[i], (void*)(intptr_t)i);
        if (i == _min_lvl) lv_obj_add_state(_min_chips[i], LV_STATE_CHECKED);
    }
    gmu::action_btn(pg, "重开", SCREEN_W - 24 - 120, 6, 120, 44, _minNewCb, this);

    _min_left_lbl = gmu::soft_label(pg, "", 430, 16, C_TEXT());
    _min_time_lbl = gmu::soft_label(pg, "0:00", 430, 44, C_DIM());
    _min_best_lbl = gmu::soft_label(pg, "", 640, 16, C_DIM());

    _minNew();
}

void AppGames::_minChipCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    int lvl = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (!self || !self->_scr || lvl == self->_min_lvl) return;
    self->_min_lvl = lvl;
    for (int i = 0; i < 3; i++) {
        if (i == lvl) lv_obj_add_state(self->_min_chips[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(self->_min_chips[i], LV_STATE_CHECKED);
    }
    self->_minNew();
}

void AppGames::_minNewCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr) return;
    // 对局进行中先确认 (防误触弃局)
    if (self->_min_started && !self->_min_over) {
        self->_showModal("重开?", "当前对局进行中, 重开将弃局", "重开", "继续玩",
                         [self]() { self->_minNew(); });
    } else {
        self->_minNew();
    }
}

// ── 建局 (雷区延迟到首翻: 首点 3×3 安全) ──
void AppGames::_minNew()
{
    const MinLevel& lv = _MIN_LV[_min_lvl];
    _min_cols = lv.cols;
    _min_rows = lv.rows;
    _min_total = lv.mines;
    for (int i = 0; i < 216; i++) {
        _min_mine[i] = _min_rev[i] = _min_flag[i] = false;
        _min_num[i] = 0;
    }
    _min_started = _min_over = false;
    _min_opened = 0;
    _min_flags = 0;
    _min_sec = 0;
    _min_lp_ms = 0;

    if (_min_left_lbl) lv_label_set_text(_min_left_lbl, "雷 0");
    if (_min_time_lbl) lv_label_set_text(_min_time_lbl, "0:00");
    if (_min_best_lbl) {
        char key[24];
        snprintf(key, sizeof(key), "gm_mines_best_%d", _min_lvl);
        const int best = _nvsInt(key);
        char buf[48];
        if (best > 0) snprintf(buf, sizeof(buf), "最佳 %d:%02d", best / 60, best % 60);
        else snprintf(buf, sizeof(buf), "最佳 -");
        lv_label_set_text(_min_best_lbl, buf);
    }

    // 整盘重建
    const int gap = 2;
    int cell = std::min((SCREEN_W - 48 - (_min_cols - 1) * gap) / _min_cols,
                        (SCREEN_H - TAB_H - 76 - 16 - (_min_rows - 1) * gap) / _min_rows);
    if (cell > 64) cell = 64;
    const int bw = _min_cols * cell + (_min_cols - 1) * gap;
    const int bh = _min_rows * cell + (_min_rows - 1) * gap;
    const int x0 = (SCREEN_W - bw) / 2;
    const int y0 = 76 + ((SCREEN_H - TAB_H - 76 - 16) - bh) / 2;

    for (int i = 0; i < 216; i++) {
        if (_min_cells[i]) { lv_obj_delete(_min_cells[i]); _min_cells[i] = nullptr; }
    }
    for (int r = 0; r < _min_rows; r++) {
        for (int c = 0; c < _min_cols; c++) {
            const int i = _min_idx(r, c, _min_cols);
            lv_obj_t* cell_obj = lv_obj_create(_pages[2]);
            if (!cell_obj) return;
            lv_obj_set_size(cell_obj, cell, cell);
            lv_obj_set_pos(cell_obj, x0 + c * (cell + gap), y0 + r * (cell + gap));
            lv_obj_set_style_radius(cell_obj, 6, 0);
            lv_obj_set_style_border_width(cell_obj, 0, 0);
            lv_obj_set_style_pad_all(cell_obj, 0, 0);
            lv_obj_set_style_shadow_width(cell_obj, 0, 0);
            lv_obj_clear_flag(cell_obj, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell_obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(cell_obj, (void*)(intptr_t)i);
            lv_obj_add_event_cb(cell_obj, _minCellCb, LV_EVENT_CLICKED, this);
            lv_obj_add_event_cb(cell_obj, _minCellCb, LV_EVENT_LONG_PRESSED, this);
            _min_cells[i] = cell_obj;
        }
    }
}

// ── 布雷 (首点 3×3 必无雷) + 算数字 ──
void AppGames::_minPlaceMines(int safe)
{
    std::vector<int> banned;
    banned.push_back(safe);
    std::vector<int> nb;
    _min_nb(safe, _min_cols, _min_rows, nb);
    for (int j : nb) banned.push_back(j);

    int placed = 0;
    while (placed < _min_total) {
        const int i = gmu::rng_range(_min_cols * _min_rows);
        if (_min_mine[i]) continue;
        bool hit = false;
        for (int b : banned)
            if (b == i) { hit = true; break; }
        if (hit) continue;
        _min_mine[i] = true;
        placed++;
    }
    for (int i = 0; i < _min_cols * _min_rows; i++) {
        if (_min_mine[i]) continue;
        _min_nb(i, _min_cols, _min_rows, nb);
        int n = 0;
        for (int j : nb)
            if (_min_mine[j]) n++;
        _min_num[i] = n;
    }
}

// ── 渲染 (子对象按需创建: 数字 label / 旗 rect×2 / 雷 circle) ──
void AppGames::_minRender(int i)
{
    lv_obj_t* cell = _min_cells[i];
    if (!cell) return;
    // 清全部子对象, 按状态重建
    while (lv_obj_get_child_cnt(cell) > 0) lv_obj_delete(lv_obj_get_child(cell, 0));

    if (_min_rev[i]) {
        if (_min_mine[i]) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(C_DANGER()), 0);
            // 雷: 深色圆盘
            lv_obj_t* m = lv_obj_create(cell);
            if (m) {
                const int s = lv_obj_get_width(cell) / 2;
                lv_obj_set_size(m, s, s);
                lv_obj_center(m);
                lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(m, lv_color_hex(0x1A0505), 0);
                lv_obj_set_style_border_width(m, 0, 0);
                lv_obj_set_style_pad_all(m, 0, 0);
                lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
            }
        } else {
            lv_obj_set_style_bg_color(cell, lv_color_hex(C_TRACK()), 0);
            if (_min_num[i]) {
                lv_obj_t* l = lv_label_create(cell);
                if (l) {
                    char b[16];
                    snprintf(b, sizeof(b), "%d", _min_num[i]);
                    lv_label_set_text(l, b);
                    lv_obj_set_style_text_font(l, gmu::zh20(), 0);
                    lv_obj_set_style_text_color(l, lv_color_hex(kNumColors[_min_num[i]]), 0);
                    lv_obj_center(l);
                }
            }
        }
    } else {
        lv_obj_set_style_bg_color(cell, lv_color_hex(C_CARD()), 0);
        if (_min_flag[i]) {
            // 旗: 杆 + 三角面 (rect 近似)
            const int w = lv_obj_get_width(cell), h = lv_obj_get_height(cell);
            lv_obj_t* pole = lv_obj_create(cell);
            if (pole) {
                lv_obj_set_size(pole, 2, h * 4 / 10);
                lv_obj_set_pos(pole, w / 2 - 4, h / 5);
                lv_obj_set_style_bg_color(pole, lv_color_hex(0xD0D0D0), 0);
                lv_obj_set_style_bg_opa(pole, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(pole, 1, 0);
                lv_obj_set_style_border_width(pole, 0, 0);
                lv_obj_set_style_pad_all(pole, 0, 0);
                lv_obj_clear_flag(pole, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(pole, LV_OBJ_FLAG_CLICKABLE);
            }
            lv_obj_t* cloth = lv_obj_create(cell);
            if (cloth) {
                lv_obj_set_size(cloth, w * 3 / 10, h * 2 / 10);
                lv_obj_set_pos(cloth, w / 2 - 2, h / 5);
                lv_obj_set_style_bg_color(cloth, lv_color_hex(C_DANGER()), 0);
                lv_obj_set_style_bg_opa(cloth, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(cloth, 2, 0);
                lv_obj_set_style_border_width(cloth, 0, 0);
                lv_obj_set_style_pad_all(cloth, 0, 0);
                lv_obj_clear_flag(cloth, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(cloth, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
}

void AppGames::_minRenderAll()
{
    for (int i = 0; i < _min_cols * _min_rows; i++) _minRender(i);
}

void AppGames::_minRefreshHead()
{
    if (_min_left_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "雷 %d", _min_total - _min_flags);
        lv_label_set_text(_min_left_lbl, buf);
    }
}

// ── 翻开 + BFS 展开 ──
void AppGames::_minReveal(int i)
{
    if (_min_over || _min_rev[i] || _min_flag[i]) return;
    if (!_min_started) {
        _minPlaceMines(i);
        _min_started = true;
    }
    if (_min_mine[i]) return _minLose(i);

    std::vector<int> st;
    st.push_back(i);
    std::vector<int> nb;
    while (!st.empty()) {
        const int j = st.back();
        st.pop_back();
        if (_min_rev[j] || _min_flag[j] || _min_mine[j]) continue;
        _min_rev[j] = true;
        _min_opened++;
        if (!_min_num[j]) {
            _min_nb(j, _min_cols, _min_rows, nb);
            for (int k : nb)
                if (!_min_rev[k]) st.push_back(k);
        }
    }
    gm_audio::tone(480, 0.05);
    _minRenderAll();
    if (_min_opened == _min_cols * _min_rows - _min_total) _minWin();
}

void AppGames::_minFlag(int i)
{
    if (_min_over || _min_rev[i]) return;
    _min_flag[i] = !_min_flag[i];
    _min_flags += _min_flag[i] ? 1 : -1;
    _minRefreshHead();
    _minRender(i);
    gm_audio::tone(820, 0.07);
}

// ── 输入: 短点翻开 / 长按插旗 ──
void AppGames::_minCellCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr || self->_min_over) return;
    if (self->_modalOpen()) return;
    const int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        self->_min_lp_ms = gmu::mono_ms();
        self->_minFlag(i);
        return;
    }
    // CLICKED: 长按刚插过旗的 300ms 内忽略 (双保险; LVGL 长按后通常不再派 CLICKED)
    if (self->_min_lp_ms != 0 && gmu::mono_ms() - self->_min_lp_ms < 300) return;
    self->_minReveal(i);
}

// ── 胜负 ──
void AppGames::_minLose(int hit)
{
    _min_over = true;
    gm_audio::jingle((const float[]){180.0f, 40.0f}, (const double[]){0.45}, 1);
    for (int i = 0; i < _min_cols * _min_rows; i++) {
        if (_min_mine[i]) _min_rev[i] = true;   // 败局复盘: 全部雷亮出
        _minRender(i);
    }
    char body[96];
    snprintf(body, sizeof(body), "用时 %d:%02d · %s", _min_sec / 60, _min_sec % 60,
             _MIN_LV[_min_lvl].nm);
    _showModal("踩雷了", body, "再来一局", "复盘", nullptr);
}

void AppGames::_minWin()
{
    _min_over = true;
    for (int i = 0; i < _min_cols * _min_rows; i++) {
        if (_min_mine[i] && !_min_flag[i]) {
            _min_flag[i] = true;
            _min_flags++;
        }
    }
    _minRefreshHead();
    gm_audio::jingle((const float[]){523.25f, 659.25f, 783.99f, 1046.5f},
                     (const double[]){0.16, 0.16, 0.16, 0.28}, 4);

    char key[24];
    snprintf(key, sizeof(key), "gm_mines_best_%d", _min_lvl);
    const int best = _nvsInt(key);
    const bool is_best = best <= 0 || _min_sec < best;
    if (is_best) _nvsSet(key, _min_sec);
    if (_min_best_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "最佳 %d:%02d", _nvsInt(key) / 60, _nvsInt(key) % 60);
        lv_label_set_text(_min_best_lbl, buf);
    }

    char body[96];
    if (is_best) snprintf(body, sizeof(body), "新纪录! 用时 %d:%02d · %s",
                          _min_sec / 60, _min_sec % 60, _MIN_LV[_min_lvl].nm);
    else snprintf(body, sizeof(body), "用时 %d:%02d · 最佳 %d:%02d · %s",
                  _min_sec / 60, _min_sec % 60, best / 60, best % 60,
                  _MIN_LV[_min_lvl].nm);
    _showModal("排雷成功!", body, "再来一局", "继续看看", nullptr);
    _minRenderAll();
}
