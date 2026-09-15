// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zIndexDistributor.inline.hpp:24-356
#pragma once
#include "Heap/z/zIndexDistributor.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Globals.h"
#include "Base/Log.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/z_globals.hpp"

namespace MapleRuntime {
class ZIndexDistributorStriped {
    static const int MemSize = 4096;
    static const int StripeCount = MemSize / ZCacheLineSize;

    const int _count;
    // For claiming a stripe
    volatile int _claim_stripe;
    // For claiming inside a stripe
    char _mem[MemSize + ZCacheLineSize];

    int claim_stripe()
    {
        return __atomic_fetch_add(&_claim_stripe, 1, __ATOMIC_RELAXED);
    }

    volatile int* claim_addr(int index)
    {
        const uintptr_t aligned = (reinterpret_cast<uintptr_t>(_mem) + ZCacheLineSize - 1) & ~(ZCacheLineSize - 1);
        return reinterpret_cast<volatile int*>(aligned + (size_t)index * ZCacheLineSize);
    }

public:
    ZIndexDistributorStriped(int count)
        : _count(count),
          _claim_stripe(0),
          _mem()
    {
        memset(_mem, 0, MemSize + ZCacheLineSize);
    }

    template <typename Function>
    void do_indices(Function function)
    {
        const int stripe_max = _count / StripeCount;

        // Use claiming
        for (int i; (i = claim_stripe()) < StripeCount;) {
            for (int index; (index = __atomic_fetch_add(claim_addr(i), 1, __ATOMIC_RELAXED)) < stripe_max;) {
                if (!function(i * stripe_max + index)) {
                    return;
                }
            }
        }

        // Use stealing
        for (int i = 0; i < StripeCount; i++) {
            for (int index; (index = __atomic_fetch_add(claim_addr(i), 1, __ATOMIC_RELAXED)) < stripe_max;) {
                if (!function(i * stripe_max + index)) {
                    return;
                }
            }
        }
    }

    static size_t get_count(size_t max_count)
    {
        // Must be multiple of the StripeCount
        return (max_count + StripeCount - 1) / StripeCount * StripeCount;
    }
};

class ZIndexDistributorClaimTree {
    friend class ZIndexDistributorTest;

private:
    // The N - 1 levels are used to claim a segment in the
    // next level the Nth level claims an index.
    static constexpr int N = 4;
    static constexpr int ClaimLevels = N - 1;

    // Describes the how the number of indices increases when going up from the given level
    static constexpr int level_multiplier(int level)
    {
        assert(level < ClaimLevels && "Must be");
        constexpr int array[ClaimLevels]{16, 16, 16};
        return array[level];
    }

    // Number of indices in one segment at the last level
    const int     _last_level_segment_size_shift;

    // For deallocation
    char*         _malloced;

    // Contains the tree of claim variables
    volatile int* _claim_array;

    // Claim index functions

    // Number of claim entries at the given level
    static constexpr int claim_level_size(int level)
    {
        if (level == 0) {
            return 1;
        }

        return level_multiplier(level - 1) * claim_level_size(level - 1);
    }

    // The index the next level starts at
    static constexpr int claim_level_end_index(int level)
    {
        if (level == 0) {

            // First level uses padding
            return ZCacheLineSize / sizeof(int);
        }

        return claim_level_size(level) + claim_level_end_index(level - 1);
    }

    static constexpr int claim_level_start_index(int level)
    {
        return claim_level_end_index(level - 1);
    }

    // Total size used to hold all claim variables
    static size_t claim_variables_size()
    {
        return sizeof(int) * (size_t)claim_level_end_index(ClaimLevels);
    }

    // Returns the index of the start of the current segment of the current level
    static constexpr int claim_level_index_accumulate(int* indices, int level, int acc = 1)
    {
        if (level == 0) {
            return acc * indices[level];
        }

        return acc * indices[level] + claim_level_index_accumulate(indices, level - 1, acc * level_multiplier(level));
    }

    static constexpr int claim_level_index(int* indices, int level)
    {
        assert(level > 0 && "Must be");

        // The claim index for the current level is found in the previous levels
        return claim_level_index_accumulate(indices, level - 1);
    }

    static constexpr int claim_index(int* indices, int level)
    {
        if (level == 0) {
            return 0;
        }

        return claim_level_start_index(level) + claim_level_index(indices, level);
    }

    // Claim functions

    int claim(int index)
    {
        return __atomic_fetch_add(&_claim_array[index], 1, __ATOMIC_RELAXED);
    }

    int claim_at(int* indices, int level)
    {
        const int index = claim_index(indices, level);
        const int value = claim(index);
        return value;
    }

    template <typename Function>
    void claim_and_do(Function function, int* indices, int level)
    {
        if (level < N) {
            // Visit ClaimLevels and the last level
            const int ci = claim_index(indices, level);
            for (indices[level] = 0; (indices[level] = claim(ci)) < level_segment_size(level);) {
                claim_and_do(function, indices, level + 1);
            }
            return;
        }

        doit(function, indices);
    }

    template <typename Function>
    void steal_and_do(Function function, int* indices, int level)
    {
        for (indices[level] = 0; indices[level] < level_segment_size(level); indices[level]++) {
            const int next_level = level + 1;
            // First try to claim at next level
            claim_and_do(function, indices, next_level);
            // Then steal at next level
            if (next_level < ClaimLevels) {
                steal_and_do(function, indices, next_level);
            }
        }
    }

    // Functions to claimed values to an index

    static constexpr int levels_size(int level)
    {
        if (level == 0) {
            return level_multiplier(0);
        }

        return level_multiplier(level) * levels_size(level - 1);
    }

    static int constexpr level_to_last_level_count_coverage(int level)
    {
        return levels_size(ClaimLevels - 1) / levels_size(level);
    }

    static int constexpr calculate_last_level_count(int* indices, int level = 0)
    {
        if (level == N - 1) {
            return 0;
        }

        return indices[level] * level_to_last_level_count_coverage(level) + calculate_last_level_count(indices, level + 1);
    }

    int calculate_index(int* indices)
    {
        const int segment_start = calculate_last_level_count(indices) << _last_level_segment_size_shift;
        return segment_start + indices[N - 1];
    }

    int level_segment_size(int level)
    {
        if (level == ClaimLevels) {
            return 1 << _last_level_segment_size_shift;
        }

        return level_multiplier(level);
    }

    template <typename Function>
    void doit(Function function, int* indices)
    {
        const int index = calculate_index(indices);

        function(index);
    }

    static int last_level_segment_size_shift(int count)
    {
        const int last_level_size = count / levels_size(ClaimLevels - 1);
        assert(levels_size(ClaimLevels - 1) * last_level_size == count && "Not exactly divisible");

        return Log2Exact(last_level_size);
    }

public:
    ZIndexDistributorClaimTree(int count)
        : _last_level_segment_size_shift(last_level_segment_size_shift(count)),
          _malloced((char*)std::malloc(claim_variables_size() + MRT_PAGE_SIZE)),
          _claim_array((volatile int*)(((uintptr_t)_malloced + MRT_PAGE_SIZE - 1) & ~(uintptr_t)(MRT_PAGE_SIZE - 1)))
    {
        assert((levels_size(ClaimLevels - 1) << _last_level_segment_size_shift) == count && "Incorrectly setup");
        CHECK_DETAIL(_malloced != nullptr, "ZIndexDistributorClaimTree malloc failed");

        memset(_malloced, 0, claim_variables_size() + MRT_PAGE_SIZE);
    }

    ~ZIndexDistributorClaimTree()
    {
        std::free(_malloced);
    }

    ZIndexDistributorClaimTree(const ZIndexDistributorClaimTree&) = delete;
    ZIndexDistributorClaimTree& operator=(const ZIndexDistributorClaimTree&) = delete;

    template <typename Function>
    void do_indices(Function function)
    {
        int indices[N];
        claim_and_do(function, indices, 0 /* level */);
        steal_and_do(function, indices, 0 /* level */);
    }

    static size_t get_count(size_t max_count)
    {
        // Must be at least claim_level_size(ClaimLevels) and a power of two
        const size_t min_count = claim_level_size(ClaimLevels);
        return RoundUpPowerOfTwo(max_count > min_count ? max_count : min_count);
    }
};

// Using dynamically allocated objects just to be able to evaluate
// different strategies. Revert when one has been choosen.

inline void* ZIndexDistributor::create_strategy(int count)
{
    switch (ZIndexDistributorStrategy) {
    case 0: return new ZIndexDistributorClaimTree(count);
    case 1: return new ZIndexDistributorStriped(count);
    default: LOG(RTLOG_FATAL, "Unknown ZIndexDistributorStrategy"); return nullptr;
    };
}

inline ZIndexDistributor::ZIndexDistributor(int count)
    : _strategy(create_strategy(count)) {}

inline ZIndexDistributor::~ZIndexDistributor()
{
    switch (ZIndexDistributorStrategy) {
    case 0: delete static_cast<ZIndexDistributorClaimTree*>(_strategy); break;
    case 1: delete static_cast<ZIndexDistributorStriped*>(_strategy); break;
    default: LOG(RTLOG_FATAL, "Unknown ZIndexDistributorStrategy"); break;
    };
}

template <typename Strategy>
inline Strategy* ZIndexDistributor::strategy()
{
    return static_cast<Strategy*>(_strategy);
}

template <typename Function>
inline void ZIndexDistributor::do_indices(Function function)
{
    switch (ZIndexDistributorStrategy) {
    case 0: strategy<ZIndexDistributorClaimTree>()->do_indices(function); break;
    case 1: strategy<ZIndexDistributorStriped>()->do_indices(function); break;
    default: LOG(RTLOG_FATAL, "Unknown ZIndexDistributorStrategy");
    };
}

inline size_t ZIndexDistributor::get_count(size_t max_count)
{
    size_t required_count = 0;
    switch (ZIndexDistributorStrategy) {
    case 0: required_count = ZIndexDistributorClaimTree::get_count(max_count); break;
    case 1: required_count = ZIndexDistributorStriped::get_count(max_count); break;
    default: LOG(RTLOG_FATAL, "Unknown ZIndexDistributorStrategy");
    };

    assert(max_count <= required_count && "unsupported max_count");

    return required_count;
}
} // namespace MapleRuntime
