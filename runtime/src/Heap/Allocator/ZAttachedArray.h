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

namespace MapleRuntime {

// zAttachedArray.hpp:29-50 + zAttachedArray.inline.hpp:32-84
// Object and its array are one allocation: [ObjectT | padding | ArrayT[length]].
template <typename ObjectT, typename ArrayT>
class ZAttachedArray {
public:
    static size_t object_size()
    {
        const size_t alignment = sizeof(ArrayT);
        return (sizeof(ObjectT) + alignment - 1) & ~(alignment - 1);
    }

    static size_t array_size(size_t length) { return sizeof(ArrayT) * length; }

    static void* alloc(size_t length)
    {
        const size_t size = object_size() + array_size(length);
        void* const addr = std::malloc(size);
        if (addr == nullptr) {
            return nullptr;
        }
        void* const arrayAddr = reinterpret_cast<char*>(addr) + object_size();
        ::new (arrayAddr) ArrayT[length]();
        return addr;
    }

    static void free(ObjectT* obj) { std::free(obj); }

    explicit ZAttachedArray(size_t length) : _length(length) {}

    size_t length() const { return _length; }

    ArrayT* operator()(const ObjectT* obj) const
    {
        return reinterpret_cast<ArrayT*>(reinterpret_cast<uintptr_t>(obj) + object_size());
    }

private:
    const size_t _length;
};

} // namespace MapleRuntime

#endif // MRT_Z_ATTACHED_ARRAY_H
