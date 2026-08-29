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
    ZGranuleMap() : _size(0), _map(nullptr), _base(0), _heapSize(0), _granule(0) {}

    bool Initialize(MAddress base, size_t heapSize, size_t granule)
    {
        if (_map != nullptr || granule == 0 || heapSize == 0) {
            return _map != nullptr;
        }
        const size_t n = heapSize / granule + ((heapSize % granule) != 0 ? 1 : 0);
        auto* map = static_cast<std::atomic<T>*>(std::calloc(n, sizeof(std::atomic<T>)));
        if (map == nullptr) {
            return false;
        }
        _map = map;
        _size = n;
        _base = base;
        _heapSize = heapSize;
        _granule = granule;
        return true;
    }

    ~ZGranuleMap()
    {
        std::free(_map);
        _map = nullptr;
    }

#if defined(MRT_GC_UNIT_TESTS)
    void ResetForTest()
    {
        std::free(_map);
        _map = nullptr;
        _size = 0;
        _base = 0;
        _heapSize = 0;
        _granule = 0;
    }
#endif

    bool Ready() const { return _map != nullptr; }

    // Sole MAddress -> zoffset gate for this heap address space. The upper
    // bound is exclusive: an offset at heapSize is not an address that this
    // map may turn into an array access.
    bool offset_for_address(MAddress addr, zoffset* result) const
    {
        if (addr < _base) {
            return false;
        }
        const MAddress offset = addr - _base;
        if (offset >= _heapSize) {
            return false;
        }
        if (result != nullptr) {
            *result = static_cast<zoffset>(offset);
        }
        return true;
    }

    T get(zoffset offset) const
    {
        return at(index_for_offset(offset));
    }

    void put(zoffset offset, T value)
    {
        _map[index_for_offset(offset)].store(value, std::memory_order_release);
    }

    void put(zoffset offset, size_t size, T value)
    {
        const size_t start = index_for_offset(offset);
        const size_t count = size / _granule + ((size % _granule) != 0 ? 1 : 0);
        for (size_t i = 0; i < count && start + i < _size; ++i) {
            _map[start + i].store(value, std::memory_order_release);
        }
    }

    bool compare_exchange(zoffset offset, T& expected, T desired)
    {
        return _map[index_for_offset(offset)].compare_exchange_strong(expected, desired, std::memory_order_release,
                                                                      std::memory_order_acquire);
    }

    T exchange(zoffset offset, T value)
    {
        return _map[index_for_offset(offset)].exchange(value, std::memory_order_acq_rel);
    }

    size_t granule() const { return _granule; }
    size_t size() const { return _size; }
    MAddress base() const { return _base; }

private:
    T at(size_t index) const
    {
        if (_map == nullptr || index >= _size) {
            return T();
        }
        return _map[index].load(std::memory_order_acquire);
    }

    size_t index_for_offset(zoffset offset) const
    {
        return static_cast<size_t>(raw(offset)) / _granule;
    }

    size_t _size;
    std::atomic<T>* _map;
    MAddress _base;
    size_t _heapSize;
    size_t _granule;
};

} // namespace MapleRuntime

#endif // MRT_Z_GRANULE_MAP_H
