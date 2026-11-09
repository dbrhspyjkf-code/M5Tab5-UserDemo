#pragma once
/*
 * PsramJson — nlohmann::json 的 PSRAM 分配器版。
 *
 * 大 JSON 文档树（HA /api/states 483 实体、邮件列表等）解析成 DOM 后体积
 * 通常是 body 的 3-5 倍；用默认分配器整棵落在内部堆里、且每个轮询周期反复
 * 构建/销毁，是内部堆碎片化（OOM 断言/假死）的主力源。本头文件把节点与
 * 字符串整棵放到 PSRAM；解析出的常驻小体量结构仍可用标准 std::string。
 *
 * 用法:
 *   auto j = PsramJson::parse(body);   // DOM 在 PSRAM
 *   e.name = j.value("name", "");      // 取出的 std::string 落内部堆
 *
 * 桌面/网页构建（web 同时定义 PLATFORM_BUILD_DESKTOP）回退标准 json，
 * 不为冻结的 web 端引入任何新代码。(原版在 app_settings.cpp, 2026-11-09
 * 提为共享头供 ha_client 复用)
 */
#include <string>
#include <nlohmann/json.hpp>

#ifndef PLATFORM_BUILD_DESKTOP

#include <cstdint>
#include <new>
#include <esp_heap_caps.h>

template <class T>
struct PsramAllocator
{
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <class U> PsramAllocator(const PsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t n)
    {
        auto* p = static_cast<T*>(heap_caps_malloc(n * sizeof(T),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!p) throw std::bad_alloc();
        return p;
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};
template <class T, class U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept { return true; }
template <class T, class U>
bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept { return false; }

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
using PsramJson   = nlohmann::basic_json<std::map, std::vector, PsramString, bool,
                                         std::int64_t, std::uint64_t, double, PsramAllocator>;

#else

using PsramString = std::string;
using PsramJson   = nlohmann::json;

#endif  // PLATFORM_BUILD_DESKTOP
