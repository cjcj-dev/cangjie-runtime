// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_Z_GRANULE_MAP_H
#define MRT_Z_GRANULE_MAP_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

#include "Base/Globals.h"
#include "Base/Panic.h"
#include "Common/TypeDef.h"
#include "Common/ColourEncoding.h"

#include "Heap/z/zIndexDistributor.hpp"
namespace MapleRuntime {

// zGranuleMap.hpp:31-61 + zGranuleMap.inline.hpp:37-103
// Indexed by (addr - base) / granule. T is a pointer type stored atomically.
// ZGC ZGranuleMap(size_t max_offset) allocates in the constructor.
template <typename T>
class ZGranuleMap {
public:
    ZGranuleMap() : _size(0), _map(nullptr), _base(0), _heapSize(0), _granule(0) {}

    ZGranuleMap(size_t max_offset, MAddress base, size_t granule)
        : _size(0), _map(nullptr), _base(base), _heapSize(max_offset), _granule(granule)
    {
        CHECK(granule != 0 && max_offset != 0 && base % granule == 0 && max_offset % granule == 0);
        CHECK(IsRepresentableLow48Range(base, max_offset));
        const size_t n = max_offset / granule;
        auto* map = static_cast<std::atomic<T>*>(std::calloc(n, sizeof(std::atomic<T>)));
        CHECK(map != nullptr);
        _map = map;
        _size = n;
    }

    ZGranuleMap(const ZGranuleMap&) = delete;
    ZGranuleMap& operator=(const ZGranuleMap&) = delete;

    ZGranuleMap(ZGranuleMap&& other) noexcept
        : _size(other._size), _map(other._map), _base(other._base), _heapSize(other._heapSize),
          _granule(other._granule)
    {
        other._map = nullptr;
        other._size = 0;
    }

    ZGranuleMap& operator=(ZGranuleMap&& other) noexcept
    {
        if (this != &other) {
            std::free(_map);
            _size = other._size;
            _map = other._map;
            _base = other._base;
            _heapSize = other._heapSize;
            _granule = other._granule;
            other._map = nullptr;
            other._size = 0;
        }
        return *this;
    }

    ~ZGranuleMap()
    {
        std::free(_map);
        _map = nullptr;
    }

    bool Ready() const { return _map != nullptr; }

    bool offset_for_address(MAddress addr, zoffset* result) const
    {
        if (!Ready() || addr < _base) {
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

    T get(zoffset offset) const;

    void put(zoffset offset, T value);

    void put(zoffset offset, size_t size, T value);

    const T* addr(zoffset offset) const;
    T* addr(zoffset offset);

    size_t granule() const;
    size_t size() const;
    MAddress base() const;

    T at(size_t index) const;

    T get_acquire(zoffset offset) const;
    void release_put(zoffset offset, T value);
    void release_put(zoffset offset, size_t size, T value);

private:
    size_t index_for_offset(zoffset offset) const;

    size_t _size;
    std::atomic<T>* _map;
    MAddress _base;
    size_t _heapSize;
    size_t _granule;
};

} // namespace MapleRuntime

#include "Heap/z/zGranuleMap.inline.hpp"

#endif // MRT_Z_GRANULE_MAP_H
