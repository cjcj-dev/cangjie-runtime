// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Port of OpenJDK test/hotspot/gtest/gc/z/test_zIndexDistributor.cpp (the
// claim-tree geometry tests, through the ZIndexDistributorTest friend), plus
// the distribution invariant every strategy must uphold: each index in
// [0, count) is claimed exactly once across concurrent workers.

#include <atomic>
#include <thread>
#include <vector>

#include "Heap/z/zIndexDistributor.inline.hpp"
#include "Heap/z/zPageTable.hpp"
#include "gc_unittest.hpp"

namespace MapleRuntime {

class ZIndexDistributorTest {
public:
    static void test_claim_tree_claim_level_size()
    {
        // max_index: 16, 16, 16, rest
        // claim level: 1, 16, 16 * 16, 16 * 16 * 16
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_size(0), 1);
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_size(1), 16);
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_size(2), 16 * 16);
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_size(3), 16 * 16 * 16);
    }

    static void test_claim_tree_claim_level_end_index()
    {
        // First level is padded
        const int first_level_end = int(ZCacheLineSize / sizeof(int));
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_end_index(0), first_level_end);
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_end_index(1), first_level_end + 16);
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_end_index(2), first_level_end + 16 + 16 * 16);
        GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_end_index(3), first_level_end + 16 + 16 * 16 + 16 * 16 * 16);
    }

    static void test_claim_tree_claim_index()
    {
        // First level should always give index 0
        {
            int indices[4] = {0, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 0), 0);
        }
        {
            int indices[4] = {1, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 0), 0);
        }
        {
            int indices[4] = {15, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 0), 0);
        }
        {
            int indices[4] = {16, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 0), 0);
        }

        // Second level should depend on first claimed index

        // Second-level start after first-level padding
        const int second_level_start = int(ZCacheLineSize / sizeof(int));

        {
            int indices[4] = {0, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 1), second_level_start);
        }
        {
            int indices[4] = {1, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 1), second_level_start + 1);
        }
        {
            int indices[4] = {15, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 1), second_level_start + 15);
        }

        // Third level

        const int third_level_start = second_level_start + 16;

        {
            int indices[4] = {0, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 2), third_level_start);
        }
        {
            int indices[4] = {1, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 2), third_level_start + 1 * 16);
        }
        {
            int indices[4] = {15, 0, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 2), third_level_start + 15 * 16);
        }
        {
            int indices[4] = {1, 2, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 2), third_level_start + 1 * 16 + 2);
        }
        {
            int indices[4] = {15, 14, 0, 0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_index(indices, 2), third_level_start + 15 * 16 + 14);
        }
    }

    static void test_claim_tree_claim_level_index()
    {
        {
            int indices[4] = {0,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 1), 0);
        }
        {
            int indices[4] = {1,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 1), 1);
        }

        {
            int indices[4] = {0,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 2), 0);
        }
        {
            int indices[4] = {1,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 2), 1 * 16);
        }
        {
            int indices[4] = {2,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 2), 2 * 16);
        }
        {
            int indices[4] = {2,1,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 2), 2 * 16 + 1);
        }

        {
            int indices[4] = {0,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 3), 0);
        }
        {
            int indices[4] = {1,0,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 3), 1 * 16 * 16);
        }
        {
            int indices[4] = {1,2,0,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 3), 1 * 16 * 16 + 2 * 16);
        }
        {
            int indices[4] = {1,2,1,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 3), 1 * 16 * 16 + 2 * 16 + 1);
        }
        {
            int indices[4] = {1,2,3,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 3), 1 * 16 * 16 + 2 * 16 + 3);
        }
        {
            int indices[4] = {1,2,3,0};
            GC_EXPECT_EQ(ZIndexDistributorClaimTree::claim_level_index(indices, 2), 1 * 16 + 2);
        }
    }

    // Every claim is one fetch-and-add on its claim word (zIndexDistributor.
    // inline.hpp:174-176). Repeated do_indices on an exhausted tree makes each
    // caller take exactly one terminating claim at level 0 per call, so the
    // level-0 word ends at 16 (the useful claims) + workers * calls. A claim
    // that is not atomic loses increments under this contention.
    static void test_claim_tree_concurrent_claims_are_accounted()
    {
        constexpr int workers = 16;
        constexpr int calls = 10000;
        constexpr size_t count = 4096; // claim_level_size(ClaimLevels): one index per leaf segment
        ZIndexDistributorClaimTree tree(static_cast<int>(count));
        std::vector<std::atomic<unsigned>> visits(count);
        for (auto& visit : visits) {
            visit.store(0);
        }
        std::vector<std::thread> threads;
        for (int worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&]() {
                for (int call = 0; call < calls; ++call) {
                    tree.do_indices([&](int index) {
                        visits[static_cast<size_t>(index)].fetch_add(1, std::memory_order_relaxed);
                        return true;
                    });
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        size_t duplicates = 0;
        for (const auto& visit : visits) {
            if (visit.load() != 1) {
                ++duplicates;
            }
        }
        GC_EXPECT_EQ(duplicates, 0u);
        // Target invariant: the level-0 claim word accounts for every claim.
        const int level0 = tree._claim_array[ZIndexDistributorClaimTree::claim_index(nullptr, 0)];
        GC_EXPECT_EQ(level0, 16 + workers * calls);
    }

    // Mechanism-only control, not the index-uniqueness invariant.
    // The claim primitive itself (zIndexDistributor.inline.hpp:174-176): one
    // fetch-and-add per call, so contending callers never lose an increment.
    static void test_claim_tree_claim_is_atomic()
    {
        constexpr int workers = 16;
        constexpr int iterations = 200000;
        ZIndexDistributorClaimTree tree(4096);
        std::vector<std::thread> threads;
        for (int worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&]() {
                for (int i = 0; i < iterations; ++i) {
                    (void)tree.claim(0);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        GC_EXPECT_EQ(tree._claim_array[0], workers * iterations);
    }

    // Invariant 3 (spec): every index in [0, count) is claimed exactly once.
    template <typename Distributor>
    static void distribute_once(size_t requested, size_t workers)
    {
        const size_t count = Distributor::get_count(requested);
        GC_EXPECT_TRUE(count >= requested);
        Distributor distributor(static_cast<int>(count));
        std::vector<std::atomic<unsigned>> visits(count);
        for (auto& visit : visits) {
            visit.store(0);
        }
        std::atomic<size_t> out_of_range{ 0 };
        std::vector<std::thread> threads;
        for (size_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&]() {
                distributor.do_indices([&](int index) {
                    if (index < 0 || static_cast<size_t>(index) >= count) {
                        out_of_range.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                    visits[static_cast<size_t>(index)].fetch_add(1, std::memory_order_relaxed);
                    return true;
                });
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        GC_EXPECT_EQ(out_of_range.load(), 0u);
        size_t duplicates = 0;
        size_t missed = 0;
        for (const auto& visit : visits) {
            if (visit.load() > 1) {
                ++duplicates;
            } else if (visit.load() == 0) {
                ++missed;
            }
        }
        GC_EXPECT_EQ(duplicates, 0u);
        GC_EXPECT_EQ(missed, 0u);
    }
};

} // namespace MapleRuntime

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_COMPONENT_TEST(ZIndexDistributorTest, test_claim_tree_claim_level_size)
{
    ZIndexDistributorTest::test_claim_tree_claim_level_size();
}

GC_COMPONENT_TEST(ZIndexDistributorTest, test_claim_tree_claim_level_end_index)
{
    ZIndexDistributorTest::test_claim_tree_claim_level_end_index();
}

GC_COMPONENT_TEST(ZIndexDistributorTest, test_claim_tree_claim_level_index)
{
    ZIndexDistributorTest::test_claim_tree_claim_level_index();
}

GC_COMPONENT_TEST(ZIndexDistributorTest, test_claim_tree_claim_index)
{
    ZIndexDistributorTest::test_claim_tree_claim_index();
}

GC_COMPONENT_TEST(ZIndexDistributorTest, test_claim_tree_claim_is_atomic)
{
    ZIndexDistributorTest::test_claim_tree_claim_is_atomic();
}

GC_COMPONENT_TEST(ZIndexDistributorTest, test_claim_tree_concurrent_claims_are_accounted)
{
    ZIndexDistributorTest::test_claim_tree_concurrent_claims_are_accounted();
}

// Serial arm: one worker claims and then steals the whole tree.
GC_COMPONENT_TEST(ZIndexDistributorTest, claim_tree_serial_distributes_each_index_once)
{
    ZIndexDistributorTest::distribute_once<ZIndexDistributorClaimTree>(4096, 1);
    ZIndexDistributorTest::distribute_once<ZIndexDistributorClaimTree>(5000, 1);
}

// Parallel arm: concurrent claimers never duplicate or miss an index.
GC_COMPONENT_TEST(ZIndexDistributorTest, claim_tree_parallel_distributes_each_index_once)
{
    ZIndexDistributorTest::distribute_once<ZIndexDistributorClaimTree>(1 << 16, 8);
}

GC_COMPONENT_TEST(ZIndexDistributorTest, striped_distributes_each_index_once)
{
    ZIndexDistributorTest::distribute_once<ZIndexDistributorStriped>(4096, 1);
    ZIndexDistributorTest::distribute_once<ZIndexDistributorStriped>(1 << 16, 8);
}

// The shell (zIndexDistributor.hpp:29-48) dispatches on ZIndexDistributorStrategy.
GC_COMPONENT_TEST(ZIndexDistributorTest, shell_distributes_each_index_once)
{
    ZIndexDistributorTest::distribute_once<ZIndexDistributor>(1 << 14, 8);
}

// Product wiring: ZPageTableParallelIterator (zPageTable.hpp:73) holds a
// ZIndexDistributor and emits each page once, from its start granule only.
GC_COMPONENT_TEST(ZIndexDistributorTest, page_table_parallel_iterator_emits_each_page_once)
{
    struct Page {
        MAddress offset;
        size_t id;
        zoffset start_offset() const { return static_cast<zoffset>(offset); }
        zoffset start() const { return start_offset(); }
    };
    constexpr size_t domain = 4096;
    constexpr size_t granule = ZGranuleSize;
    constexpr MAddress base = 0;
    ZGranuleMap<Page*> table(domain * granule);
    Page pages[] = {{base, 0}, {base + (domain / 2) * granule, 1}, {base + (domain - 1) * granule, 2}};
    table.put(static_cast<zoffset>(0), 3 * granule, &pages[0]);
    table.put(static_cast<zoffset>((domain / 2) * granule), 2 * granule, &pages[1]);
    table.put(static_cast<zoffset>((domain - 1) * granule), granule, &pages[2]);
    std::vector<size_t> expected(3);
    Page* last = nullptr;
    for (size_t i = 0; i < table.size(); ++i) {
        Page* page = table.at(i);
        if (page != nullptr && page != last) {
            ++expected[page->id];
            last = page;
        }
    }
    std::vector<std::atomic<size_t>> actual(expected.size());
    for (auto& count : actual) {
        count.store(0);
    }
    ZPageTableParallelIterator<Page*> iterator(table);
    std::vector<std::thread> threads;
    for (size_t worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&]() {
            iterator.do_pages([&](Page* page) {
                actual[page->id].fetch_add(1, std::memory_order_relaxed);
                return true;
            });
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        GC_EXPECT_EQ(expected[i], 1u);
        GC_EXPECT_EQ(actual[i].load(), expected[i]);
    }
}
