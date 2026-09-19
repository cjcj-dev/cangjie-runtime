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
        _map[index_for_offset(offset)].store(value, std::memory_order_release);
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
            _map[start + i].store(value, std::memory_order_release);
        }
    }
}

namespace MapleRuntime {
template<typename T>
inline T ZGranuleMap<T>::get_acquire(zoffset offset) const
{
        return _map[index_for_offset(offset)].load(std::memory_order_acquire);
    }
}

namespace MapleRuntime {
template<typename T>
inline void ZGranuleMap<T>::release_put(zoffset offset, T value)
{
        std::atomic_thread_fence(std::memory_order_release);
        _map[index_for_offset(offset)].store(value, std::memory_order_relaxed);
    }
}

namespace MapleRuntime {
template<typename T>
inline void ZGranuleMap<T>::release_put(zoffset offset, size_t size, T value)
{
        std::atomic_thread_fence(std::memory_order_release);
        const size_t start = index_for_offset(offset);
        const size_t count = size >> ZGranuleSizeShift;
        for (size_t i = 0; i < count; ++i) {
            _map[start + i].store(value, std::memory_order_relaxed);
        }
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
        if (_map == nullptr || index >= _size) {
            return T();
        }
        return _map[index].load(std::memory_order_acquire);
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
        static_assert(sizeof(std::atomic<T>) == sizeof(T), "atomic slot must alias T");
        return reinterpret_cast<const T*>(_map + index_for_offset(offset));
    }

template<typename T>
inline T* ZGranuleMap<T>::addr(zoffset offset)
{
        static_assert(sizeof(std::atomic<T>) == sizeof(T), "atomic slot must alias T");
        return reinterpret_cast<T*>(_map + index_for_offset(offset));
    }
}
