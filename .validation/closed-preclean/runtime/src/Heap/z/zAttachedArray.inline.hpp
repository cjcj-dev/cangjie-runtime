// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zAttachedArray.hpp"

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline size_t ZAttachedArray<ObjectT, ArrayT>::object_size()
{
        const size_t alignment = sizeof(ArrayT);
        return (sizeof(ObjectT) + alignment - 1) & ~(alignment - 1);
    }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline size_t ZAttachedArray<ObjectT, ArrayT>::array_size(size_t length)
{ return sizeof(ArrayT) * length; }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline bool ZAttachedArray<ObjectT, ArrayT>::allocation_size(size_t length, size_t* size)
{
        if (length > (std::numeric_limits<size_t>::max() - object_size()) / sizeof(ArrayT)) {
            return false;
        }
        *size = object_size() + array_size(length);
        return true;
    }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline void ZAttachedArray<ObjectT, ArrayT>::initialize(void* addr, size_t length)
{
        void* const arrayAddr = reinterpret_cast<char*>(addr) + object_size();
        ::new (arrayAddr) ArrayT[length]();
    }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline void* ZAttachedArray<ObjectT, ArrayT>::alloc(size_t length)
{
        size_t size;
        if (!allocation_size(length, &size)) {
            return nullptr;
        }
        void* const addr = std::malloc(size);
        if (addr == nullptr) {
            return nullptr;
        }
        initialize(addr, length);
        return addr;
    }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
template <typename Allocator>
inline void* ZAttachedArray<ObjectT, ArrayT>::alloc(Allocator* allocator, size_t length)
{
        const size_t size = object_size() + array_size(length);
        void* const addr = allocator->alloc(size);
        if (addr == nullptr) {
            return nullptr;
        }
        initialize(addr, length);
        return addr;
    }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline void ZAttachedArray<ObjectT, ArrayT>::free(ObjectT* obj)
{ std::free(obj); }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline size_t ZAttachedArray<ObjectT, ArrayT>::length() const
{ return _length; }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline ArrayT* ZAttachedArray<ObjectT, ArrayT>::operator()(const ObjectT* obj) const
{
        return reinterpret_cast<ArrayT*>(reinterpret_cast<uintptr_t>(obj) + object_size());
    }
}

namespace MapleRuntime {
template <typename ObjectT, typename ArrayT>
inline ZAttachedArray<ObjectT, ArrayT>::ZAttachedArray(size_t length) : _length(length)
{}
}
