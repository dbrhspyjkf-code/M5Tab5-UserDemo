#pragma once
#include <stdint.h>

// 5x7 LED 点阵字体, ASCII 0x20..0x7E (95 字符). 共享给「灯阵」/「自选股」LED ticker.
// 格式: 每字符 7 字节, 每字节 1 行. bit4=最左列 … bit0=最右列 (bit5..6=0).
//      第 0 字节 = 顶部行, 第 6 字节 = 底部行.
// 小写字母按全 0 占位, 渲染端把 a-z 转成大写后查表.
#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t font5x7[95][7];

#ifdef __cplusplus
}
#endif
