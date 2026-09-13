// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/Allocator/ZGranuleMap.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

namespace MapleRuntime {

// Port of gc/z/test_zIndexDistributor.cpp's four geometry tests. As in
// ZGC, this friend exercises the product class, not a copy of its formulas.
class ZIndexDistributorTest {
    static void require(bool condition)
    {
        if (!condition) {
            std::abort();
        }
    }

public:
    static void test_claim_tree_claim_level_size()
    {
        using D = ZIndexDistributorClaimTree;
        require(D::claim_level_size(0) == 1);
        require(D::claim_level_size(1) == 16);
        require(D::claim_level_size(2) == 16 * 16);
        require(D::claim_level_size(3) == 16 * 16 * 16);
    }

    static void test_claim_tree_claim_level_end_index()
    {
        using D = ZIndexDistributorClaimTree;
        const size_t first = 64 / sizeof(std::atomic<size_t>);
        require(D::claim_level_end_index(0) == first);
        require(D::claim_level_end_index(1) == first + 16);
        require(D::claim_level_end_index(2) == first + 16 + 16 * 16);
        require(D::claim_level_end_index(3) == first + 16 + 16 * 16 + 16 * 16 * 16);
    }

    static void test_claim_tree_claim_level_index()
    {
        using D = ZIndexDistributorClaimTree;
        for (size_t a : {size_t(0), size_t(1), size_t(2), size_t(15)}) {
            for (size_t b : {size_t(0), size_t(1), size_t(2), size_t(14)}) {
                for (size_t c : {size_t(0), size_t(1), size_t(3), size_t(15)}) {
                    const size_t indices[] = {a, b, c, 0};
                    require(D::claim_level_index(indices, 1) == a);
                    require(D::claim_level_index(indices, 2) == a * 16 + b);
                    require(D::claim_level_index(indices, 3) == a * 256 + b * 16 + c);
                }
            }
        }
    }

    static void test_claim_tree_claim_index()
    {
        using D = ZIndexDistributorClaimTree;
        const size_t first = 64 / sizeof(std::atomic<size_t>);
        for (size_t a : {size_t(0), size_t(1), size_t(15), size_t(16)}) {
            const size_t indices[] = {a, 0, 0, 0};
            require(D::claim_index(indices, 0) == 0);
        }
        for (size_t a : {size_t(0), size_t(1), size_t(15)}) {
            for (size_t b : {size_t(0), size_t(2), size_t(14)}) {
                const size_t indices[] = {a, b, 0, 0};
                require(D::claim_index(indices, 1) == first + a);
                require(D::claim_index(indices, 2) == first + 16 + a * 16 + b);
            }
        }
    }

    static void parallel_indices(size_t requested, size_t workers)
    {
        const size_t count = ZIndexDistributorClaimTree::get_count(requested);
        require(count >= requested);
        ZIndexDistributorClaimTree distributor(count);
        std::vector<std::atomic<size_t>> visits(count);
        for (auto& visit : visits) {
            visit.store(0);
        }
        std::vector<std::thread> threads;
        for (size_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&]() {
                distributor.do_indices([&](size_t index) {
                    require(index < count);
                    visits[index].fetch_add(1, std::memory_order_relaxed);
                    return true;
                });
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        for (const auto& visit : visits) {
            require(visit.load() == 1);
        }
    }

    static void sparse_pages(size_t domain, size_t workers)
    {
        struct Page {
            MAddress start;
            size_t id;
            MAddress GetRegionStart() const { return start; }
        };
        constexpr size_t granule = 4096;
        constexpr MAddress base = 0x40000000;
        ZGranuleMap<Page*> table;
        require(table.Initialize(base, domain * granule, granule));
        Page pages[] = {{base, 0}, {base + (domain / 2) * granule, 1},
                        {base + (domain - 1) * granule, 2}};
        table.put(static_cast<zoffset>(0), 3 * granule, &pages[0]);
        table.put(static_cast<zoffset>((domain / 2) * granule), 2 * granule, &pages[1]);
        table.put(static_cast<zoffset>((domain - 1) * granule), granule, &pages[2]);
        std::vector<size_t> expected(3);
        table.visit_unique([&](Page* page) { ++expected[page->id]; });
        std::vector<std::atomic<size_t>> actual(expected.size());
        for (auto& count : actual) {
            count.store(0);
        }
        ZPageTableParallelIterator<Page*> iterator(table);
        std::vector<std::thread> threads;
        for (size_t worker = 0; worker < workers; ++worker) {
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
            require(actual[i].load() == expected[i]);
        }
    }
};
} // namespace MapleRuntime

int main()
{
    using namespace MapleRuntime;
    ZIndexDistributorTest::test_claim_tree_claim_level_size();
    ZIndexDistributorTest::test_claim_tree_claim_level_end_index();
    ZIndexDistributorTest::test_claim_tree_claim_level_index();
    ZIndexDistributorTest::test_claim_tree_claim_index();
    for (size_t workers : {size_t(1), size_t(4)}) {
        for (size_t domain : {size_t(17), size_t(4096), size_t(8193)}) {
            ZIndexDistributorTest::parallel_indices(domain, workers);
            ZIndexDistributorTest::sparse_pages(domain, workers);
        }
    }
    std::puts("z_index_distributor: geometry, unique indices and serial/parallel page sets agree");
}
