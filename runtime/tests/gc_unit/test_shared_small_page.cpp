// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// ZObjectAllocator::PerAge::alloc_object_in_shared_page / retire_pages and
// ZPage::alloc_object_atomic. OpenJDK has no dedicated object-allocator or
// ZPerCPU gtest; these deterministic cases cover those product invariants,
// with age enumeration corresponding to test_zPageAge.cpp.
#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "zunittest.hpp"
#include "Heap/z/zCPU.inline.hpp"
#include "Heap/z/zObjectAllocator.hpp"
#include "Heap/z/zStat.hpp"
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_OTHER_VM_TEST(SharedSmallPage, AtomicBoundsPreserveTop)
{
    GcHeapFixture fixture;
    RegionInfo* page = fixture.region0;
    const uintptr_t start = page->GetRegionStart();
    page->SetRegionAllocPtr(start);
    const size_t capacity = page->GetRegionSize();
    GC_EXPECT_EQ(page->AtomicAlloc(capacity - 16), start);
    const uintptr_t top = page->GetRegionAllocPtr();
    GC_EXPECT_EQ(page->AtomicAlloc(32), uintptr_t{0});
    GC_EXPECT_EQ(page->GetRegionAllocPtr(), top);
    GC_EXPECT_EQ(page->AtomicAlloc(std::numeric_limits<size_t>::max()), uintptr_t{0});
    GC_EXPECT_EQ(page->GetRegionAllocPtr(), top);
    GC_EXPECT_EQ(page->AtomicAlloc(16), top);
    GC_EXPECT_EQ(page->GetRegionAllocPtr(), page->GetRegionEnd());
}

GC_OTHER_VM_TEST(SharedSmallPage, AtomicReservationsDoNotOverlap)
{
    GcHeapFixture fixture;
    RegionInfo* page = fixture.region0;
    page->SetRegionAllocPtr(page->GetRegionStart());
    constexpr size_t threads = 4;
    constexpr size_t perThread = 8;
    constexpr size_t bytes = 16;
    uintptr_t addresses[threads][perThread]{};
    std::atomic<size_t> ready{0};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            ready.fetch_add(1);
            while (ready.load() != threads) { std::this_thread::yield(); }
            for (size_t index = 0; index < perThread; ++index) {
                addresses[worker][index] = page->AtomicAlloc(bytes);
            }
        });
    }
    for (auto& worker : workers) { worker.join(); }
    std::vector<uintptr_t> sorted;
    for (const auto& arm : addresses) {
        sorted.insert(sorted.end(), std::begin(arm), std::end(arm));
    }
    std::sort(sorted.begin(), sorted.end());
    for (size_t index = 0; index < sorted.size(); ++index) {
        GC_EXPECT_EQ(sorted[index], page->GetRegionStart() + index * bytes);
    }
}

#if defined(__linux__)
namespace {
// A synthetic, single-caller heap, using the product page allocator and page
// table. There are no registered mutators; retire_pages has a quiescent world.
struct SharedPageFixture {
    // The RegionManager (mapped caches keep entries in heap memory) must be
    // destroyed before the mapping: declare it last.
    std::unique_ptr<ZTestRegionHeap> heap;
    RegionManager manager;
    SharedPageFixture()
    {
        // Match CollectorResources::Init before allocation-rate sampling.
        ZStat::Initialize();
        constexpr size_t units = 64;
        HeapParam params{};
        params.regionSize = RegionInfo::UNIT_SIZE / KB;
        params.exemptionThreshold = 0.8;
        heap.reset(new ZTestRegionHeap(units, manager, params, 0.5));
    }
};

class CPUAffinity {
public:
    CPUAffinity() : count(static_cast<size_t>(sysconf(_SC_NPROCESSORS_CONF))),
                    bytes(CPU_ALLOC_SIZE(count)), saved(CPU_ALLOC(count)), selected(CPU_ALLOC(count))
    {
        GC_EXPECT_TRUE(saved != nullptr && selected != nullptr);
        GC_EXPECT_EQ(sched_getaffinity(0, bytes, saved), 0);
        for (size_t cpu = 0; cpu < count; ++cpu) {
            if (CPU_ISSET_S(cpu, bytes, saved)) { available.push_back(cpu); }
        }
        GC_EXPECT_TRUE(!available.empty());
        Select(available.front());
    }
    ~CPUAffinity()
    {
        (void)sched_setaffinity(0, bytes, saved);
        CPU_FREE(selected);
        CPU_FREE(saved);
    }
    void Select(size_t cpu)
    {
        CPU_ZERO_S(bytes, selected);
        CPU_SET_S(cpu, bytes, selected);
        GC_EXPECT_EQ(sched_setaffinity(0, bytes, selected), 0);
    }
    std::vector<size_t> available;
private:
    size_t count;
    size_t bytes;
    cpu_set_t* saved;
    cpu_set_t* selected;
};
}

GC_OTHER_VM_TEST(SharedSmallPage, AgeRefillAndRetirement)
{
    CPUAffinity affinity;
    SharedPageFixture fixture;
    auto& manager = fixture.manager;
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    RegionInfo* pages[kPageAgeCount]{};
    for (PageAge age : kPageAgeRangeAll) {
        const uintptr_t address = manager.AllocSharedObject(16, age, true);
        GC_EXPECT_TRUE(address != 0);
        RegionInfo* page = RegionInfo::GetRegionInfoAt(address);
        pages[untype(age)] = page;
        GC_EXPECT_EQ(page->BirthSequence(), page->GetSnapshotEpoch());
        GC_EXPECT_TRUE(page->IsAllocating());
        const auto other = age == PageAge::old ? GCCycleGeneration::YOUNG : GCCycleGeneration::OLD;
        GC_EXPECT_EQ(page->OtherSequence(), Heap::GetHeap().GetCollector().GetCycleSnapshot(other).sequence);
        GC_EXPECT_EQ(page->IsYoungRegion(), age != PageAge::old);
        GC_EXPECT_EQ(page->GetYoungAge(), age == PageAge::old ? uint8_t{0} : static_cast<uint8_t>(untype(age)));
        GC_EXPECT_TRUE(!page->IsThreadLocalRegion());
        GC_EXPECT_EQ(manager.AllocSharedObject(16, age, true), address + 16);
        for (uint32_t previous = 0; previous < untype(age); ++previous) {
            GC_EXPECT_TRUE(pages[previous] != page);
        }
    }
    RegionInfo* eden = pages[untype(PageAge::eden)];
    const size_t remaining = eden->GetRegionSize() - 32;
    GC_EXPECT_EQ(manager.AllocSharedObject(remaining, PageAge::eden, true), eden->GetRegionStart() + 32);
    const uintptr_t refilled = manager.AllocSharedObject(16, PageAge::eden, true);
    GC_EXPECT_TRUE(refilled != 0);
    GC_EXPECT_TRUE(RegionInfo::GetRegionInfoAt(refilled) != eden);
    manager.RetireSharedPages(kPageAgeRangeYoung);
    GcHeapFixture::AdvanceGeneration(Generation::Young);
    const uintptr_t retired = manager.AllocSharedObject(16, PageAge::eden, true);
    GC_EXPECT_TRUE(retired != 0);
    auto* retiredPage = RegionInfo::GetRegionInfoAt(retired);
    std::fprintf(stderr, "P1_SHARED_BIRTH_ASSERT generation=young birth=%llu owner=%llu\n",
        static_cast<unsigned long long>(retiredPage->BirthSequence()),
        static_cast<unsigned long long>(retiredPage->GetSnapshotEpoch()));
    GC_EXPECT_TRUE(retiredPage->IsAllocating());
    GC_EXPECT_TRUE(RegionInfo::GetRegionInfoAt(retired) != RegionInfo::GetRegionInfoAt(refilled));
    RegionInfo* old = pages[untype(PageAge::old)];
    GC_EXPECT_EQ(manager.AllocSharedObject(16, PageAge::old, true), old->GetRegionStart() + 32);
    GC_EXPECT_TRUE(old->IsAllocating());
    manager.RetireSharedPages(kPageAgeRangeOld);
    GcHeapFixture::AdvanceGeneration(Generation::Old);
    const uintptr_t newOld = manager.AllocSharedObject(16, PageAge::old, true);
    GC_EXPECT_TRUE(newOld != 0);
    auto* newOldPage = RegionInfo::GetRegionInfoAt(newOld);
    std::fprintf(stderr, "P1_SHARED_BIRTH_ASSERT generation=old birth=%llu owner=%llu\n",
        static_cast<unsigned long long>(newOldPage->BirthSequence()),
        static_cast<unsigned long long>(newOldPage->GetSnapshotEpoch()));
    GC_EXPECT_TRUE(newOldPage->IsAllocating());
    GC_EXPECT_TRUE(old->IsRelocatable());
    GC_EXPECT_TRUE(RegionInfo::GetRegionInfoAt(newOld) != old);
}

// ZHeap::account_alloc_page / is_small_eden_page (zHeap.cpp:229-237).
GC_OTHER_VM_TEST(SharedSmallPage, TLABAccountingOnlySmallEden)
{
    CPUAffinity affinity;
    SharedPageFixture fixture;
    auto& manager = fixture.manager;
    size_t expected = 0;
    for (PageAge age : kPageAgeRangeAll) {
        const uintptr_t address = manager.AllocSharedObject(16, age, true);
        GC_EXPECT_TRUE(address != 0);
        if (age == PageAge::eden) {
            expected += RegionInfo::GetRegionInfoAt(address)->GetRegionSize();
        }
    }
    const uintptr_t large = manager.AllocSharedObject(manager.GetLargeObjectThreshold() + 16,
                                                      PageAge::eden, true);
    GC_EXPECT_TRUE(large != 0);
    GC_EXPECT_TRUE(!RegionInfo::GetRegionInfoAt(large)->IsSmallRegion());
    GC_EXPECT_TRUE(expected != 0);
    manager.ResetTLABUsage();
    GC_EXPECT_EQ(manager.GetTLABUsed(), expected);
}

// zCPU.inline.hpp:36-46 / zCPU.cpp:54-66: ZCPU::id() caches the CPU per thread
// and revalidates through the affinity table; the cached id is only replaced
// once another thread has claimed that CPU's entry (the slow path). The shared
// small page follows ZCPU::id() (zObjectAllocator.cpp:48-50).
GC_OTHER_VM_TEST(SharedSmallPage, MigrationUsesCurrentCPU)
{
    CPUAffinity affinity;
    SharedPageFixture fixture;
    auto& manager = fixture.manager;
    if (affinity.available.size() < 2) {
        std::fprintf(stderr, "SharedSmallPage: migration arm unavailable: one allowed CPU\n");
        return;
    }
    const size_t cpuA = affinity.available.front();
    const size_t cpuB = affinity.available.back();
    affinity.Select(cpuA);
    // Fresh thread state: the first id() takes the slow path and reads cpuA.
    GC_EXPECT_EQ(ZCPU::id(), cpuA);
    const uintptr_t first = manager.AllocSharedObject(16, PageAge::eden, true);
    GC_EXPECT_TRUE(first != 0);
    GC_EXPECT_TRUE(manager.objectAllocators[untype(PageAge::eden)]->sharedSmallPage.get(static_cast<uint32_t>(cpuA)) ==
                   RegionInfo::GetRegionInfoAt(first));

    affinity.Select(cpuB);
    // Fast path: the affinity entry for cpuA still names this thread.
    GC_EXPECT_EQ(ZCPU::id(), cpuA);
    GC_EXPECT_EQ(manager.AllocSharedObject(16, PageAge::eden, true), first + 16);

    // Another thread pinned to cpuA claims cpuA's entry (zCPU.cpp:62-64) ...
    std::thread claimer([&] {
        cpu_set_t* set = CPU_ALLOC(affinity.available.back() + 1);
        const size_t bytes = CPU_ALLOC_SIZE(affinity.available.back() + 1);
        CPU_ZERO_S(bytes, set);
        CPU_SET_S(cpuA, bytes, set);
        (void)sched_setaffinity(0, bytes, set);
        CPU_FREE(set);
        (void)ZCPU::id();
    });
    claimer.join();
    // ... so this thread's next id() falls to the slow path and reads cpuB.
    GC_EXPECT_EQ(ZCPU::id(), cpuB);
    const uintptr_t second = manager.AllocSharedObject(16, PageAge::eden, true);
    GC_EXPECT_TRUE(second != 0);
    GC_EXPECT_TRUE(RegionInfo::GetRegionInfoAt(first) != RegionInfo::GetRegionInfoAt(second));
    GC_EXPECT_TRUE(manager.objectAllocators[untype(PageAge::eden)]->sharedSmallPage.get(static_cast<uint32_t>(cpuB)) ==
                   RegionInfo::GetRegionInfoAt(second));
}
#endif
