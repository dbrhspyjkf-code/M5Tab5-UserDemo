/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD.
 * SPDX-License-Identifier: MIT
 */
#include "assets_partition.h"
#include <string.h>

#ifndef PLATFORM_BUILD_DESKTOP

#include <esp_partition.h>
#include <mooncake_log.h>
#include <string.h>

namespace {

constexpr uint32_t kMagic   = 0x42545341;  // 'ASTB' (little-endian)
constexpr uint32_t kVersion = 1;

// 与 pack_assets.py 的打包格式逐字段对应（全部 4 字节成员，无对齐填充）
struct Entry {
    char     name[32];
    uint32_t offset;
    uint32_t size;
};
struct Table {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
    Entry    entries[];
};

const char* TAG = "res";

struct MappedAssets {
    const uint8_t* base = nullptr;
    const Table*   table = nullptr;
};

// 惰性映射整个 assets 分区，只做一次
const MappedAssets& map_assets()
{
    static MappedAssets mapped;
    static bool tried = false;
    if (tried) return mapped;
    tried = true;

    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "res");
    if (!part) {
        mclog::tagError(TAG, "partition 'res' not found");
        return mapped;
    }

    const void* ptr       = nullptr;
    static esp_partition_mmap_handle_t handle;  // 释放时机 = 进程生命周期
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle) != ESP_OK) {
        mclog::tagError(TAG, "mmap failed");
        return mapped;
    }

    const Table* t = reinterpret_cast<const Table*>(ptr);
    if (t->magic != kMagic || t->version != kVersion) {
        mclog::tagError(TAG, "bad table (assets.img not flashed?) — magic=0x{:x} version={}", t->magic, t->version);
        return mapped;
    }

    mapped.base  = static_cast<const uint8_t*>(ptr);
    mapped.table = t;
    mclog::tagInfo(TAG, "mapped {} bytes, {} assets", part->size, t->count);
    return mapped;
}

}  // namespace

namespace assets_part {

const uint8_t* find(const char* name, size_t* size)
{
    const MappedAssets& mapped = map_assets();
    if (!mapped.table) return nullptr;

    for (uint32_t i = 0; i < mapped.table->count; ++i) {
        const Entry& e = mapped.table->entries[i];
        if (strncmp(e.name, name, sizeof(e.name)) == 0) {
            if (size) *size = e.size;
            return mapped.base + e.offset;
        }
    }
    return nullptr;
}

}  // namespace assets_part

#elif defined(PLATFORM_BUILD_WEB)

// web: 资产镜像(与真机 res 分区同格式 'ASTB', 三档 cbin 字体)由 worker
// 启动时同步 XHR 拉取到 malloc 缓冲 —— 不链进 wasm(巨型 const 数组会
// 触发启动期越界, 且运行时拉取可独立缓存/版本化)。
extern "C" const char* tab5_web_assets_name;

namespace {
struct TableW {
    uint32_t magic, version, count, reserved;
};
struct EntryW {
    char name[32];
    uint32_t offset, size;
};
uint8_t* g_img = nullptr;

// 同步 XHR (worker 内合法) → malloc 缓冲; 返回指针或 nullptr
__attribute__((import_name("tab5_fetch_assets"))) extern "C" uint8_t* tab5_fetch_assets(const char* url);
}  // namespace

namespace assets_part {

void web_preload()
{
    if (g_img) return;
    g_img = tab5_fetch_assets(tab5_web_assets_name);
    auto* t = reinterpret_cast<const TableW*>(g_img);
    if (!g_img || t->magic != 0x42545341 || t->version != 1) g_img = nullptr;
}

const uint8_t* find(const char* name, size_t* size)
{
    if (!g_img) return nullptr;
    auto* t = reinterpret_cast<const TableW*>(g_img);
    auto* e = reinterpret_cast<const EntryW*>(g_img + sizeof(TableW));
    for (uint32_t i = 0; i < t->count; ++i) {
        if (strncmp(e[i].name, name, sizeof(EntryW::name)) == 0) {
            if (size) *size = e[i].size;
            return g_img + e[i].offset;
        }
    }
    return nullptr;
}
}  // namespace assets_part

#else  // PLATFORM_BUILD_DESKTOP

namespace assets_part {
const uint8_t* find(const char*, size_t*) { return nullptr; }
}  // namespace assets_part

#endif
