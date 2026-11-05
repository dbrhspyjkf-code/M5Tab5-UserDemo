// game_sudoku — 数独 (car-dash games/sudoku 移植, 2026-11-05)
//
// 生成: 随机回溯造全解 → 挖洞并逐步唯一解校验 (解数≤1 才留) → 三档难度
// (40/32/32→26 提示数)。生成在 worker (32KB 栈 tryRunDetached, PSRAM pthread
// 铁律: 内部 RAM 碎片化下大栈任务必败), UI 出「生成中…」等 _sud_ready。
// 交互: 点格选位 → 点数字填入 (笔记模式写 3×3 候选小数字) / 再点同数字擦除;
// 数字键盘显示剩余可放个数 (放满 9 置灰); 错格即时标红 (对照唯一解);
// 全对通关, 最佳用时按难度存 NVS。
#include "app_games.h"
#include "gmu.h"
#include "gm_audio.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ── 回溯求解/计数 (位掩码, 扫描序; 与 car-dash 同款) ──
static int sud_count_sol(uint8_t* a, int limit)
{
    uint16_t rows[9] = {}, cols[9] = {}, boxes[9] = {};
    for (int i = 0; i < 81; i++) {
        const uint8_t v = a[i];
        if (!v) continue;
        const uint16_t bit = (uint16_t)(1 << (v - 1));
        const int r = i / 9, c = i % 9, b = (r / 3) * 3 + (c / 3);
        if ((rows[r] | cols[c] | boxes[b]) & bit) return 0;   // 非法局面
        rows[r] |= bit; cols[c] |= bit; boxes[b] |= bit;
    }
    int count = 0;
    std::function<void(int)> dfs = [&](int i) {
        if (count >= limit) return;
        while (i < 81 && a[i]) i++;
        if (i == 81) { count++; return; }
        const int r = i / 9, c = i % 9, b = (r / 3) * 3 + (c / 3);
        for (uint8_t d = 1; d <= 9; d++) {
            const uint16_t bit = (uint16_t)(1 << (d - 1));
            if ((rows[r] | cols[c] | boxes[b]) & bit) continue;
            a[i] = d; rows[r] |= bit; cols[c] |= bit; boxes[b] |= bit;
            dfs(i + 1);
            a[i] = 0; rows[r] &= (uint16_t)~bit; cols[c] &= (uint16_t)~bit; boxes[b] &= (uint16_t)~bit;
            if (count >= limit) return;
        }
    };
    dfs(0);
    return count;
}

// ── 生成 (worker 内): 随机回溯全解 → 挖洞唯一解 ──
static void _sudGenFull(uint8_t* a, uint32_t (*rng)())
{
    uint16_t rows[9] = {}, cols[9] = {}, boxes[9] = {};
    std::function<bool(int)> dfs = [&](int i) -> bool {
        if (i == 81) return true;
        const int r = i / 9, c = i % 9, b = (r / 3) * 3 + (c / 3);
        uint8_t ds[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        // Fisher-Yates 洗牌
        for (int k = 8; k > 0; k--) {
            const int j = (int)(rng() % (uint32_t)(k + 1));
            const uint8_t t = ds[k]; ds[k] = ds[j]; ds[j] = t;
        }
        for (int k = 0; k < 9; k++) {
            const uint8_t d = ds[k];
            const uint16_t bit = (uint16_t)(1 << (d - 1));
            if ((rows[r] | cols[c] | boxes[b]) & bit) continue;
            a[i] = d; rows[r] |= bit; cols[c] |= bit; boxes[b] |= bit;
            if (dfs(i + 1)) return true;
            a[i] = 0; rows[r] &= (uint16_t)~bit; cols[c] &= (uint16_t)~bit; boxes[b] &= (uint16_t)~bit;
        }
        return false;
    };
    dfs(0);
}

static void _sudMakePuzzle(uint8_t* puz, uint8_t* sol, int givens, uint32_t (*rng)())
{
    _sudGenFull(sol, rng);
    memcpy(puz, sol, 81);
    int order[81];
    for (int i = 0; i < 81; i++) order[i] = i;
    for (int k = 80; k > 0; k--) {
        const int j = (int)(rng() % (uint32_t)(k + 1));
        const int t = order[k]; order[k] = order[j]; order[j] = t;
    }
    int left = 81;
    uint8_t trial[81];
    for (int k = 0; k < 81 && left > givens; k++) {
        const int i = order[k];
        const uint8_t v = puz[i];
        puz[i] = 0;
        memcpy(trial, puz, 81);
        if (sud_count_sol(trial, 2) != 1) puz[i] = v;   // 多解则放回
        else left--;
    }
}

// ── 页面骨架 ──
void AppGames::_buildSudokuPage()
{
    lv_obj_t* pg = _pages[3];
    if (!pg) return;

    // 布局 (2026-11-06 用户要求: 格子加大 + 难度/重开挪到数字键盘上面):
    // 棋盘 70px cell 占满左侧全高 (630², y 13..643); 右栏 = 难度/重开 → 数字键盘
    // → 笔记/擦除/用时 → 最佳
    for (int i = 0; i < 3; i++) {
        _sud_chips[i] = gmu::chip_btn(pg, _SUD_LV[i].nm, 648 + i * 124, 8, 120, 44,
                                      C_ACC(), _sudChipCb, this);
        lv_obj_set_user_data(_sud_chips[i], (void*)(intptr_t)i);
        if (i == _sud_lvl) lv_obj_add_state(_sud_chips[i], LV_STATE_CHECKED);
    }
    gmu::action_btn(pg, "重开", 1136, 8, 120, 44, _sudNewCb, this);

    // 棋盘 9×9 (cell 70, 630²), 左侧满高
    constexpr int CELL = 70;
    const int bd = CELL * 9;
    const int x0 = (648 - bd) / 2;
    const int y0 = (SCREEN_H - TAB_H - bd) / 2;

    for (int i = 0; i < 81; i++) {
        const int r = i / 9, c = i % 9;
        lv_obj_t* cell = lv_obj_create(pg);
        if (!cell) return;
        lv_obj_set_size(cell, CELL - 2, CELL - 2);
        lv_obj_set_pos(cell, x0 + c * CELL + 1, y0 + r * CELL + 1);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(cell, (void*)(intptr_t)i);
        lv_obj_add_event_cb(cell, _sudCellCb, LV_EVENT_CLICKED, this);
        lv_obj_t* l = lv_label_create(cell);
        if (l) {
            lv_label_set_text(l, "");
            lv_obj_set_style_text_font(l, gmu::zh30(), 0);
            lv_obj_center(l);
        }
        _sud_cells[i] = cell;
        _sud_lbls[i] = l;
    }
    // 3×3 宫分隔线 (4 条, 盖在格间隙上)
    for (int k = 1; k <= 2; k++) {
        lv_obj_t* v = lv_obj_create(pg);
        if (v) {
            lv_obj_set_size(v, 3, bd);
            lv_obj_set_pos(v, x0 + k * 3 * CELL - 1, y0);
            lv_obj_set_style_bg_color(v, lv_color_hex(0x5A5E64), 0);
            lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(v, 0, 0);
            lv_obj_set_style_pad_all(v, 0, 0);
            lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_t* hsep = lv_obj_create(pg);
        if (hsep) {
            lv_obj_set_size(hsep, bd, 3);
            lv_obj_set_pos(hsep, x0, y0 + k * 3 * CELL - 1);
            lv_obj_set_style_bg_color(hsep, lv_color_hex(0x5A5E64), 0);
            lv_obj_set_style_bg_opa(hsep, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(hsep, 0, 0);
            lv_obj_set_style_pad_all(hsep, 0, 0);
            lv_obj_clear_flag(hsep, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_clear_flag(hsep, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    // 生成中提示 (棋盘中央)
    _sud_status = gmu::soft_label(pg, "", x0 + bd / 2 - 90, y0 + bd / 2 - 20, C_DIM(), gmu::zh30());

    // 右侧数字键盘 3×3 (剩余计数)
    const int nx = 648, ny = 64, nw = 190, nh = 110, ngap = 12;
    for (int i = 0; i < 9; i++) {
        lv_obj_t* btn = lv_button_create(pg);
        if (!btn) return;
        lv_obj_set_size(btn, nw, nh);
        lv_obj_set_pos(btn, nx + (i % 3) * (nw + ngap), ny + (i / 3) * (nh + ngap));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2C30), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1C1E), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_TEXT()), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0x5A5E64), LV_STATE_DISABLED);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(intptr_t)(i + 1));
        lv_obj_add_event_cb(btn, _sudNumCb, LV_EVENT_CLICKED, this);
        lv_obj_t* dl = lv_label_create(btn);
        if (dl) {
            char b[16];
            snprintf(b, sizeof(b), "%d", i + 1);
            lv_label_set_text(dl, b);
            lv_obj_set_style_text_font(dl, gmu::zh30(), 0);
            lv_obj_align(dl, LV_ALIGN_TOP_MID, 0, 16);
        }
        lv_obj_t* cl = lv_label_create(btn);
        if (cl) {
            lv_label_set_text(cl, "9");
            lv_obj_set_style_text_font(cl, gmu::mono20(), 0);
            lv_obj_set_style_text_color(cl, lv_color_hex(C_DIM()), 0);
            lv_obj_align(cl, LV_ALIGN_BOTTOM_MID, 0, -12);
        }
        _sud_num_btns[i] = btn;
        _sud_num_cnts[i] = cl;
    }

    // 键盘下方: 笔记 / 擦除 + 用时/最佳
    _sud_note_btn = gmu::chip_btn(pg, "笔记", nx, ny + 3 * nh + 2 * ngap + 16, 190, 52,
                                  C_ACC(), _sudNoteCb, this);
    gmu::action_btn(pg, "擦除", nx + 202, ny + 3 * nh + 2 * ngap + 16, 190, 52,
                    _sudEraseCb, this);
    _sud_time_lbl = gmu::soft_label(pg, "0:00", nx + 414, ny + 3 * nh + 2 * ngap + 20,
                                    C_TEXT());
    _sud_best_lbl = gmu::soft_label(pg, "最佳 -", nx, ny + 3 * nh + 2 * ngap + 88,
                                    C_DIM());

    _sudNew();
}

void AppGames::_sudChipCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    int lvl = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (!self || !self->_scr || lvl == self->_sud_lvl) return;
    self->_sud_lvl = lvl;
    for (int i = 0; i < 3; i++) {
        if (i == lvl) lv_obj_add_state(self->_sud_chips[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(self->_sud_chips[i], LV_STATE_CHECKED);
    }
    self->_sudNew();
}

void AppGames::_sudNewCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr) return;
    if (self->_sud_started && !self->_sud_over) {
        self->_showModal("重开?", "当前对局进行中, 重开将弃局", "重开", "继续玩",
                         [self]() { self->_sudNew(); });
    } else {
        self->_sudNew();
    }
}

// ── 发 worker 生成 ──
void AppGames::_sudNew()
{
    if (_sud_gen_in_flight.exchange(true)) return;
    _sud_over = _sud_started = false;
    _sud_sel = -1;
    _sud_sec = 0;
    for (int i = 0; i < 81; i++) {
        _sud_cur[i] = _sud_sol[i] = _sud_given[i] = 0;
        _sud_notes[i] = 0;
    }
    if (_sud_time_lbl) lv_label_set_text(_sud_time_lbl, "0:00");
    {
        char key[24];
        snprintf(key, sizeof(key), "gm_sud_best_%d", _sud_lvl);
        const int best = _nvsInt(key);
        if (_sud_best_lbl) {
            char buf[48];
            if (best > 0) snprintf(buf, sizeof(buf), "最佳 %d:%02d", best / 60, best % 60);
            else snprintf(buf, sizeof(buf), "最佳 -");
            lv_label_set_text(_sud_best_lbl, buf);
        }
    }
    _sudRenderAll();
    if (_sud_status) {
        lv_obj_set_style_text_color(_sud_status, lv_color_hex(C_DIM()), 0);
        lv_label_set_text(_sud_status, "生成中…");
    }

    const int givens = _SUD_LV[_sud_lvl].givens;
    auto alive = _alive;
    bool ok = GetHAL()->tryRunDetached([this, alive, givens]() {
        uint8_t puz[81], sol[81];
        _sudMakePuzzle(puz, sol, givens, &gmu::rng_u32);
        if (!alive || !*alive) {
            _sud_gen_in_flight.store(false);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(_sud_mu);
            _sud_puz_buf.assign(puz, puz + 81);
            _sud_sol_buf.assign(sol, sol + 81);
        }
        _sud_gen_in_flight.store(false);
        _sud_ready.store(true);
    }, 32 * 1024);
    if (!ok) {
        _sud_gen_in_flight.store(false);
        if (_sud_status) lv_label_set_text(_sud_status, "生成失败 (线程忙)");
    }
}

// ── worker 结果上屏 (onRunning 消费; LVGL 全在 UI 线程) ──
void AppGames::_sudApply()
{
    if (!_scr) return;
    {
        std::lock_guard<std::mutex> lk(_sud_mu);
        if (_sud_puz_buf.size() == 81 && _sud_sol_buf.size() == 81) {
            for (int i = 0; i < 81; i++) {
                _sud_sol[i] = _sud_sol_buf[i];
                _sud_cur[i] = _sud_puz_buf[i];
                _sud_given[i] = _sud_puz_buf[i] > 0 ? 1 : 0;
            }
        }
    }
    if (_sud_status) lv_label_set_text(_sud_status, "");
    _sudRenderAll();
}

// ── 渲染 ──
void AppGames::_sudRender(int i)
{
    lv_obj_t* cell = _sud_cells[i];
    lv_obj_t* l = _sud_lbls[i];
    if (!cell || !l) return;
    if (_sud_cur[i]) {
        lv_obj_set_style_text_font(l, gmu::zh30(), 0);
        char b[16];
        snprintf(b, sizeof(b), "%d", _sud_cur[i]);
        lv_label_set_text(l, b);
    } else if (_sud_notes[i]) {
        // 3×3 候选小数字 (3 行, 缺位空格)
        lv_obj_set_style_text_font(l, gmu::mono14(), 0);
        char b[24];
        int p = 0;
        for (int row = 0; row < 3; row++) {
            for (int k = 0; k < 3; k++) {
                const int d = row * 3 + k + 1;
                b[p++] = (_sud_notes[i] & (1 << (d - 1))) ? (char)('0' + d) : ' ';
            }
            b[p++] = '\n';
        }
        b[p] = 0;
        lv_label_set_text(l, b);
    } else {
        lv_label_set_text(l, "");
    }
    _sudHighlight();
}

void AppGames::_sudRenderAll()
{
    for (int i = 0; i < 81; i++) {
        // 直接渲染 (不含 highlight, 统一在末尾刷)
        lv_obj_t* cell = _sud_cells[i];
        lv_obj_t* l = _sud_lbls[i];
        if (!cell || !l) continue;
        if (_sud_cur[i]) {
            lv_obj_set_style_text_font(l, gmu::zh30(), 0);
            char b[16];
            snprintf(b, sizeof(b), "%d", _sud_cur[i]);
            lv_label_set_text(l, b);
        } else if (_sud_notes[i]) {
            lv_obj_set_style_text_font(l, gmu::mono14(), 0);
            char b[24];
            int p = 0;
            for (int row = 0; row < 3; row++) {
                for (int k = 0; k < 3; k++) {
                    const int d = row * 3 + k + 1;
                    b[p++] = (_sud_notes[i] & (1 << (d - 1))) ? (char)('0' + d) : ' ';
                }
                b[p++] = '\n';
            }
            b[p] = 0;
            lv_label_set_text(l, b);
        } else {
            lv_label_set_text(l, "");
        }
    }
    _sudHighlight();
    _sudRenderNums();
    _sudRefreshStatus();
}

void AppGames::_sudHighlight()
{
    const int sr = _sud_sel >= 0 ? _sud_sel / 9 : -1;
    const int sc = _sud_sel >= 0 ? _sud_sel % 9 : -1;
    const uint8_t sv = _sud_sel >= 0 ? _sud_cur[_sud_sel] : 0;
    for (int i = 0; i < 81; i++) {
        lv_obj_t* cell = _sud_cells[i];
        lv_obj_t* l = _sud_lbls[i];
        if (!cell || !l) continue;
        const int r = i / 9, c = i % 9;
        const bool same = sv != 0 && _sud_cur[i] == sv && i != _sud_sel;
        const bool wrong = _sud_cur[i] != 0 && _sud_cur[i] != _sud_sol[i];
        // 底色: 选中=强调色 / 提示数=卡色 / 填过=轨道色 / 空=深卡
        if (i == _sud_sel) lv_obj_set_style_bg_color(cell, lv_color_hex(C_ACC()), 0);
        else if (_sud_given[i]) lv_obj_set_style_bg_color(cell, lv_color_hex(C_CARD()), 0);
        else if (_sud_cur[i]) lv_obj_set_style_bg_color(cell, lv_color_hex(C_TRACK()), 0);
        else lv_obj_set_style_bg_color(cell, lv_color_hex(C_CARD0()), 0);
        // 字色: 选中=黑 / 错=红 / 同数=琥珀 / 提示=白 / 手填=强调
        if (i == _sud_sel) lv_obj_set_style_text_color(l, lv_color_hex(0x000000), 0);
        else if (wrong) lv_obj_set_style_text_color(l, lv_color_hex(C_DANGER()), 0);
        else if (same) lv_obj_set_style_text_color(l, lv_color_hex(C_WARN()), 0);
        else if (_sud_given[i]) lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT()), 0);
        else lv_obj_set_style_text_color(l, lv_color_hex(C_ACC()), 0);
        (void)sr; (void)sc;
    }
}

// 数字键盘: 显示剩余可放个数, 放满 9 置灰
void AppGames::_sudRenderNums()
{
    for (int d = 1; d <= 9; d++) {
        int used = 0;
        for (int i = 0; i < 81; i++)
            if (_sud_cur[i] == d) used++;
        lv_obj_t* btn = _sud_num_btns[d - 1];
        lv_obj_t* cl = _sud_num_cnts[d - 1];
        if (cl) {
            char b[16];
            snprintf(b, sizeof(b), "%d", 9 - used);
            lv_label_set_text(cl, b);
        }
        if (btn) {
            if (used >= 9) lv_obj_add_state(btn, LV_STATE_DISABLED);
            else lv_obj_remove_state(btn, LV_STATE_DISABLED);
        }
    }
}

void AppGames::_sudRefreshStatus()
{
    if (!_sud_best_lbl) return;
    char key[24];
    snprintf(key, sizeof(key), "gm_sud_best_%d", _sud_lvl);
    const int best = _nvsInt(key);
    char buf[48];
    if (best > 0) snprintf(buf, sizeof(buf), "最佳 %d:%02d", best / 60, best % 60);
    else snprintf(buf, sizeof(buf), "最佳 -");
    lv_label_set_text(_sud_best_lbl, buf);
}

// ── 交互 ──
void AppGames::_sudCellCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr || self->_sud_over) return;
    if (self->_modalOpen()) return;
    const int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    self->_sud_sel = i;
    gm_audio::tone(650, 0.04);
    self->_sudRender(i);   // 内含 highlight
}

void AppGames::_sudNumCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr || self->_sud_over) return;
    if (self->_modalOpen()) return;
    const int d = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    self->_sudFill(d);
}

void AppGames::_sudNoteCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr) return;
    self->_sud_note = !self->_sud_note;
    if (self->_sud_note_btn) {
        if (self->_sud_note) lv_obj_add_state(self->_sud_note_btn, LV_STATE_CHECKED);
        else lv_obj_remove_state(self->_sud_note_btn, LV_STATE_CHECKED);
        // chip_btn 的 label 是 0 号子对象
        lv_obj_t* l = lv_obj_get_child(self->_sud_note_btn, 0);
        if (l) lv_label_set_text(l, self->_sud_note ? "笔记中" : "笔记");
    }
}

void AppGames::_sudEraseCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr || self->_sud_over) return;
    if (self->_modalOpen()) return;
    const int i = self->_sud_sel;
    if (i < 0 || self->_sud_given[i]) return;
    self->_sud_cur[i] = 0;
    self->_sud_notes[i] = 0;
    gm_audio::tone(650, 0.04);
    self->_sudRender(i);
    self->_sudRenderNums();
}

void AppGames::_sudFill(int d)
{
    const int i = _sud_sel;
    if (i < 0 || _sud_given[i]) return;
    if (_sud_note) {
        if (_sud_cur[i]) return;   // 有值不写笔记
        _sud_notes[i] ^= (uint16_t)(1 << (d - 1));
        gm_audio::tone(650, 0.04);
    } else {
        const bool same = _sud_cur[i] == d;
        _sud_cur[i] = same ? 0 : d;          // 再点同数字=擦除
        _sud_notes[i] = 0;
        _sud_started = true;
        gm_audio::tone(same ? 650 : (_sud_cur[i] == _sud_sol[i] ? 560 : 230),
                       _sud_cur[i] == _sud_sol[i] && !same ? 0.07 : 0.1);
    }
    _sudRender(i);
    _sudRenderNums();
    _sudRefreshStatus();

    // 通关: 全对
    bool all = true;
    for (int k = 0; k < 81; k++)
        if (_sud_cur[k] != _sud_sol[k]) { all = false; break; }
    if (all) _sudWin();
}

// ── 通关 ──
void AppGames::_sudWin()
{
    _sud_over = true;
    gm_audio::jingle((const float[]){523.25f, 659.25f, 783.99f, 1046.5f},
                     (const double[]){0.16, 0.16, 0.16, 0.28}, 4);
    char key[24];
    snprintf(key, sizeof(key), "gm_sud_best_%d", _sud_lvl);
    const int best = _nvsInt(key);
    const bool is_best = best <= 0 || _sud_sec < best;
    if (is_best) _nvsSet(key, _sud_sec);
    _sudRefreshStatus();

    char body[96];
    if (is_best) snprintf(body, sizeof(body), "新纪录! 用时 %d:%02d · %s",
                          _sud_sec / 60, _sud_sec % 60, _SUD_LV[_sud_lvl].nm);
    else snprintf(body, sizeof(body), "用时 %d:%02d · 最佳 %d:%02d · %s",
                  _sud_sec / 60, _sud_sec % 60, best / 60, best % 60,
                  _SUD_LV[_sud_lvl].nm);
    _showModal("完成!", body, "再来一局", "继续看看", nullptr);
}
