#pragma once
/*
 * theme — 全局配色主题（2026-10-26 起）。
 *
 * 所有 app（主页/收音机/播放器/计时器/HA/小智）的界面颜色统一从这里取，
 * 不再各写死 hex。新增或调整主题只改本文件 kThemes 表。
 *
 * - 当前主题索引持久化在 NVS("theme_idx")，首次用到时惰性读取；
 * - 主页齿轮弹窗里有「主题」按钮循环切换，切换后主页立即重建，
 *   其他 app 下次打开时生效（各自 onOpen 时取快照）。
 * - ok/warn/danger 是语义色（阈值/告警），默认各主题同值，一般不用动。
 */
#include <cstdint>
#include <cstdlib>
#include <string>
#include <hal/hal.h>

namespace theme {

struct Palette {
    const char* name;       // 中文主题名（切换按钮上显示）
    uint32_t hero;          // 主卡/焦点面板底（整屏最亮的一档强调底）
    uint32_t accent;        // 强调色: 高亮文字/进度填充/激活指示/指针
    uint32_t press;         // 按钮按下/激活态底色
    uint32_t card[4];       // 卡片梯度（从上到下渐深；card[1]≈普通卡底）
    uint32_t border;        // 卡片描边
    uint32_t text_hi;       // 主文字
    uint32_t text_soft;     // 次文字
    uint32_t text_dim;      // 弱文字/标签/编号
    uint32_t ok, warn, danger;   // 语义色（绿/琥珀/红）
    uint32_t track;         // 进度条轨道/滑轨/刻度盘内嵌底
};

inline const Palette kThemes[] = {
    {"松绿", 0x204234, 0x34D399, 0x2F8066,
        {0x1B362C, 0x173024, 0x13291F, 0x0F221A}, 0x2C4433,
        0xF2F7F3, 0xC7D6CB, 0x7E9486,
        0x34D399, 0xFBBF24, 0xEF4444, 0x0A1811},
    {"暖橙", 0x6E3A16, 0xF5A263, 0xB3661F,
        {0x3A2412, 0x30200F, 0x271A0C, 0x1D1309}, 0x5C3A1C,
        0xF2F7F3, 0xC7D6CB, 0x7E9486,
        0x34D399, 0xFBBF24, 0xEF4444, 0x160D04},
    {"海蓝", 0x1C4258, 0x4FA3FF, 0x2F6FA8,
        {0x16334A, 0x122B40, 0x0E2233, 0x0A1A28}, 0x2A4A6A,
        0xF2F7F3, 0xC7D6CB, 0x7E9486,
        0x34D399, 0xFBBF24, 0xEF4444, 0x081522},
    {"岩紫", 0x3A2C55, 0xB08CFF, 0x6A4FA0,
        {0x2B213F, 0x251C37, 0x1E162C, 0x171022}, 0x453663,
        0xF2F7F3, 0xC7D6CB, 0x7E9486,
        0x34D399, 0xFBBF24, 0xEF4444, 0x120D1E},
};

inline int count() { return (int)(sizeof(kThemes) / sizeof(kThemes[0])); }

namespace detail {
inline int g_idx = -1;   // -1 = 尚未从 NVS 读取
}

inline int cur_index()
{
    if (detail::g_idx < 0) {
        // 桌面调试: TAB5_THEME=n 可强制指定主题(设备上忽略, 走 NVS)
        int v = -1;
        if (const char* e = getenv("TAB5_THEME")) v = atoi(e);
        if (v < 0) v = atoi(GetHAL()->getConfig("theme_idx", "0").c_str());
        detail::g_idx = (v >= 0 && v < count()) ? v : 0;
    }
    return detail::g_idx;
}

inline const Palette& cur() { return kThemes[cur_index()]; }

// 切换并持久化（i 允许越界，自动取模回绕）
inline void set_index(int i)
{
    detail::g_idx = ((i % count()) + count()) % count();
    GetHAL()->setConfig("theme_idx", std::to_string(detail::g_idx));
}

// 循环切到下一个主题，返回新主题名
inline const char* cycle()
{
    set_index(cur_index() + 1);
    return cur().name;
}

}  // namespace theme
