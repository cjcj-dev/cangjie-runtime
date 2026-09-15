// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zUtils.inline.hpp:24-112
#pragma once
#include "Heap/z/zUtils.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Base/Log.h"
#include "Heap/z/zAddress.inline.hpp"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
// zUtils.inline.hpp:37-50
inline uintptr_t ZUtils::alloc_aligned_unfreeable(size_t alignment, size_t size)
{
    const size_t padded_size = size + (alignment - 1);
    void* const addr = std::malloc(padded_size);
    if (addr == nullptr) {
        LOG(RTLOG_FATAL, "ZGC alloc_aligned_unfreeable malloc failed (%zu bytes)", padded_size);
    }
    void* const aligned_addr =
        reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(addr) + (alignment - 1)) & ~(alignment - 1));

    memset(aligned_addr, 0, size);

    // Since free expects pointers returned by malloc, aligned_addr cannot be
    // freed since it is most likely not the same as addr after alignment.
    return reinterpret_cast<uintptr_t>(aligned_addr);
}

// zUtils.inline.hpp:65-67 (Copy::aligned_disjoint_words)
inline void ZUtils::object_copy_disjoint(zaddress from, zaddress to, size_t size)
{
    memcpy(reinterpret_cast<void*>(untype(to)), reinterpret_cast<const void*>(untype(from)), size);
#if defined(CANGJIE_TSAN_SUPPORT)
    // I17 (PLAN §5): TSAN shadow follows the relocated object; HotSpot has no TSAN heap.
    Sanitizer::TsanFixShadow(reinterpret_cast<void*>(untype(from)), reinterpret_cast<void*>(untype(to)), size);
#endif
}

// zUtils.inline.hpp:69-73 (Copy::aligned_conjoint_words)
inline void ZUtils::object_copy_conjoint(zaddress from, zaddress to, size_t size)
{
    if (from != to) {
        memmove(reinterpret_cast<void*>(untype(to)), reinterpret_cast<const void*>(untype(from)), size);
#if defined(CANGJIE_TSAN_SUPPORT)
        Sanitizer::TsanFixShadow(reinterpret_cast<void*>(untype(from)), reinterpret_cast<void*>(untype(to)), size);
#endif
    }
}

// zUtils.inline.hpp:75-80 (Copy::disjoint_words_atomic: word-at-a-time stores)
inline void ZUtils::object_copy_disjoint_atomic(zaddress from, zaddress to, size_t offset, size_t size)
{
    const uintptr_t from_addr = untype(from) + offset;
    const uintptr_t to_addr = untype(to) + offset;

    assert((size & (sizeof(uintptr_t) - 1)) == 0);
    const uintptr_t* src = reinterpret_cast<const uintptr_t*>(from_addr);
    uintptr_t* dst = reinterpret_cast<uintptr_t*>(to_addr);
    for (size_t count = size / sizeof(uintptr_t); count > 0; --count) {
        *dst++ = *src++;
    }
}

// zUtils.inline.hpp:82-85
template <typename T>
inline void ZUtils::copy_disjoint(T* dest, const T* src, size_t count)
{
    memcpy(dest, src, sizeof(T) * count);
}

// zUtils.inline.hpp:87-92
template <typename T>
inline void ZUtils::copy_disjoint(T* dest, const T* src, int count)
{
    assert(count >= 0);

    copy_disjoint(dest, src, static_cast<size_t>(count));
}

// zUtils.inline.hpp:94-103
template <typename T, typename Comparator>
inline void ZUtils::sort(T* array, size_t count, Comparator comparator)
{
    using SortType = int(const void*, const void*);
    using ComparatorType = int(const T*, const T*);

    ComparatorType* const comparator_fn_ptr = comparator;

    // We rely on ABI compatibility between ComparatorType and SortType
    qsort(array, count, sizeof(T), reinterpret_cast<SortType*>(comparator_fn_ptr));
}

// zUtils.inline.hpp:105-110
template <typename T, typename Comparator>
inline void ZUtils::sort(T* array, int count, Comparator comparator)
{
    assert(count >= 0);

    sort(array, static_cast<size_t>(count), comparator);
}
} // namespace MapleRuntime
