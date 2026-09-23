// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zUtils.hpp:31-61
#pragma once
#include <cstddef>
#include <cstdint>
#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {
class ZUtils {
public:
    // Thread
    static const char* thread_name();

    // Allocation
    static uintptr_t alloc_aligned_unfreeable(size_t alignment, size_t size);

    // Object
    // I2 (PLAN §5): object sizes are bytes from TypeInfo/GCTib (BaseObject::GetSize);
    // there is no HeapWord layer, so bytes_to_words/words_to_bytes/object_size are
    // not part of this unit (zUtils.inline.hpp:52-63).
    static void object_copy_disjoint(zaddress from, zaddress to, size_t size);
    static void object_copy_conjoint(zaddress from, zaddress to, size_t size);
    static void object_copy_disjoint_atomic(zaddress from, zaddress to, size_t offset, size_t size);

    // Memory
    static void fill(uintptr_t* addr, size_t count, uintptr_t value);
    template <typename T>
    static void copy_disjoint(T* dest, const T* src, size_t count);
    template <typename T>
    static void copy_disjoint(T* dest, const T* src, int count);

    // Sort
    template <typename T, typename Comparator>
    static void sort(T* array, size_t count, Comparator comparator);
    template <typename T, typename Comparator>
    static void sort(T* array, int count, Comparator comparator);
};
} // namespace MapleRuntime
