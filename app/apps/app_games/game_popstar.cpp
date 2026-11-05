// game_popstar — 同色消除「星星」(car-dash games/popstar 移植, 2026-11-05)
//
// 规则 (消灭星星经典): 14×6 五色 (car-dash 横屏变体), 点同色正交相邻组(≥2)
// 首点选中、再点消除, 得分 = n²×5; 消后上块下落、空列右侧左移; 全清 +2000;
// 无可消组即结束。触屏防误触: 首点选组(白描边+显示预得分), 再点同组才消除。
// 消除反馈 = 淡出动画 (voice_input 同款: 拆屏前 lv_anim_delete 防悬垂) +
// 重力位移动画; 最佳分存 NVS gm_pop_best。
#include "app_games.h"
#include "gmu.h"
#include "gm_audio.h"
#include <cstdio>
#include <cstring>
#include <vector>

const uint32_t AppGames::_POP_COLORS[5] = {
    0xEF4444,   // 红
    0xF59E0B,   // 琥珀
    0x22C55E,   // 绿
    0x3B82F6,   // 蓝
    0xA855F7,   // 紫
};

// ── 动画回调 (静态, var=方块对象) ──
static void _pop_opa_cb(void* var, int32_t v)
{
    lv_obj_t* o = (lv_obj_t*)var;
    if (o) lv_obj_set_style_opa(o, (lv_opa_t)v, 0);
}
static void _pop_move_x_cb(void* var, int32_t v)
{
    lv_obj_t* o = (lv_obj_t*)var;
    if (o) lv_obj_set_x(o, (int32_t)v);
}
static void _pop_move_y_cb(void* var, int32_t v)
{
    lv_obj_t* o = (lv_obj_t*)var;
    if (o) lv_obj_set_y(o, (int32_t)v);
}

static inline int _pop_idx(int r, int c) { return r * AppGames::POP_COLS + c; }

// ── 页面骨架 ──
void AppGames::_buildPopPage()
{
    lv_obj_t* pg = _pages[1];
    if (!pg) return;

    gmu::action_btn(pg, "重开", SCREEN_W - 24 - 120, 6, 120, 44, _popNewCb, this);
    _pop_score_lbl = gmu::soft_label(pg, "", 24, 16, C_TEXT());
    _pop_pick_lbl = gmu::soft_label(pg, "-", 24, 44, C_ACC());

    _pop_best = _nvsInt("gm_pop_best");
    _popNew();
}

void AppGames::_popNewCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr) return;
    // 有分且盘面还有块 → 先确认 (防误触清分, car-dash 同款)
    if (self->_pop_score > 0 && self->_popAny()) {
        char body[64];
        snprintf(body, sizeof(body), "当前分数 %d, 重开将清零", self->_pop_score);
        self->_showModal("重开?", body, "重开", "继续玩",
                         [self]() { self->_popNew(); });
    } else {
        self->_popNew();
    }
}

// ── 建局 ──
void AppGames::_popNew()
{
    _popKillAnims();
    if (_pop_t1) { lv_timer_del(_pop_t1); _pop_t1 = nullptr; }
    if (_pop_t2) { lv_timer_del(_pop_t2); _pop_t2 = nullptr; }
    // 残留的淡出中对象 (正常流程 t1 已删, modal 重开兜底)
    for (lv_obj_t* b : _pop_dying) {
        if (b) lv_obj_delete(b);
    }
    _pop_dying.clear();
    _pop_sel.clear();
    _pop_lock = false;
    _pop_score = 0;
    _pop_best = _nvsInt("gm_pop_best");

    for (int i = 0; i < POP_ROWS * POP_COLS; i++) {
        if (_pop_blk[i]) { lv_obj_delete(_pop_blk[i]); _pop_blk[i] = nullptr; }
    }

    // 棋盘: 14×6, cell 取短边, 居中 (页内 y 64..648 可用区 584 高)
    const int gap = 4;
    int cell = std::min((SCREEN_W - 48 - (POP_COLS - 1) * gap) / POP_COLS,
                        (SCREEN_H - TAB_H - 64 - 16 - (POP_ROWS - 1) * gap) / POP_ROWS);
    if (cell > 84) cell = 84;
    const int bw = POP_COLS * cell + (POP_COLS - 1) * gap;
    const int bh = POP_ROWS * cell + (POP_ROWS - 1) * gap;
    const int x0 = (SCREEN_W - bw) / 2;
    const int y0 = 64 + ((SCREEN_H - TAB_H - 64 - 16) - bh) / 2;

    for (int r = 0; r < POP_ROWS; r++) {
        for (int c = 0; c < POP_COLS; c++) {
            const int i = _pop_idx(r, c);
            _pop_col[i] = gmu::rng_range(5);
            lv_obj_t* blk = lv_obj_create(_pages[1]);
            if (!blk) return;
            lv_obj_set_size(blk, cell, cell);
            lv_obj_set_pos(blk, x0 + c * (cell + gap), y0 + r * (cell + gap));
            lv_obj_set_style_radius(blk, 14, 0);
            lv_obj_set_style_border_width(blk, 0, 0);
            lv_obj_set_style_pad_all(blk, 0, 0);
            lv_obj_set_style_shadow_width(blk, 0, 0);
            lv_obj_set_style_bg_color(blk, lv_color_hex(_POP_COLORS[_pop_col[i]]), 0);
            lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
            lv_obj_clear_flag(blk, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(blk, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(blk, (void*)(intptr_t)i);
            lv_obj_add_event_cb(blk, _popBlkCb, LV_EVENT_CLICKED, this);
            _pop_blk[i] = blk;
        }
    }
    _popRefreshScore();
    if (_pop_pick_lbl) lv_label_set_text(_pop_pick_lbl, "-");
}

void AppGames::_popRefreshScore()
{
    if (!_pop_score_lbl) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "分数 %d · 最佳 %d", _pop_score, _pop_score > _pop_best ? _pop_score : _pop_best);
    lv_label_set_text(_pop_score_lbl, buf);
}

// ── 组查找: 同色正交相邻 BFS ──
int AppGames::_popGroup(int r0, int c0, std::vector<int>& out)
{
    out.clear();
    const int color = _pop_col[_pop_idx(r0, c0)];
    bool seen[POP_ROWS * POP_COLS] = {};
    std::vector<int> st;
    const int start = _pop_idx(r0, c0);
    seen[start] = true;
    out.push_back(start);
    st.push_back(start);
    while (!st.empty()) {
        const int cur = st.back();
        st.pop_back();
        const int r = cur / POP_COLS, c = cur % POP_COLS;
        const int nb[4] = {
            r > 0 ? _pop_idx(r - 1, c) : -1,
            r < POP_ROWS - 1 ? _pop_idx(r + 1, c) : -1,
            c > 0 ? _pop_idx(r, c - 1) : -1,
            c < POP_COLS - 1 ? _pop_idx(r, c + 1) : -1,
        };
        for (int k = 0; k < 4; k++) {
            const int j = nb[k];
            if (j < 0 || seen[j] || !_pop_blk[j]) continue;
            if (_pop_col[j] != color) continue;
            seen[j] = true;
            out.push_back(j);
            st.push_back(j);
        }
    }
    return (int)out.size();
}

void AppGames::_popBlkCb(lv_event_t* e)
{
    auto* self = static_cast<AppGames*>(lv_event_get_user_data(e));
    if (!self || !self->_scr) return;
    const int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    self->_popTap(i / POP_COLS, i % POP_COLS);
}

// ── 点块: 选 / 消 (car-dash 两段式防胖指误消) ──
void AppGames::_popTap(int r, int c)
{
    if (_pop_lock || _modalOpen()) return;
    const int i = _pop_idx(r, c);
    if (!_pop_blk[i]) { _popDeselect(); return; }

    std::vector<int> g;
    if (_popGroup(r, c, g) < 2) { _popDeselect(); return; }

    // 与当前选中是同一连通组 → 消除; 否则改为选中
    if (!_pop_sel.empty() && _pop_blk[_pop_sel[0]] &&
        std::find(g.begin(), g.end(), _pop_sel[0]) != g.end()) {
        _popEliminate();
    } else {
        _popSelect(g);
    }
}

void AppGames::_popSelect(const std::vector<int>& g)
{
    _popDeselect();
    _pop_sel = g;
    for (int j : g) {
        lv_obj_t* b = _pop_blk[j];
        if (b) {
            lv_obj_set_style_outline_width(b, 6, 0);
            lv_obj_set_style_outline_color(b, lv_color_hex(0xFFFFFF), 0);
        }
    }
    if (_pop_pick_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "+%d", (int)g.size() * (int)g.size() * 5);
        lv_label_set_text(_pop_pick_lbl, buf);
    }
    gm_audio::tone(880, 0.05);
}

void AppGames::_popDeselect()
{
    for (int j : _pop_sel) {
        lv_obj_t* b = _pop_blk[j];
        if (b) lv_obj_set_style_outline_width(b, 0, 0);
    }
    _pop_sel.clear();
    if (_pop_pick_lbl) lv_label_set_text(_pop_pick_lbl, "-");
}

// ── 消除: 得分 → 淡出 → (t1) 删对象+清格+重力 → (t2) 解锁+判结束 ──
void AppGames::_popEliminate()
{
    _pop_lock = true;
    const int n = (int)_pop_sel.size();
    _pop_score += n * n * 5;
    gm_audio::tone(260 + std::min(n, 18) * 34, 0.17);

    for (int j : _pop_sel) {
        lv_obj_t* b = _pop_blk[j];
        if (!b) continue;
        lv_obj_set_style_outline_width(b, 0, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, b);
        lv_anim_set_exec_cb(&a, _pop_opa_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, 0);
        lv_anim_set_time(&a, 160);
        lv_anim_start(&a);
        // 立即摘出 grid (引用清空, 防重力/判邻阶段读到淡出中的块);
        // 对象本身进 _pop_dying, 由 t1 统一删 (UI 逻辑持有生命周期)
        _pop_blk[j] = nullptr;
        _pop_dying.push_back(b);
    }
    _pop_sel.clear();
    if (_pop_pick_lbl) lv_label_set_text(_pop_pick_lbl, "-");

    if (_pop_t1) lv_timer_del(_pop_t1);
    _pop_t1 = lv_timer_create(_popStageCb1, 200, this);
    lv_timer_set_repeat_count(_pop_t1, 1);
}

void AppGames::_popStageCb1(lv_timer_t* t)
{
    auto* self = static_cast<AppGames*>(lv_timer_get_user_data(t));
    self->_pop_t1 = nullptr;
    lv_timer_del(t);
    if (!self || !self->_scr) return;
    // 淡出完毕: 统一删对象 (拆屏场景由 _popKillAnims+screen delete 兜底)
    for (lv_obj_t* b : self->_pop_dying) {
        if (b) lv_obj_delete(b);
    }
    self->_pop_dying.clear();
    self->_popGravity();
    self->_popRefreshScore();

    if (self->_pop_t2) lv_timer_del(self->_pop_t2);
    self->_pop_t2 = lv_timer_create(_popStageCb2, 220, self);
    lv_timer_set_repeat_count(self->_pop_t2, 1);
}

void AppGames::_popStageCb2(lv_timer_t* t)
{
    auto* self = static_cast<AppGames*>(lv_timer_get_user_data(t));
    self->_pop_t2 = nullptr;
    lv_timer_del(t);
    if (!self || !self->_scr) return;
    self->_pop_lock = false;
    self->_popEnd(false);   // 判结束 (全清/无可消); 未结束则为空操作
}

// ── 下落 + 空列左移: 重建 grid, 存活块动画挪位 ──
void AppGames::_popGravity()
{
    // 每列自下而上收集存活块 (底对齐)
    int new_idx[POP_ROWS * POP_COLS];
    for (int i = 0; i < POP_ROWS * POP_COLS; i++) new_idx[i] = -1;

    int ncols = 0;
    for (int c = 0; c < POP_COLS; c++) {
        int col[POP_ROWS];
        int n = 0;
        for (int r = POP_ROWS - 1; r >= 0; r--) {
            const int i = _pop_idx(r, c);
            if (_pop_blk[i]) col[n++] = i;   // 自下而上
        }
        if (n == 0) continue;
        // 该列压到新列 ncols
        for (int k = 0; k < n; k++) {
            const int old_i = col[k];
            const int nr = POP_ROWS - n + k;
            new_idx[_pop_idx(nr, ncols)] = old_i;
        }
        ncols++;
    }

    // 重建 _pop_blk/_pop_col; 旧对象动画挪到新位 (cell 布局参数重算)
    lv_obj_t* old_blk[POP_ROWS * POP_COLS];
    int old_col[POP_ROWS * POP_COLS];
    for (int i = 0; i < POP_ROWS * POP_COLS; i++) {
        old_blk[i] = _pop_blk[i];
        old_col[i] = _pop_col[i];
        _pop_blk[i] = nullptr;
    }

    const int gap = 4;
    int cell = std::min((SCREEN_W - 48 - (POP_COLS - 1) * gap) / POP_COLS,
                        (SCREEN_H - TAB_H - 64 - 16 - (POP_ROWS - 1) * gap) / POP_ROWS);
    if (cell > 84) cell = 84;
    const int bw = POP_COLS * cell + (POP_COLS - 1) * gap;
    const int x0 = (SCREEN_W - bw) / 2;
    const int y0 = 64 + ((SCREEN_H - TAB_H - 64 - 16) - (POP_ROWS * cell + (POP_ROWS - 1) * gap)) / 2;

    for (int nr = 0; nr < POP_ROWS; nr++) {
        for (int nc = 0; nc < POP_COLS; nc++) {
            const int ni = _pop_idx(nr, nc);
            const int oi = new_idx[ni];
            if (oi < 0) continue;   // 空格
            lv_obj_t* b = old_blk[oi];
            _pop_blk[ni] = b;
            _pop_col[ni] = old_col[oi];
            if (!b) continue;
            const int nx = x0 + nc * (cell + gap);
            const int ny = y0 + nr * (cell + gap);
            const int cx = lv_obj_get_x(b), cy = lv_obj_get_y(b);
            if (cx == nx && cy == ny) continue;
            lv_anim_t ax, ay;
            lv_anim_init(&ax);
            lv_anim_set_var(&ax, b);
            lv_anim_set_exec_cb(&ax, _pop_move_x_cb);
            lv_anim_set_values(&ax, cx, nx);
            lv_anim_set_time(&ax, 200);
            lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
            lv_anim_start(&ax);
            lv_anim_init(&ay);
            lv_anim_set_var(&ay, b);
            lv_anim_set_exec_cb(&ay, _pop_move_y_cb);
            lv_anim_set_values(&ay, cy, ny);
            lv_anim_set_time(&ay, 200);
            lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
            lv_anim_start(&ay);
        }
    }
}

// ── 结束判定 ──
bool AppGames::_popAny()
{
    for (int i = 0; i < POP_ROWS * POP_COLS; i++)
        if (_pop_blk[i]) return true;
    return false;
}

bool AppGames::_popHasMoves()
{
    for (int r = 0; r < POP_ROWS; r++) {
        for (int c = 0; c < POP_COLS; c++) {
            const int i = _pop_idx(r, c);
            if (!_pop_blk[i]) continue;
            if (c + 1 < POP_COLS && _pop_blk[_pop_idx(r, c + 1)] &&
                _pop_col[_pop_idx(r, c + 1)] == _pop_col[i]) return true;
            if (r + 1 < POP_ROWS && _pop_blk[_pop_idx(r + 1, c)] &&
                _pop_col[_pop_idx(r + 1, c)] == _pop_col[i]) return true;
        }
    }
    return false;
}

void AppGames::_popEnd(bool cleared)
{
    if (cleared) {
        _pop_score += POP_CLEAR_BONUS;
        _popRefreshScore();
        gm_audio::jingle((const float[]){523.25f, 659.25f, 1046.5f},
                         (const double[]){0.12, 0.12, 0.24}, 3);
    } else if (_popHasMoves()) {
        return;   // 还能玩
    } else {
        gm_audio::jingle((const float[]){330.0f, 233.1f},
                         (const double[]){0.14, 0.26}, 2);
    }

    const bool is_best = _pop_score > _pop_best;
    if (is_best) _nvsSet("gm_pop_best", _pop_score);
    _pop_best = std::max(_pop_best, _pop_score);
    _popRefreshScore();

    char body[96];
    if (cleared) snprintf(body, sizeof(body), "全清通杀! +2000 · 分数 %d · 最佳 %d",
                          _pop_score, _pop_best);
    else snprintf(body, sizeof(body), "没有可消的了 · 分数 %d · 最佳 %d",
                  _pop_score, _pop_best);
    _showModal(cleared ? "漂亮!" : "结束", body, "再来一局", "继续看看", nullptr);
}

// ── 拆屏前清动画 (voice_input 同款: 不清则 anim 继续写已释放对象) ──
void AppGames::_popKillAnims()
{
    for (int i = 0; i < POP_ROWS * POP_COLS; i++) {
        if (_pop_blk[i]) lv_anim_delete(_pop_blk[i], nullptr);
    }
    for (lv_obj_t* b : _pop_dying) {
        if (b) lv_anim_delete(b, nullptr);
    }
}
