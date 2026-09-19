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
#include "Heap/z/zGlobals.hpp"
namespace MapleRuntime {

// zGranuleMap.inline.hpp:37-103: one slot per ZGC granule.
template <typename T>
class ZGranuleMap {
public:
    ZGranuleMap() : _size(0), _map(nullptr) {}
    explicit ZGranuleMap(size_t max_offset)
        : _size(max_offset >> ZGranuleSizeShift),
          _map(static_cast<std::atomic<T>*>(std::calloc(_size, sizeof(std::atomic<T>))))
    {
        CHECK(max_offset != 0 && max_offset % ZGranuleSize == 0);
        CHECK(_map != nullptr);
    }
    ZGranuleMap(const ZGranuleMap&) = delete;
    ZGranuleMap& operator=(const ZGranuleMap&) = delete;
    ZGranuleMap(ZGranuleMap&& other) noexcept : _size(other._size), _map(other._map)
    {
        other._size = 0;
        other._map = nullptr;
    }
    ZGranuleMap& operator=(ZGranuleMap&& other) noexcept
    {
        if (this != &other) {
            std::free(_map);
            _size = other._size;
            _map = other._map;
            other._size = 0;
            other._map = nullptr;
        }
        return *this;
    }
    ~ZGranuleMap() { std::free(_map); }
    bool Ready() const { return _map != nullptr; }

    T get(zoffset offset) const;

    void put(zoffset offset, T value);

    void put(zoffset offset, size_t size, T value);

    const T* addr(zoffset offset) const;
    T* addr(zoffset offset);

    size_t size() const;

    T at(size_t index) const;

    T get_acquire(zoffset offset) const;
    void release_put(zoffset offset, T value);
    void release_put(zoffset offset, size_t size, T value);

private:
    size_t index_for_offset(zoffset offset) const;

    size_t _size;
    std::atomic<T>* _map;
};

} // namespace MapleRuntime

#include "Heap/z/zGranuleMap.inline.hpp"

#endif // MRT_Z_GRANULE_MAP_H
