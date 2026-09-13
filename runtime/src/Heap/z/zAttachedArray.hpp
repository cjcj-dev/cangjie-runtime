// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_ATTACHED_ARRAY_H
#define MRT_Z_ATTACHED_ARRAY_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <limits>

namespace MapleRuntime {

// zAttachedArray.hpp:29-50 + zAttachedArray.inline.hpp:32-84
// Object and its array are one allocation: [ObjectT | padding | ArrayT[length]].
template <typename ObjectT, typename ArrayT>
class ZAttachedArray {
public:
    static size_t object_size();

    static size_t array_size(size_t length);

    // Check before multiplication/addition, including the caller's arena budget.
    static bool allocation_size(size_t length, size_t* size);

    static void initialize(void* addr, size_t length);

    static void* alloc(size_t length);

    static void free(ObjectT* obj);

    explicit ZAttachedArray(size_t length);

    size_t length() const;

    ArrayT* operator()(const ObjectT* obj) const;

private:
    const size_t _length;
};

} // namespace MapleRuntime

#include "Heap/z/zAttachedArray.inline.hpp"

#endif // MRT_Z_ATTACHED_ARRAY_H
