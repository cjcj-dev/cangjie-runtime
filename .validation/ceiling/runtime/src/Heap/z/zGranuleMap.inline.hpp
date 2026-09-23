// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zGranuleMap.hpp"

namespace MapleRuntime {
template<typename T>
inline T ZGranuleMap<T>::get(zoffset offset) const
{
        return at(index_for_offset(offset));
    }
}

namespace MapleRuntime {
template<typename T>
inline void ZGranuleMap<T>::put(zoffset offset, T value)
{
        __atomic_store(_map + index_for_offset(offset), &value, __ATOMIC_RELAXED);
    }
}

namespace MapleRuntime {
template<typename T>
inline void ZGranuleMap<T>::put(zoffset offset, size_t size, T value)
{
        const size_t start = index_for_offset(offset);
        assert(size % ZGranuleSize == 0);
        const size_t count = size >> ZGranuleSizeShift;
        assert(start <= _size && count <= _size - start);
        for (size_t i = 0; i < count; ++i) {
            __atomic_store(_map + start + i, &value, __ATOMIC_RELAXED);
        }
    }
}

namespace MapleRuntime {
template<typename T>
inline T ZGranuleMap<T>::get_acquire(zoffset offset) const
{
        T value;
        __atomic_load(_map + index_for_offset(offset), &value, __ATOMIC_ACQUIRE);
        return value;
    }
}

namespace MapleRuntime {
template<typename T>
inline void ZGranuleMap<T>::release_put(zoffset offset, T value)
{
        __atomic_store(_map + index_for_offset(offset), &value, __ATOMIC_RELEASE);
    }
}

namespace MapleRuntime {
template<typename T>
inline void ZGranuleMap<T>::release_put(zoffset offset, size_t size, T value)
{
        std::atomic_thread_fence(std::memory_order_release);
        put(offset, size, value);
    }
}


namespace MapleRuntime {
template<typename T>
inline size_t ZGranuleMap<T>::size() const
{ return _size; }
}


namespace MapleRuntime {
template<typename T>
inline T ZGranuleMap<T>::at(size_t index) const
{
        assert(index < _size);
        T value;
        __atomic_load(_map + index, &value, __ATOMIC_RELAXED);
        return value;
    }
}

namespace MapleRuntime {
template<typename T>
inline size_t ZGranuleMap<T>::index_for_offset(zoffset offset) const
{
        const size_t index = static_cast<size_t>(raw(offset)) >> ZGranuleSizeShift;
        assert(index < _size);
        return index;
    }
}

namespace MapleRuntime {
template<typename T>
inline const T* ZGranuleMap<T>::addr(zoffset offset) const
{
        return _map + index_for_offset(offset);
    }

template<typename T>
inline T* ZGranuleMap<T>::addr(zoffset offset)
{
        return _map + index_for_offset(offset);
    }
}
