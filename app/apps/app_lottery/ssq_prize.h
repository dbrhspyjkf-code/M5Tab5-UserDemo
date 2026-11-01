#pragma once
/*
 * ssq_prize.h — 双色球对奖判定引擎 (header-only, 设备端离线判定)。
 *
 * 开奖数据来自 hermes /api/ssq/history (issues[].grades 带官方各奖级
 * 单注奖金, 一二等奖每期浮动); 判定是纯逻辑, 不需要再回服务端。
 *
 * 规则 (红 r 命中数 / 蓝 b 是否命中):
 *   一等 6r+1b   二等 6r   三等 5r+1b   四等 5r 或 4r+1b
 *   五等 4r 或 3r+1b   六等 中蓝 (2r+1b/1r+1b/0r+1b)
 * 三~六等奖金固定 (3000/200/10/5), 一二等从当期 grades 取 (缺失=0, UI
 * 显示"浮动")。
 *
 * 复式: 红 n(6≤n≤20) 蓝 m(1≤m≤16), 枚举 C(n,6)×m 注逐注判定统计
 * (C(20,6)=38760 ×16 ≈ 62 万次简单比较, P4/模拟器毫秒级)。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ssq {

// 一期开奖 (对应 /api/ssq/history 的 issues[i] / latest)
struct DrawIssue {
    std::string code;             // "2026124"
    std::string date;             // "2026-10-29"
    std::string week;             // "四"
    int reds[6] = {0, 0, 0, 0, 0, 0};
    int blue = 0;
    long sales = 0;               // 销售额 (元)
    long pool = 0;                // 奖池 (元)
    struct Grade { int level; long money; long num; };
    std::vector<Grade> grades;    // level 1-6
};

// 三~六等固定奖金; 一二等查当期 grades, 缺失返回 0 (UI 显示"浮动")
inline long gradeMoney(int level, const DrawIssue& d)
{
    switch (level) {
        case 3: return 3000;
        case 4: return 200;
        case 5: return 10;
        case 6: return 5;
        case 1: case 2:
            for (const auto& g : d.grades)
                if (g.level == level) return g.money;
            return 0;
        default: return 0;
    }
}

// 单注判定结果
struct PrizeResult {
    int red_hits = 0;
    bool blue_hit = false;
    int level = 0;    // 0=未中, 1-6 奖级
    long money = 0;   // 该注奖金 (一二等浮动值按当期官方单注计)
};

inline PrizeResult judgeOne(const DrawIssue& d, const int* reds6, int blue)
{
    PrizeResult r;
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 6; j++) {
            if (reds6[i] == d.reds[j]) { r.red_hits++; break; }
        }
    }
    r.blue_hit = (blue == d.blue);
    const int rh = r.red_hits, bh = r.blue_hit ? 1 : 0;
    if (rh == 6 && bh) r.level = 1;
    else if (rh == 6) r.level = 2;
    else if (rh == 5 && bh) r.level = 3;
    else if (rh == 5 || (rh == 4 && bh)) r.level = 4;
    else if (rh == 4 || (rh == 3 && bh)) r.level = 5;
    else if (bh) r.level = 6;
    r.money = r.level ? gradeMoney(r.level, d) : 0;
    return r;
}

// 复式选号 (红 6-20 蓝 1-16; 单式是 n=6,m=1 特例)
struct Pick {
    std::vector<int> reds;
    std::vector<int> blues;
    int multiplier = 1;   // 票面 ×N 倍投: 奖金/投注额按注乘 (拍照识别携带)
};

// 复式枚举判定统计
struct MultiResult {
    long bet_count = 0;       // 总注数 C(n,6)×m
    long bet_money = 0;       // 投注额 = bet_count×2
    long level_counts[7] = {0, 0, 0, 0, 0, 0, 0};  // [1..6] 各奖级注数, [0] 未中
    long total_money = 0;     // 总奖金 (一二等按当期官方单注浮动值计)
    bool has_floating = false; // 中了一/二等且当期 grades 缺失 (金额未知)
};

inline MultiResult judgeMulti(const DrawIssue& d, const Pick& p)
{
    MultiResult out;
    const int n = (int)p.reds.size(), m = (int)p.blues.size();
    if (n < 6 || n > 20 || m < 1 || m > 16) return out;

    // 预取 6 个奖级金额 (避免每注遍历 grades)
    long gm[7] = {0, 0, 0, 3000, 200, 10, 5};
    for (const auto& g : d.grades)
        if (g.level >= 1 && g.level <= 6) gm[g.level] = g.money;

    // C(n,6) 组合枚举 (字典序 next combination)
    int idx[6] = {0, 1, 2, 3, 4, 5};
    const int combos = [n] {
        long c = 1;
        for (int i = 0; i < 6; i++) c = c * (n - i) / (i + 1);
        return (int)c;
    }();
    int reds6[6];
    for (int cnum = 0; cnum < combos; cnum++) {
        for (int i = 0; i < 6; i++) reds6[i] = p.reds[idx[i]];
        for (int b = 0; b < m; b++) {
            PrizeResult r = judgeOne(d, reds6, p.blues[b]);
            out.level_counts[r.level]++;
            if (r.level >= 3) {
                out.total_money += gm[r.level];
            } else if (r.level >= 1) {
                if (gm[r.level] > 0) out.total_money += gm[r.level];
                else out.has_floating = true;
            }
        }
        // next combination
        int i = 5;
        while (i >= 0 && idx[i] == n - 6 + i) i--;
        if (i < 0) break;
        idx[i]++;
        for (int j = i + 1; j < 6; j++) idx[j] = idx[j - 1] + 1;
    }
    out.bet_count = (long)combos * m;
    out.bet_money = out.bet_count * 2;
    return out;
}

}  // namespace ssq
