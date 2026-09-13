// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include "Heap/z/zIndexDistributor.hpp"

namespace MapleRuntime {
class ZIndexDistributorClaimTree {
#if defined(MRT_GC_UNIT_TESTS)
    friend class ZIndexDistributorTest;
#endif
    static constexpr size_t N = 4;
    static constexpr size_t ClaimLevels = N - 1;
    static constexpr size_t CacheLineSize = 64;

    static constexpr size_t claim_level_size(size_t level)
    {
        return level == 0 ? 1 : 16 * claim_level_size(level - 1);
    }

    static constexpr size_t claim_level_end_index(size_t level)
    {
        return level == 0 ? CacheLineSize / sizeof(std::atomic<size_t>) :
            claim_level_size(level) + claim_level_end_index(level - 1);
    }

    static size_t claim_level_index(const size_t* indices, size_t level);

    static size_t claim_index(const size_t* indices, size_t level);

    size_t level_segment_size(size_t level) const;

    template<typename Function>
    void claim_and_do(Function function, size_t* indices, size_t level);

    template<typename Function>
    void steal_and_do(Function function, size_t* indices, size_t level);

public:
    explicit ZIndexDistributorClaimTree(size_t count);

    ~ZIndexDistributorClaimTree() { std::free(allocation); }
    ZIndexDistributorClaimTree(const ZIndexDistributorClaimTree&) = delete;
    ZIndexDistributorClaimTree& operator=(const ZIndexDistributorClaimTree&) = delete;

    template<typename Function>
    void do_indices(Function function);

    static size_t get_count(size_t maxCount);

private:
    size_t lastLevelSegmentSizeShift;
    void* allocation;
    std::atomic<size_t>* claims;
};
}

namespace MapleRuntime {
inline size_t ZIndexDistributorClaimTree::claim_level_index(const size_t* indices, size_t level)
{
        assert(level > 0);
        size_t index = 0;
        for (size_t i = 0; i < level; ++i) {
            index = index * 16 + indices[i];
        }
        return index;
    }
}

namespace MapleRuntime {
inline size_t ZIndexDistributorClaimTree::claim_index(const size_t* indices, size_t level)
{
        return level == 0 ? 0 : claim_level_end_index(level - 1) + claim_level_index(indices, level);
    }
}

namespace MapleRuntime {
inline size_t ZIndexDistributorClaimTree::level_segment_size(size_t level) const
{
        return level == ClaimLevels ? size_t(1) << lastLevelSegmentSizeShift : 16;
    }
}

namespace MapleRuntime {
inline size_t ZIndexDistributorClaimTree::get_count(size_t maxCount)
{
        size_t count = claim_level_size(ClaimLevels);
        while (count < maxCount) {
            count <<= 1;
        }
        return count;
    }
}

namespace MapleRuntime {
template<typename Function>
inline void ZIndexDistributorClaimTree::claim_and_do(Function function, size_t* indices, size_t level)
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
}

namespace MapleRuntime {
template<typename Function>
inline void ZIndexDistributorClaimTree::steal_and_do(Function function, size_t* indices, size_t level)
{
        for (indices[level] = 0; indices[level] < level_segment_size(level); ++indices[level]) {
            const size_t nextLevel = level + 1;
            claim_and_do(function, indices, nextLevel);
            if (nextLevel < ClaimLevels) {
                steal_and_do(function, indices, nextLevel);
            }
        }
    }
}

namespace MapleRuntime {
template<typename Function>
inline void ZIndexDistributorClaimTree::do_indices(Function function)
{
        size_t indices[N];
        claim_and_do(function, indices, 0);
        steal_and_do(function, indices, 0);
    }
}

namespace MapleRuntime {
inline ZIndexDistributorClaimTree::ZIndexDistributorClaimTree(size_t count) : lastLevelSegmentSizeShift(0)
{
        assert(count >= claim_level_size(ClaimLevels) && (count & (count - 1)) == 0);
        for (size_t leafSize = count / claim_level_size(ClaimLevels); leafSize > 1; leafSize >>= 1) {
            ++lastLevelSegmentSizeShift;
        }
        const size_t entries = claim_level_end_index(ClaimLevels);
        const size_t claimAlignment = MRT_PAGE_SIZE;
        allocation = std::malloc(entries * sizeof(std::atomic<size_t>) + claimAlignment);
        if (allocation == nullptr) {
            std::abort();
        }
        const uintptr_t aligned = (reinterpret_cast<uintptr_t>(allocation) + claimAlignment - 1) &
            ~(uintptr_t(claimAlignment) - 1);
        claims = reinterpret_cast<std::atomic<size_t>*>(aligned);
        for (size_t i = 0; i < entries; ++i) {
            new (&claims[i]) std::atomic<size_t>(0);
        }
    }
}
