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

#include "Common/TypeDef.h"
#include "Common/ColourEncoding.h"

namespace MapleRuntime {

// zIndexDistributor.inline.hpp:100-320. Three 16-way claim levels lead to
// power-of-two leaf segments; stealing descends through the same claim tree.
class ZIndexDistributorClaimTree {
    friend class ZIndexDistributorTest;
    static constexpr size_t N = 4;
    static constexpr size_t ClaimLevels = N - 1;
    static constexpr size_t CacheLineSize = 64;
    static constexpr size_t ClaimAlignment = 4096;

    static constexpr size_t claim_level_size(size_t level)
    {
        return level == 0 ? 1 : 16 * claim_level_size(level - 1);
    }

    static constexpr size_t claim_level_end_index(size_t level)
    {
        return level == 0 ? CacheLineSize / sizeof(std::atomic<size_t>) :
            claim_level_size(level) + claim_level_end_index(level - 1);
    }

    static size_t claim_level_index(const size_t* indices, size_t level)
    {
        assert(level > 0);
        size_t index = 0;
        for (size_t i = 0; i < level; ++i) {
            index = index * 16 + indices[i];
        }
        return index;
    }

    static size_t claim_index(const size_t* indices, size_t level)
    {
        return level == 0 ? 0 : claim_level_end_index(level - 1) + claim_level_index(indices, level);
    }

    size_t level_segment_size(size_t level) const
    {
        return level == ClaimLevels ? size_t(1) << lastLevelSegmentSizeShift : 16;
    }

    template<typename Function>
    void claim_and_do(Function function, size_t* indices, size_t level)
    {
        if (level < N) {
            const size_t ci = claim_index(indices, level);
            while ((indices[level] = claims[ci].fetch_add(1, std::memory_order_relaxed)) <
                   level_segment_size(level)) {
                claim_and_do(function, indices, level + 1);
            }
            return;
        }
        const size_t segmentStart = claim_level_index(indices, ClaimLevels) << lastLevelSegmentSizeShift;
        // Like ZGC's claim-tree, callbacks do not terminate distribution early.
        function(segmentStart + indices[N - 1]);
    }

    template<typename Function>
    void steal_and_do(Function function, size_t* indices, size_t level)
    {
        for (indices[level] = 0; indices[level] < level_segment_size(level); ++indices[level]) {
            const size_t nextLevel = level + 1;
            claim_and_do(function, indices, nextLevel);
            if (nextLevel < ClaimLevels) {
                steal_and_do(function, indices, nextLevel);
            }
        }
    }

public:
    explicit ZIndexDistributorClaimTree(size_t count) : lastLevelSegmentSizeShift(0)
    {
        assert(count >= claim_level_size(ClaimLevels) && (count & (count - 1)) == 0);
        for (size_t leafSize = count / claim_level_size(ClaimLevels); leafSize > 1; leafSize >>= 1) {
            ++lastLevelSegmentSizeShift;
        }
        const size_t entries = claim_level_end_index(ClaimLevels);
        allocation = std::malloc(entries * sizeof(std::atomic<size_t>) + ClaimAlignment);
        if (allocation == nullptr) {
            std::abort();
        }
        const uintptr_t aligned = (reinterpret_cast<uintptr_t>(allocation) + ClaimAlignment - 1) &
            ~(uintptr_t(ClaimAlignment) - 1);
        claims = reinterpret_cast<std::atomic<size_t>*>(aligned);
        for (size_t i = 0; i < entries; ++i) {
            new (&claims[i]) std::atomic<size_t>(0);
        }
    }

    ~ZIndexDistributorClaimTree() { std::free(allocation); }
    ZIndexDistributorClaimTree(const ZIndexDistributorClaimTree&) = delete;
    ZIndexDistributorClaimTree& operator=(const ZIndexDistributorClaimTree&) = delete;

    template<typename Function>
    void do_indices(Function function)
    {
        size_t indices[N];
        claim_and_do(function, indices, 0);
        steal_and_do(function, indices, 0);
    }

    static size_t get_count(size_t maxCount)
    {
        size_t count = claim_level_size(ClaimLevels);
        while (count < maxCount) {
            count <<= 1;
        }
        return count;
    }

private:
    size_t lastLevelSegmentSizeShift;
    void* allocation;
    std::atomic<size_t>* claims;
};

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
        assert(size % _granule == 0);
        const size_t count = size / _granule;
        assert(start <= _size && count <= _size - start);
        for (size_t i = 0; i < count; ++i) {
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

    T at(size_t index) const
    {
        if (_map == nullptr || index >= _size) {
            return T();
        }
        return _map[index].load(std::memory_order_acquire);
    }

private:
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

// zPageTable.inline.hpp:79-99. The map includes reservation holes. A page
// spanning several granules is emitted only at its own start granule.
// z_globals.hpp:99 selects claim-tree by default. The diagnostic strategy
// selector is not part of this port.
template<typename T>
class ZPageTableParallelIterator {
public:
    explicit ZPageTableParallelIterator(const ZGranuleMap<T>& table)
        : table(table), distributor(ZIndexDistributorClaimTree::get_count(table.size())) {}

    template<typename Function>
    void do_pages(Function function)
    {
        distributor.do_indices([&](size_t index) {
            T page = table.at(index);
            if (page != T()) {
                const size_t startIndex = (page->GetRegionStart() - table.base()) / table.granule();
                if (index == startIndex) {
                    return function(page);
                }
            }
            return true;
        });
    }

private:
    const ZGranuleMap<T>& table;
    ZIndexDistributorClaimTree distributor;
};

} // namespace MapleRuntime

#endif // MRT_Z_GRANULE_MAP_H
