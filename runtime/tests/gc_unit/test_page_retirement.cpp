// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Lifecycle extension of ZVirtualMemoryManagerTest::test_remove_from_low:
// zPageTable.cpp:101-113 and zArray.inline.hpp:210-246 require the last
// iterator to consume deferred destruction. There is no standalone upstream
// ZSafeDelete gtest; these cases exercise that protocol via RegionManager
// over the product memory managers.

#include "gc_unittest.hpp"
#include "zunittest.hpp"

#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zStat.hpp"
#include "Mutator/ThreadLocal.h"

namespace MapleRuntime {
namespace {

enum class RetirementPath { RETURN, RECLAIM, RELEASE, MARK_QUARANTINE };

int ExercisePageRetirement(RetirementPath path, bool concurrent)
{
    // This is a native fixture, not a scheduler-managed mutator thread.
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    const size_t unit = ZPage::UNIT_SIZE;
    int result = 0;
    {
        // Destroyed in reverse order: the manager (mapped caches keep entries
        // in heap memory) goes before the mapping.
        HeapParam heapParam{};
        heapParam.regionSize = 64;
        heapParam.exemptionThreshold = 0.8;
        std::unique_ptr<ZTestRegionHeap> heapHolder;
        RegionManager manager;
        heapHolder.reset(new ZTestRegionHeap(4, manager, heapParam, 0.5));
        ZTestRegionHeap& heap = *heapHolder;
        (void)heap;
        // ReleaseRetiredRegion clears the product remembered set before
        // returning the page. Its address space must exist as after heap init.
        const auto role = ZPageType::small;
        BindFixturePageTable(manager, 4);
        ZPage* first = manager.TakeRegion(2, role, false, false, false);
        ZPage* second = manager.TakeRegion(2, role, false, false, false);
        if (first == nullptr || second == nullptr) {
            return 21;
        }
        PublishAllocatedPage(first);
        PublishAllocatedPage(second);
        const uintptr_t start = first->GetRegionStart();
        const uintptr_t end = first->GetRegionEnd();
        const auto life = first->GetRegionLifeId();
        const auto index = first->GetUnitIdx();
        const auto type = first->OnNamedList("from regions");
        const size_t capacity = manager.GetCommittedCapacity();
        size_t retired = 0;
        auto retire = [&] {
            ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
            switch (path) {
                case RetirementPath::RETURN:
                    manager.ReturnPageMemory({ index, 2, 0, true });
                    break;
                case RetirementPath::RECLAIM:
                    manager.ReclaimRegion(first);
                    break;
                case RetirementPath::RELEASE:
                    if (manager.ReleaseRegion(first) != 2 * unit) {
                        result = 22;
                    }
                    break;
                case RetirementPath::MARK_QUARANTINE:
                    manager.ReclaimRegionToMarkQuarantine(first);
                    break;
            }
            ++retired;
        };
        auto inspectRetiredPage = [&] {
            // Every granule is withdrawn, including the last byte of a
            // multi-unit page. The descriptor still describes its old life.
            for (uintptr_t address = start; address < end; address += unit) {
                if (Heap::page(address) != nullptr ||
                    Heap::page(address + unit - 1) != nullptr) {
                    result = 23;
                }
            }
            if (first->GetRegionEnd() != end || first->GetRegionLifeId() != life ||
                first->OnNamedList("from regions") != type || first->IsFreeRegion()) {
                result = 24;
            }
            // Memory stays out of the cache and committed while an iterator
            // can still read the descriptor (ZGC free_page only after the
            // page table iteration ends).
            if (manager.GetDirtyUnitCount() != 0 || manager.GetCommittedCapacity() != capacity) {
                result = 25;
            }
            // All capacity is owned; a retired page is not available for
            // cache allocation while either iterator can still read it.
            if (manager.TakeRegion(1, role, false, false, false) != nullptr) {
                result = 26;
            }
            if (Heap::page(second->GetRegionStart()) != second) {
                result = 27;
            }
        };
        manager.VisitPageOwners([&](ZPage* outer) {
            if (outer != first) {
                return;
            }
            manager.VisitPageOwners([&](ZPage* inner) {
                if (inner != first) {
                    return;
                }
                if (concurrent) {
                    std::thread reclaimer(retire);
                    reclaimer.join();
                } else {
                    retire();
                }
                inspectRetiredPage();
            });
            // Ending the nested iterator must not drain the queue while
            // this outer callback still holds the original descriptor.
            inspectRetiredPage();
        });
        if (retired != 1 || !first->IsFreeRegion() || first->GetRegionLifeId() == life) {
            result = 28;
        }
        if (path == RetirementPath::MARK_QUARANTINE) {
            manager.ReleaseMarkQuarantine();
        }
        // Every path hands the page's memory back to the mapped cache (ZGC
        // free_page); capacity is unchanged, only ZUncommitter uncommits.
        if (manager.GetDirtyUnitCount() != 2 || manager.GetCommittedCapacity() != capacity) {
            result = 30;
        }
        ZPage* reused = manager.TakeRegion(2, role, false, false, false);
        PublishAllocatedPage(reused);
        if (reused == nullptr || reused->GetRegionStart() != start ||
            Heap::page(end - 1) != reused) {
            result = 31;
        }
        Heap::bind_test_page_allocator(nullptr);
    }
    return result;
}

void CheckPageRetirement(RetirementPath path, bool concurrent)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        _exit(ExercisePageRetirement(path, concurrent));
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

} // namespace

GC_TEST(PageRetirement, PageTableReturnWaitsForOutermostIterator)
{
    CheckPageRetirement(RetirementPath::RETURN, false);
}

GC_TEST(PageRetirement, PageTableConcurrentReclaimPreservesDescriptor)
{
    CheckPageRetirement(RetirementPath::RECLAIM, true);
}

GC_TEST(PageRetirement, PageTableReleaseWaitsForIterator)
{
    CheckPageRetirement(RetirementPath::RELEASE, true);
}

GC_TEST(PageRetirement, PageTableMarkQuarantineWaitsForIterator)
{
    CheckPageRetirement(RetirementPath::MARK_QUARANTINE, false);
}

} // namespace MapleRuntime
