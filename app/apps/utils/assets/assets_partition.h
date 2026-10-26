/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD.
 * SPDX-License-Identifier: MIT
 *
 * assets 分区只读访问。分区内容由 platforms/tab5/tools/pack_assets.py 生成，
 * 分区标签为 "res"（刻意避开 xiaozhi 自己的 "assets" 下载分区）：
 *
 *   [16B 表头: magic 'ASTB' / version / count / reserved]
 *   [count × 40B 条目: name[32] / offset / size]   （offset 相对分区起始）
 *   [各资产数据，4 字节对齐]
 *
 * 设备上整分区 esp_partition_mmap 一次，资产数据原址引用（与 rodata 等价的
 * cache 访问，零拷贝）；桌面模拟器无分区概念，find() 恒返回 nullptr，
 * 调用方回退到链接进来的 C 数组资源。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace assets_part {

// 按名查找资产；成功返回只读数据指针（生命周期 = 整个分区 mmap，恒有效），
// 可选用 size 带出字节数。失败（无分区 / 表未刷 / 名字不存在 / 桌面）返回 nullptr。
const uint8_t* find(const char* name, size_t* size = nullptr);

}  // namespace assets_part
