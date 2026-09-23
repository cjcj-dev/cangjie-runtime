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
#include "Mutator/Mutator.inline.h"
#include "Cangjie.h"
#include "Common/Handle.h"
#include "Heap/z/zRootsIterator.hpp"
#include "TypeInfoManager.h"
#include "ObjectModel/MObject.h"
#include "Heap/z/zPageTable.inline.hpp"

namespace MapleRuntime {
namespace {

size_t CountGarbagePages()
{
    size_t count = 0;
    ZPageTableIterator iterator(&Heap::page_table());
    for (ZPage* page; iterator.next(&page);) {
        count += page->IsGarbageRegion();
    }
    return count;
}

// zPageAllocator.cpp:1401-1407,1467-1478: allocation consumes allocator
// capacity; only the owner returning a page makes that page available.
void CheckAllocationPreservesOwnedPage(bool allowSaferegion, bool nonBlocking)
{
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    MapleRuntime::GcUnit::CreateStandaloneHeap(16);
    ZStat::Initialize();
    RegionManager& manager = Heap::GetHeap().page_allocator();
    ZPage* owned = Heap::alloc_page(ZGranuleSize, ZPageType::large, false, false, false);
    GC_EXPECT_TRUE(owned != nullptr);
    const uintptr_t address = owned->GetRegionStart();
    // Model a page awaiting GC reclamation. Allocation must not claim it
    // merely because its role is Garbage; the GC owner still owns the page.
    owned->SetRegionRole(ZPageRole::Garbage);
    const size_t garbageBefore = CountGarbagePages();
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
    GC_EXPECT_TRUE(garbageBefore != 0);
    GC_EXPECT_EQ(usedAfter, used + ZGranuleSize);
    GC_EXPECT_TRUE(allocated != nullptr);
    GC_EXPECT_NE(allocated->GetRegionStart(), address);
    Heap::free_page(allocated);
    Heap::free_page(owned);
}

// ZGC zGeneration.cpp:204-240: an empty page is registered and freed by
// the selector in the same collection, with no deferred Garbage-role scan.
struct EmptyPageCycles {
    size_t objectSize;
    bool promote;
    bool rootedPagePresent = false;
    bool expectedGeneration = false;
    bool rootsReleased = false;
    size_t ownedBytes = 0;
    size_t usedBefore = 0;
    size_t usedAfter[2]{};
    size_t garbage[2]{};
    bool withdrawn[2]{};
    uint32_t sequenceBefore = 0;
    uint32_t sequenceAfter = 0;
};

extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);

void* RunEmptyPageCycles(void* context)
{
    auto& result = *static_cast<EmptyPageCycles*>(context);
    auto& heap = Heap::GetHeap();
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(result.objectSize - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    const size_t rootsBefore = mutator->NativeFrameRootCount();
    uintptr_t address = 0;
    {
        HandleMark roots(*mutator);
        Handle root(mutator, MCC_NewObject(type, result.objectSize));
        if (result.promote) {
            heap.RequestGC(GC_REASON_USER, false);
        }
        address = reinterpret_cast<uintptr_t>(root());
        ZPage* page = Heap::page(address);
        result.rootedPagePresent = page != nullptr;
        result.ownedBytes = page == nullptr ? 0 : page->size();
        result.expectedGeneration = page != nullptr && page->generation_id() ==
            (result.promote ? ZGenerationId::old : ZGenerationId::young);
    }
    result.rootsReleased = mutator->NativeFrameRootCount() == rootsBefore;
    result.usedBefore = heap.page_allocator().GetAllocatedSize();
    const GCReason reason = result.promote ? GC_REASON_USER : GC_REASON_YOUNG;
    ZGeneration* generation = result.promote
        ? static_cast<ZGeneration*>(ZGeneration::old())
        : static_cast<ZGeneration*>(ZGeneration::young());
    result.sequenceBefore = generation->seqnum();
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        heap.RequestGC(reason, false);
        ZPage* observed = Heap::page(address);
        result.withdrawn[cycle] = observed == nullptr;
        if (observed != nullptr) {
            std::fprintf(stderr, "EMPTY_STATE_904 cycle=%u gen=%u seq=%u current=%u marked=%d allocating=%d role=%u\n",
                cycle, unsigned(observed->generation_id()), observed->seqnum(), observed->generation()->seqnum(),
                observed->is_marked(), observed->is_allocating(), unsigned(observed->GetRegionRole()));
        }
        result.usedAfter[cycle] = heap.page_allocator().GetAllocatedSize();
        result.garbage[cycle] = CountGarbagePages();
    }
    result.sequenceAfter = generation->seqnum();
    mutator->SetManagedContext(true);
    return nullptr;
}

void CheckEmptyPageCycles(size_t objectSize, bool promote)
{
    RuntimeParam params{};
    params.heapParam.heapSize = 512 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    EmptyPageCycles result{objectSize, promote};
    auto task = RunCJTask(RunEmptyPageCycles, &result);
    GC_EXPECT_TRUE(task != nullptr);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    // Positive ownership observation and collection progress are reported with
    // the target results; no fatal prerequisite hides either target assertion.
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        std::fprintf(stderr,
            "EMPTY_PAGE_904_TARGET old=%d size=%zu cycle=%u rooted=%d bytes=%zu "
            "withdrawn=%d garbage=%zu used_before=%zu used_after=%zu seq_before=%u seq_after=%u\n",
            promote, objectSize, cycle + 1, result.rootedPagePresent, result.ownedBytes,
            result.withdrawn[cycle], result.garbage[cycle], result.usedBefore,
            result.usedAfter[cycle], result.sequenceBefore, result.sequenceAfter);
    }
    GC_EXPECT_TRUE(result.withdrawn[0] && result.withdrawn[1]);
    GC_EXPECT_TRUE(result.usedAfter[0] + result.ownedBytes <= result.usedBefore &&
                   result.usedAfter[1] + result.ownedBytes <= result.usedBefore);
    GC_EXPECT_EQ(result.garbage[0] + result.garbage[1], size_t{0});
    GC_EXPECT_TRUE(result.rootedPagePresent && result.ownedBytes != 0);
    GC_EXPECT_TRUE(result.expectedGeneration && result.rootsReleased);
    GC_EXPECT_TRUE(result.sequenceAfter >= result.sequenceBefore + 2);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
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

        ZPage* first = manager.TakeRegion((2) * ZGranuleSize, role, false, false);
        ZPage* second = manager.TakeRegion((2) * ZGranuleSize, role, false, false);
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
            if (manager.TakeRegion((1) * ZGranuleSize, role, false, false) != nullptr) {
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
        // Every path hands the page's memory back to the mapped cache (ZGC
        // free_page); capacity is unchanged, only ZUncommitter uncommits.
        if ((manager.GetCachedBytes() / ZGranuleSize) != 2 || manager.GetCommittedCapacity() != capacity) {
            result = 30;
        }
        ZPage* reused = manager.TakeRegion((2) * ZGranuleSize, role, false, false);
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

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, YoungEmptyPageFreedAcrossTwoCycles)
{
    CheckEmptyPageCycles(ZGranuleSize, false);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, OldEmptyPageFreedAcrossTwoCycles)
{
    CheckEmptyPageCycles(ZGranuleSize, true);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, OldLargeEmptyPageFreedAcrossTwoCycles)
{
    CheckEmptyPageCycles(3 * ZGranuleSize, true);
}

GC_RUNTIME_OTHER_VM_TEST(AllocationOwnership904, ExplicitFreeWithdrawsOwnedPage)
{
    ThreadLocal::SetThreadType(ThreadType::GC_THREAD);
    MapleRuntime::GcUnit::CreateStandaloneHeap(16);
    ZStat::Initialize();
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


namespace {
void CheckMarkReclaim(bool freePage)
{
    ThreadLocal::SetThreadType(ThreadType::FP_THREAD);
    ZStat::Initialize();
    GcUnit::CreateStandaloneHeap(4);
    auto& heap = Heap::GetHeap();
    auto& manager = heap.page_allocator();
    // Fill capacity so the following allocation can only reuse returned memory.
    const size_t size = 2 * ZGranuleSize;
    ZPage* first = Heap::alloc_page(size, ZPageType::large, false, false);
    ZPage* occupied = Heap::alloc_page(size, ZPageType::large, false, false);
    GC_EXPECT_TRUE(first != nullptr && occupied != nullptr);
    const uintptr_t start = first->GetRegionStart();
    const size_t capacity = manager.GetCommittedCapacity();
    const size_t cachedBefore = manager.GetCachedBytes();
    const auto previousPhase = heap.old().Snapshot().phase;
    heap.old().set_phase(ZGenerationPhase::Mark);
    // ZGC zPageAllocator.cpp:692-699,2253-2266: returning memory has no
    // mark-epoch holding branch. Both existing product entry paths obey it.
    if (freePage) {
        Heap::free_page(first);
    } else {
        manager.ReclaimRegion(first);
    }
    const size_t cachedAfter = manager.GetCachedBytes();
    const bool withdrawn = Heap::page(start) == nullptr;
    ZPage* reused = Heap::alloc_page(size, ZPageType::large, false, false);
    const bool sameRange = reused != nullptr && reused->GetRegionStart() == start;
    const bool stillMark = heap.old().Snapshot().phase == ZGenerationPhase::Mark;
    const bool sameCapacity = manager.GetCommittedCapacity() == capacity;
    std::fprintf(stderr,
        "MARK_CACHE_TARGET path=%s before=%zu after=%zu size=%zu withdrawn=%d reused=%d mark=%d capacity_same=%d\n",
        freePage ? "free_page" : "ReclaimRegion", cachedBefore, cachedAfter, size,
        withdrawn, sameRange, stillMark, sameCapacity);
    heap.old().set_phase(previousPhase);
    GC_EXPECT_EQ(cachedAfter, cachedBefore + size);
    GC_EXPECT_TRUE(withdrawn && sameRange && stillMark && sameCapacity);
}
} // namespace

GC_COMPONENT_OTHER_VM_TEST(PageRetirement, MarkReclaimReturnsToMappedCache)
{
    CheckMarkReclaim(false);
}

GC_COMPONENT_OTHER_VM_TEST(PageRetirement, MarkFreePageReturnsToMappedCache)
{
    CheckMarkReclaim(true);
}

} // namespace MapleRuntime
