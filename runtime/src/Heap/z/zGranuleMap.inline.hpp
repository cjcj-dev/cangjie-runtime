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
        assert(size % _granule == 0);
        const size_t count = size / _granule;
        assert(start <= _size && count <= _size - start);
        for (size_t i = 0; i < count; ++i) {
            _map[start + i].store(value, std::memory_order_release);
        }
    }
}

namespace MapleRuntime {
template<typename T>
inline bool ZGranuleMap<T>::compare_exchange(zoffset offset, T& expected, T desired)
{
        return _map[index_for_offset(offset)].compare_exchange_strong(expected, desired, std::memory_order_release,
                                                                      std::memory_order_acquire);
    }
}

namespace MapleRuntime {
template<typename T>
inline T ZGranuleMap<T>::exchange(zoffset offset, T value)
{
        return _map[index_for_offset(offset)].exchange(value, std::memory_order_acq_rel);
    }
}

namespace MapleRuntime {
template<typename T>
inline size_t ZGranuleMap<T>::granule() const
{ return _granule; }
}

namespace MapleRuntime {
template<typename T>
inline size_t ZGranuleMap<T>::size() const
{ return _size; }
}

namespace MapleRuntime {
template<typename T>
inline MAddress ZGranuleMap<T>::base() const
{ return _base; }
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
        return static_cast<size_t>(raw(offset)) / _granule;
    }
}
