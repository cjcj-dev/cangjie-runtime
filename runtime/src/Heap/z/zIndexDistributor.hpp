// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
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

#include "Heap/z/zIndexDistributor.inline.hpp"
