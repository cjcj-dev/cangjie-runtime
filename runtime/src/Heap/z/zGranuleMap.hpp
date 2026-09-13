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

#include "Base/Globals.h"
#include "Common/TypeDef.h"
#include "Common/ColourEncoding.h"

#include "Heap/z/zIndexDistributor.hpp"
namespace MapleRuntime {

// zIndexDistributor.inline.hpp:100-320. Three 16-way claim levels lead to
// power-of-two leaf segments; stealing descends through the same claim tree.
// zGranuleMap.hpp:31-61 + zGranuleMap.inline.hpp:37-103
// Indexed by (addr - base) / granule. T is a pointer type stored atomically.
template <typename T>
class ZGranuleMap {
public:
    ZGranuleMap() : _size(0), _map(nullptr), _base(0), _heapSize(0), _granule(0) {}

    bool Initialize(MAddress base, size_t heapSize, size_t granule)
    {
        if (_map != nullptr) {
            return _base == base && _heapSize == heapSize && _granule == granule;
        }
        // zPageTable.cpp:37-42 sizes the map by the highest available offset,
        // including reservation holes, rather than by reserved capacity.
        if (granule == 0 || heapSize == 0 || base % granule != 0 || heapSize % granule != 0 ||
            !IsRepresentableLow48Range(base, heapSize)) {
            return false;
        }
        const size_t n = heapSize / granule;
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

    void Reset()
    {
        std::free(_map);
        _map = nullptr;
        _size = 0;
        _base = 0;
        _heapSize = 0;
        _granule = 0;
    }
#if defined(MRT_GC_UNIT_TESTS)
    void ResetForTest() { Reset(); }
#endif

    bool Ready() const { return _map != nullptr; }

    // Sole MAddress -> zoffset gate for this heap address space. The upper
    // bound is exclusive: an offset at heapSize is not an address that this
    // map may turn into an array access.
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

    bool compare_exchange(zoffset offset, T& expected, T desired);

    T exchange(zoffset offset, T value);

    size_t granule() const;
    size_t size() const;
    MAddress base() const;

    template<typename Function>
    void visit_unique(Function function) const
    {
        T last{};
        for (size_t i = 0; i < _size; ++i) {
            T value = at(i);
            if (value != T() && value != last) {
                function(value);
                last = value;
            }
        }
    }

    T at(size_t index) const;

private:
    size_t index_for_offset(zoffset offset) const;

    size_t _size;
    std::atomic<T>* _map;
    MAddress _base;
    size_t _heapSize;
    size_t _granule;
};

} // namespace MapleRuntime

#include "Heap/z/zPageTable.hpp"

#include "Heap/z/zGranuleMap.inline.hpp"

#endif // MRT_Z_GRANULE_MAP_H
