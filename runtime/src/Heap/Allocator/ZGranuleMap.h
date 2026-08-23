// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_GRANULE_MAP_H
#define MRT_Z_GRANULE_MAP_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "Common/TypeDef.h"

namespace MapleRuntime {

// zGranuleMap.hpp:31-61 + zGranuleMap.inline.hpp:37-103
// Indexed by (addr - base) / granule. T is a pointer type stored atomically.
template <typename T>
class ZGranuleMap {
public:
    ZGranuleMap() : _size(0), _map(nullptr), _base(0), _granule(0) {}

    bool Initialize(MAddress base, size_t heapSize, size_t granule)
    {
        if (_map != nullptr || granule == 0 || heapSize == 0) {
            return _map != nullptr;
        }
        const size_t n = heapSize / granule + 1;
        auto* map = static_cast<std::atomic<T>*>(std::calloc(n, sizeof(std::atomic<T>)));
        if (map == nullptr) {
            return false;
        }
        _map = map;
        _size = n;
        _base = base;
        _granule = granule;
        return true;
    }

    ~ZGranuleMap()
    {
        std::free(_map);
        _map = nullptr;
    }

    bool Ready() const { return _map != nullptr; }

    size_t index_for_offset(MAddress addr) const
    {
        if (addr < _base || _granule == 0) {
            return SIZE_MAX;
        }
        const size_t index = static_cast<size_t>(addr - _base) / _granule;
        return index < _size ? index : SIZE_MAX;
    }

    T at(size_t index) const
    {
        if (_map == nullptr || index >= _size) {
            return T();
        }
        return _map[index].load(std::memory_order_acquire);
    }

    T get(MAddress addr) const
    {
        const size_t index = index_for_offset(addr);
        if (index == SIZE_MAX) {
            return T();
        }
        return at(index);
    }

    void put(MAddress addr, T value)
    {
        const size_t index = index_for_offset(addr);
        if (index == SIZE_MAX) {
            return;
        }
        _map[index].store(value, std::memory_order_release);
    }

    void put(MAddress addr, size_t size, T value)
    {
        const size_t start = index_for_offset(addr);
        if (start == SIZE_MAX || _granule == 0) {
            return;
        }
        const size_t count = size / _granule + ((size % _granule) != 0 ? 1 : 0);
        for (size_t i = 0; i < count && start + i < _size; ++i) {
            _map[start + i].store(value, std::memory_order_release);
        }
    }

    bool compare_exchange(MAddress addr, T& expected, T desired)
    {
        const size_t index = index_for_offset(addr);
        if (index == SIZE_MAX) {
            return false;
        }
        return _map[index].compare_exchange_strong(expected, desired, std::memory_order_release,
                                                   std::memory_order_acquire);
    }

    T exchange(MAddress addr, T value)
    {
        const size_t index = index_for_offset(addr);
        if (index == SIZE_MAX) {
            return T();
        }
        return _map[index].exchange(value, std::memory_order_acq_rel);
    }

    size_t granule() const { return _granule; }
    size_t size() const { return _size; }
    MAddress base() const { return _base; }

private:
    size_t _size;
    std::atomic<T>* _map;
    MAddress _base;
    size_t _granule;
};

} // namespace MapleRuntime

#endif // MRT_Z_GRANULE_MAP_H
