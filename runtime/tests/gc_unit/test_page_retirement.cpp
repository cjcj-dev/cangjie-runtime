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

// zPageAllocator.cpp:1401-1407,1467-1478: allocation consumes allocator
// capacity; only the owner returning a page makes that page available.
void CheckAllocationPreservesOwnedPage(bool allowSaferegion, bool nonBlocking)
{
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    RegionManager& manager = Heap::GetHeap().page_allocator();
    ZPage* owned = Heap::alloc_page(ZGranuleSize, ZPageType::large, false, false, false);
    GC_EXPECT_TRUE(owned != nullptr);
    const uintptr_t address = owned->GetRegionStart();
    // Model a page awaiting GC reclamation. Allocation must not claim it
    // merely because its role is Garbage; the GC owner still owns the page.
    owned->SetRegionRole(ZPageRole::Garbage);
    const size_t used = manager.GetAllocatedSize();
    ZAllocationFlags flags;
    if (nonBlocking) {
        flags.set_non_blocking();
    }
    ZPage* allocated = Heap::alloc_page(ZGranuleSize, ZPageType::large, false,
                                      allowSaferegion, false, PageAge::eden, flags);
    ZPage* current = Heap::page(address);
    const bool retained = current == owned && current->IsGarbageRegion();
    const size_t usedAfter = manager.GetAllocatedSize();
    std::printf("AllocationOwnership retained=%d used_before=%zu used_after=%zu\n",
                retained, used, usedAfter);
    // This is the target assertion, before checks of the new allocation.
    GC_EXPECT_TRUE(retained);
    GC_EXPECT_EQ(usedAfter, used + ZGranuleSize);
    GC_EXPECT_TRUE(allocated != nullptr);
    GC_EXPECT_NE(allocated->GetRegionStart(), address);
    Heap::free_page(allocated);
    Heap::free_page(owned);
}

enum class RetirementPath { RETURN, RECLAIM, RELEASE, MARK_QUARANTINE };

int ExercisePageRetirement(RetirementPath path, bool concurrent)
{
    // This is a native fixture, not a scheduler-managed mutator thread.
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    const size_t unit = ZGranuleSize;
    int result = 0;
    {
        // Destroyed in reverse order: the manager (mapped caches keep entries
        // in heap memory) goes before the mapping.
        MapleRuntime::GcUnit::CreateStandaloneHeap(4);
        RegionManager& manager = Heap::GetHeap().page_allocator();
        // ReleaseRetiredRegion clears the product remembered set before
        // returning the page. Its address space must exist as after heap init.
        const auto role = ZPageType::large;

        ZPage* first = manager.TakeRegion((2) * ZGranuleSize, role, false, false, false);
        ZPage* second = manager.TakeRegion((2) * ZGranuleSize, role, false, false, false);
        if (first == nullptr || second == nullptr) {
            return 21;
        }
        PublishAllocatedPage(first);
        PublishAllocatedPage(second);
        const uintptr_t start = first->GetRegionStart();
        const uintptr_t end = first->GetRegionEnd();
        const auto life = first->GetRegionLifeId();
        const auto index = first->granule_index();
        const ZPageRole roleBefore = first->GetRegionRole();
        const size_t capacity = manager.GetCommittedCapacity();
        size_t retired = 0;
        auto retire = [&] {
            ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
            switch (path) {
                case RetirementPath::RETURN:
                    manager.ReturnPageMemory({ index, 2 * ZGranuleSize, 0, true });
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
            if (first->GetRegionEnd() != end || first->GetRegionLifeId() != life) {
                result = 24;
            }
            (void)roleBefore;
            // Memory stays out of the cache and committed while an iterator
            // can still read the descriptor (ZGC free_page only after the
            // page table iteration ends).
            if ((manager.GetCachedBytes() / ZGranuleSize) != 0 || manager.GetCommittedCapacity() != capacity) {
                result = 25;
            }
            // All capacity is owned; a retired page is not available for
            // cache allocation while either iterator can still read it.
            if (manager.TakeRegion((1) * ZGranuleSize, role, false, false, false) != nullptr) {
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
        if ((manager.GetCachedBytes() / ZGranuleSize) != 2 || manager.GetCommittedCapacity() != capacity) {
            result = 30;
        }
        ZPage* reused = manager.TakeRegion((2) * ZGranuleSize, role, false, false, false);
        PublishAllocatedPage(reused);
        if (reused == nullptr || reused->GetRegionStart() != start ||
            Heap::page(end - 1) != reused) {
            result = 31;
        }

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

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, SaferegionAllocationPreservesOwnedPage)
{
    CheckAllocationPreservesOwnedPage(true, false);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, NonSaferegionAllocationPreservesOwnedPage)
{
    CheckAllocationPreservesOwnedPage(false, false);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, NonBlockingAllocationPreservesOwnedPage)
{
    CheckAllocationPreservesOwnedPage(true, true);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, ExplicitFreeWithdrawsOwnedPage)
{
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    RegionManager& manager = Heap::GetHeap().page_allocator();
    const size_t used = manager.GetAllocatedSize();
    ZPage* owned = Heap::alloc_page(ZGranuleSize, ZPageType::large, false, false, false);
    GC_EXPECT_TRUE(owned != nullptr);
    const uintptr_t address = owned->GetRegionStart();
    owned->SetRegionRole(ZPageRole::Garbage);
    Heap::free_page(owned);
    const bool withdrawn = Heap::page(address) == nullptr;
    std::printf("AllocationOwnership explicit_free_withdrawn=%d used=%zu\n",
                withdrawn, manager.GetAllocatedSize());
    GC_EXPECT_TRUE(withdrawn);
    GC_EXPECT_EQ(manager.GetAllocatedSize(), used);
}

GC_COMPONENT_OTHER_VM_TEST(PageRetirement, PageTableReturnWaitsForOutermostIterator)
{
    CheckPageRetirement(RetirementPath::RETURN, false);
}

GC_COMPONENT_OTHER_VM_TEST(PageRetirement, PageTableConcurrentReclaimPreservesDescriptor)
{
    CheckPageRetirement(RetirementPath::RECLAIM, true);
}

GC_COMPONENT_OTHER_VM_TEST(PageRetirement, PageTableReleaseWaitsForIterator)
{
    CheckPageRetirement(RetirementPath::RELEASE, true);
}

GC_COMPONENT_OTHER_VM_TEST(PageRetirement, PageTableMarkQuarantineWaitsForIterator)
{
    CheckPageRetirement(RetirementPath::MARK_QUARANTINE, false);
}

} // namespace MapleRuntime
