#include <vector>
#include <algorithm>
#include <future>
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "LoaderManager.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zPageTable.hpp"
#include "Heap/z/zPageType.hpp"
#include "gc_unittest.hpp"
#include "cj_sret16_fixture.hpp"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(ZPageType, EnumValues)
{
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::small), 0);
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::medium), 1);
    GC_EXPECT_EQ(static_cast<uint8_t>(ZPageType::large), 2);
}

GC_TEST(ZPage, AllocPagePublishedInTable)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0();
    GC_EXPECT_TRUE(page != nullptr);
    GC_EXPECT_TRUE(ZPageTable::heap_table().get(page->GetRegionStart()) == page);
    GC_EXPECT_TRUE(Heap::page(page->GetRegionStart()) == page);
    page->reset_seqnum();
    GC_EXPECT_TRUE(page->is_allocating());
    GC_EXPECT_TRUE(!page->is_relocatable());
}

GC_TEST(ZPage, ObjectAlignmentFollowsType)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0();
    GC_EXPECT_EQ(page->object_alignment(), size_t(1) << page->object_alignment_shift());
    if (page->is_small()) {
        GC_EXPECT_EQ(page->object_alignment_shift(), ZObjectAlignmentSmallShift);
    }
}

GC_TEST(ZPage, AllocObjectRespectsAlignment)
{
    GcHeapFixture fx;
    ZPage* page = fx.region0();
    page->reset_seqnum();
    const uintptr_t addr = page->alloc_object(16);
    GC_EXPECT_TRUE(addr != 0);
    GC_EXPECT_EQ(addr % page->object_alignment(), 0u);
    GC_EXPECT_TRUE(page->undo_alloc_object(addr, 16));
}

// Real mutator entry -> object/page allocators -> page table. Values are copied
// out by the mutator and asserted after joining, without constructing a page or
// feeding an intermediate geometry into the consumer.
#include "Cangjie.h"
#include "TypeInfoManager.h"
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include "Heap/z/zJNICritical.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zRelocationSet.inline.hpp"
#include "Heap/z/concurrentGCBreakpoints.hpp"
#include "Heap/z/zRootsIterator.hpp"
#include "Common/Handle.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/Mutator.inline.h"

namespace MapleRuntime {
extern "C" ObjRef MCC_NewObject(const TypeInfo* klass, MSize size);
extern "C" ArrayRef MCC_NewArray8(const TypeInfo* arrayInfo, MIndex nElems);
extern "C" void* MCC_AcquireRawData(ArrayRef array, bool* isCopy);
extern "C" void MCC_ReleaseRawData(ArrayRef array, void* rawPtr);
}
namespace {
struct GranuleAllocationResult {
    size_t requested[3]{};
    size_t expected[3]{};
    size_t actual[3]{};
    unsigned type[3]{};
    bool allocated[3]{};
    bool firstMapped[3]{};
    bool lastMapped[3]{};
    bool sameSmallPage{false};
    size_t tableSize{0};
};

struct JNICriticalBlockedEnterResult {
    std::atomic<uintptr_t> array{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> proceed{false};
    std::atomic<bool> entering{false};
    std::atomic<bool> acquired{false};
    std::atomic<bool> finish{false};
    std::atomic<bool> saferegionAfterAcquire{true};
    bool copied{true};
};

struct SignalOnExit {
    explicit SignalOnExit(std::atomic<bool>& value) : flag(value) {}
    ~SignalOnExit() { flag.store(true, std::memory_order_release); }
    std::atomic<bool>& flag;
};

void* AcquireWhileJNICriticalBlocked(void* context)
{
    auto& result = *static_cast<JNICriticalBlockedEnterResult*>(context);
    alignas(TypeInfo) static unsigned char byteStorage[sizeof(TypeInfo)]{};
    alignas(TypeInfo) static unsigned char arrayStorage[sizeof(TypeInfo)]{};
    auto* byteType = reinterpret_cast<TypeInfo*>(byteStorage);
    auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
    byteType->SetType(TypeKind::TYPE_KIND_UINT8);
    byteType->SetInstanceSize(1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(byteType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(byteStorage), sizeof(byteStorage));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(arrayStorage), sizeof(arrayStorage));
    auto* array = static_cast<MArray*>(MCC_NewArray8(arrayType, 64));
    result.array.store(reinterpret_cast<uintptr_t>(array), std::memory_order_release);
    result.ready.store(true, std::memory_order_release);
    while (!result.proceed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    result.entering.store(true, std::memory_order_release);
    void* raw = MCC_AcquireRawData(array, &result.copied);
    result.saferegionAfterAcquire.store(Mutator::GetMutator()->InSaferegion(), std::memory_order_release);
    result.acquired.store(true, std::memory_order_release);
    // After publishing the observed return state, allow the collector to
    // finish even in the cut arm. Otherwise teardown masks the STW assertion.
    Mutator::GetMutator()->EnterSaferegion(false);
    while (!result.finish.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    Mutator::GetMutator()->LeaveSaferegion();
    MCC_ReleaseRawData(array, raw);
    return nullptr;
}
void* AllocateGranulePages(void* context)
{
    // ZGC zObjectAllocator.cpp:136-148: construct the cold-medium branch
    // explicitly now that heap construction primes the mapped cache.
    const size_t cached = Heap::GetHeap().page_allocator().GetCachedBytes();
    ZPage* initialCache = cached == 0 ? nullptr : Heap::alloc_page(
        cached, ZPageType::large, PageAge::eden, NonBlockingAllocationFlags());
    GC_EXPECT_TRUE(cached == 0 || initialCache != nullptr);
    auto& result = *static_cast<GranuleAllocationResult*>(context);
    alignas(TypeInfo) static unsigned char types[3][sizeof(TypeInfo)];
    result.requested[0] = 32;
    result.requested[1] = ZObjectSizeLimitSmall + 16;
    result.requested[2] = ZObjectSizeLimitMedium + ZGranuleSize + 16;
    result.expected[0] = ZPageSizeSmall;
    // zObjectAllocator.cpp:144: a cold medium request uses the maximum page size.
    // Cache-only variable sizes are covered by FastMediumConsumesCachedActualSize.
    result.expected[1] = ZPageSizeMediumMax;
    result.expected[2] = AlignUp(result.requested[2], ZGranuleSize);
    ZPage* first = nullptr;
    for (size_t i = 0; i < 3; ++i) {
        std::memset(types[i], 0, sizeof(types[i]));
        auto* type = reinterpret_cast<TypeInfo*>(types[i]);
        type->SetType(TypeKind::TYPE_KIND_CLASS);
        type->SetInstanceSize(result.requested[i] - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(types[i]), sizeof(types[i]));
        const ObjRef object = MCC_NewObject(type, result.requested[i]);
        result.allocated[i] = object != nullptr;
        ZPage* page = object == nullptr ? nullptr : Heap::page(reinterpret_cast<uintptr_t>(object));
        result.firstMapped[i] = page != nullptr;
        if (page != nullptr) {
            result.actual[i] = page->size();
            result.type[i] = static_cast<unsigned>(page->type());
            result.lastMapped[i] = Heap::page(page->GetRegionEnd() - 1) == page;
        }
        if (i == 0) { first = page; }
    }
    const ObjRef smallAgain = MCC_NewObject(reinterpret_cast<TypeInfo*>(types[0]), result.requested[0]);
    result.sameSmallPage = first != nullptr && Heap::page(reinterpret_cast<uintptr_t>(smallAgain)) == first;
    result.tableSize = Heap::page_table().map().size();
    if (initialCache != nullptr) { Heap::free_page(initialCache); }
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZPageGranule, MutatorAllocatesThreePageSizes)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    GranuleAllocationResult result;
    CJThreadHandle handle = RunCJTask(AllocateGranulePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "GRANULE_TARGET table=%zu expected=%zu shared_small=%d\n",
                 result.tableSize, ZAddressOffsetMax >> ZGranuleSizeShift, result.sameSmallPage);
    // No fatal page-existence assertion precedes the geometry assertions.
    for (size_t i = 0; i < 3; ++i) {
        std::fprintf(stderr, "GRANULE_TARGET class=%zu allocated=%d mapped=%d last=%d bytes=%zu expected=%zu type=%u\n",
                     i, result.allocated[i], result.firstMapped[i], result.lastMapped[i],
                     result.actual[i], result.expected[i], result.type[i]);
        GC_EXPECT_EQ(result.actual[i], result.expected[i]);
        GC_EXPECT_EQ(result.type[i], static_cast<unsigned>(i));
        GC_EXPECT_TRUE(result.firstMapped[i] && result.lastMapped[i]);
    }
    GC_EXPECT_EQ(result.tableSize, ZAddressOffsetMax >> ZGranuleSizeShift);
    GC_EXPECT_TRUE(result.sameSmallPage);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
static size_t CountYoungForwardings()
{
    size_t n = 0;
    ZRelocationSetIterator iter(&ZGeneration::young()->relocation_set());
    for (ZForwarding* forwarding; iter.next(&forwarding);) {
        ++n;
    }
    return n;
}

struct ForwardingSelectionResult {
    size_t roots{0};
    size_t published{0};
    size_t retained{0};
    size_t retired{0};
    size_t inPlaceCount{0};
    size_t inPlaceRetained{0};
    size_t copyCount{0};
    size_t copyLeft{0};
    size_t receipts{0};
    size_t remapReceipts{0};
    bool verifyRetirement{false};
    bool verifyPreviousMark{false};
    bool verifyPromotion{false};
    bool observedMark{false};
    size_t previousMembers{0};
    size_t markReceipts{0};
    size_t promotedRoots{0};
    size_t registered{0};
    size_t registeredDistinct{0};
    bool verifyPin{false};
    bool verifyCritical{false};
    bool collectionFinishedWhileHeld{false};
    bool movedWhileHeld{false};
    bool movedAfterRelease{false};
    // Keep the raw holder inside until the collection finishes (bounded gate).
    bool heldWriteVisible{false};
    size_t forwardingsWhileHeld{0};
    uint64_t deselectedWhileHeld{0};
    size_t forwardingsAfterRelease{0};
    bool collectionFinished{false};
    uintptr_t pinExpected{0};
    uintptr_t pinObserved{0};
    bool pinCopied{true};
    size_t usedBefore{0};
    size_t usedAfter{0};
    size_t mappedBefore{0};
    size_t mappedAfter{0};
    size_t generationBefore[2]{};
    size_t generationAfter[2]{};
    size_t mappedGenerationBefore[2]{};
    size_t mappedGenerationAfter[2]{};
    size_t reused{0};
    size_t completed{0};
    size_t pending{0};
};
void* SelectRealLivePages(void* context)
{
    auto& result = *static_cast<ForwardingSelectionResult*>(context);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)];
    std::memset(storage, 0, sizeof(storage));
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    alignas(TypeInfo) static unsigned char sourceByteStorage[sizeof(TypeInfo)]{};
    if (result.verifyPin || result.verifyCritical) {
        auto* byteType = reinterpret_cast<TypeInfo*>(sourceByteStorage);
        byteType->SetType(TypeKind::TYPE_KIND_UINT8);
        byteType->SetInstanceSize(1);
        type->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        type->SetComponentTypeInfo(byteType);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(sourceByteStorage), sizeof(sourceByteStorage));
    }
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    uintptr_t starts[3]{};
    U64 roots[3]{};
    ZPage* sourcePages[3]{};
    MAddress sourceStarts[3]{};
    ZPage* previous = nullptr;
    for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && result.roots < 3; ++i) {
        BaseObject* object = (result.verifyPin || result.verifyCritical) ? static_cast<BaseObject*>(MCC_NewArray8(type, 4096))
                                             : static_cast<BaseObject*>(MCC_NewObject(type, 4096));
        if (object == nullptr) { return nullptr; }
        ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(object));
        if (page != previous) {
            starts[result.roots] = reinterpret_cast<uintptr_t>(object);
            if (result.verifyPin || result.verifyCritical) {
                static_cast<MArray*>(object)->ConvertToCArray()[0] = result.roots + 1;
            } else {
                *reinterpret_cast<uint64_t*>(starts[result.roots] + TYPEINFO_PTR_SIZE) = result.roots + 1;
            }
            roots[result.roots] = Heap::GetHeap().cross_vm().export_roots().RegisterExportRoot(reinterpret_cast<BaseObject*>(object));
            sourcePages[result.roots] = page;
            sourceStarts[result.roots] = page->GetRegionStart();
            ++result.roots;
            previous = page;
        }
    }
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    auto snapshot = [](size_t& used, size_t& mapped, size_t* generations, size_t* mappedGenerations) {
        auto& allocator = Heap::GetHeap().page_allocator();
        used = allocator.GetUsedBytes();
        generations[0] = allocator.used_generation(ZGenerationId::young);
        generations[1] = allocator.used_generation(ZGenerationId::old);
        ZPageTableIterator iter(&Heap::page_table());
        for (ZPage* page; iter.next(&page);) {
            mapped += page->size();
            mappedGenerations[page->generation_id() == ZGenerationId::young ? 0 : 1] += page->size();
        }
    };
    if (result.verifyRetirement) {
        snapshot(result.usedBefore, result.mappedBefore, result.generationBefore, result.mappedGenerationBefore);
    }
    const int savedTenuringThreshold = ZTenuringThreshold;
    // Live objects in three real small pages force a non-empty relocation set.
    if (result.verifyCritical) {
        auto root0 = [&]() {
            return static_cast<MArray*>(Heap::GetHeap().cross_vm().export_roots().GetExportRoot(roots[0]));
        };
        // Keep every survivor young, so that the page of root 0 is still a young
        // relocation candidate in the collection after release (the positive
        // control below); a flip-promoted page would leave the young set.
        ZTenuringThreshold = static_cast<int>(MaxTenuringThreshold);
        auto* array = root0();
        bool copied = true;
        void* raw = MCC_AcquireRawData(array, &copied);
        const uint64_t deselectedBefore = ZJNICritical::deselected_pages();
        // Let STW proceed while the array is held, as a holder blocked in a
        // foreign call does. The collection must complete without waiting for it.
        auto collect = [&](GCReason reason, std::atomic<bool>& finished) {
            mutator->EnterSaferegion(false);
            std::thread collector([&finished, reason] {
                Heap::GetHeap().RequestGC(reason);
                finished.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool done = finished.load(std::memory_order_acquire);
            mutator->LeaveSaferegion();
            return std::make_pair(done, std::move(collector));
        };
        std::atomic<bool> heldFinished{false};
        auto held = collect(GC_REASON_YOUNG, heldFinished);
        result.collectionFinishedWhileHeld = held.first;
        if (result.collectionFinishedWhileHeld) {
            result.movedWhileHeld = reinterpret_cast<uintptr_t>(root0()) != starts[0];
            result.forwardingsWhileHeld = CountYoungForwardings();
            result.deselectedWhileHeld = ZJNICritical::deselected_pages() - deselectedBefore;
        }
        // The holder writes through the raw pointer; the managed view sees it
        // before release.
        static_cast<unsigned char*>(raw)[0] = 0xa5;
        result.heldWriteVisible = root0()->ConvertToCArray() == raw &&
            static_cast<unsigned char*>(root0()->ConvertToCArray())[0] == 0xa5;
        MCC_ReleaseRawData(root0(), raw);
        mutator->EnterSaferegion(false);
        held.second.join();
        mutator->LeaveSaferegion();
        // Positive control: once released, the page is selectable again.
        std::atomic<bool> releasedFinished{false};
        auto released = collect(GC_REASON_YOUNG, releasedFinished);
        mutator->EnterSaferegion(false);
        released.second.join();
        mutator->LeaveSaferegion();
        result.collectionFinished = released.first;
        result.forwardingsAfterRelease = CountYoungForwardings();
        result.movedAfterRelease = reinterpret_cast<uintptr_t>(root0()) != starts[0];
        ZTenuringThreshold = savedTenuringThreshold;
    } else {
        if (result.verifyPromotion) {
            // Threshold 0 makes every selected page promote, so the install
            // task's track_if_promoted fills the registration array. A young
            // cycle keeps the finished set in place for the observation below.
            ZTenuringThreshold = 0;
            std::fprintf(stderr, "PROMOTION1315_PRECONDITION roots=%zu tenuring_threshold=0\n", result.roots);
        }
        Heap::GetHeap().RequestGC(result.verifyPromotion ? GC_REASON_YOUNG : GC_REASON_YOUNG);
    }
    if (result.verifyPromotion) {
        // The promote-all cycle registered its relocated pages through
        // ZRelocationSet::register_relocate_promoted; the registration array
        // is the product's own record, read here after the collection.
        ZArray<ZPage*>* const registeredPages = Heap::GetHeap().young().relocation_set().relocate_promoted_pages();
        result.registered = static_cast<size_t>(registeredPages->length());
        for (int i = 0; i < registeredPages->length(); ++i) {
            ZPage* const candidate = registeredPages->at(i);
            bool seen = false;
            for (int j = 0; j < i && !seen; ++j) {
                seen = registeredPages->at(j) == candidate;
            }
            result.registeredDistinct += seen ? 0 : 1;
        }
        std::fprintf(stderr, "PROMOTION1315_UNIQUENESS_TARGET executed=1 registered=%zu distinct=%zu\n",
                     result.registered, result.registeredDistinct);
        for (size_t i = 0; i < result.roots; ++i) {
            BaseObject* root = Heap::GetHeap().cross_vm().export_roots().GetExportRoot(roots[i]);
            ZPage* page = root == nullptr ? nullptr : Heap::page(reinterpret_cast<MAddress>(root));
            result.promotedRoots += page != nullptr && page->generation_id() == ZGenerationId::old;
            Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(roots[i]);
        }
        ZTenuringThreshold = savedTenuringThreshold;
        std::fprintf(stderr, "PROMOTION1315_TARGET executed=1 promoted=%zu roots=%zu\n",
                     result.promotedRoots, result.roots);
        mutator->SetManagedContext(true);
        return nullptr;
    }
    if (result.verifyPreviousMark) {
        auto& generation = Heap::GetHeap().young();
        ZRelocationSetIterator previous(&generation.relocation_set());
        for (ZForwarding* forwarding; previous.next(&forwarding);) {
            for (size_t i = 0; i < result.roots; ++i) {
                result.previousMembers += forwarding->covers(starts[i]) && forwarding->find(starts[i]) != 0;
            }
        }
        // Resolve the exported roots through the product load barrier before
        // the next cycle. The saved from-addresses above remain unchanged:
        // those addresses, not root repair, are the lifetime observation.
        for (size_t i = 0; i < result.roots; ++i) {
            (void)Heap::GetHeap().cross_vm().export_roots().GetExportRoot(roots[i]);
        }
        // A concurrent caller of the existing public static-root iterator
        // owns its normal table lock. Concurrent mark must visit that table;
        // holding this lock fixes the observation window after mark-start and
        // before mark-end, without a product test callback or phase hook.
        NativeSlot blockedRoot(zpointer::null);
        NativeSlot* blockedRoots[] = {&blockedRoot};
        LoaderManager::GetInstance()->RegisterStaticRoots(reinterpret_cast<Uptr>(blockedRoots), 1);
        std::promise<void> readerEntered;
        std::promise<void> releaseReader;
        auto readerReady = readerEntered.get_future();
        auto release = releaseReader.get_future();
        std::thread rootReader([&] {
            LoaderManager::GetInstance()->VisitStaticRoots([&](NativeSlot& slot) {
                if (&slot == &blockedRoot) {
                    readerEntered.set_value();
                    release.wait();
                }
            });
        });
        readerReady.wait();
        const bool wasSafe = mutator->InSaferegion();
        if (wasSafe) { mutator->LeaveSaferegion(); }
        const uint32_t before = generation.seqnum();
        std::thread collector([&] { Heap::GetHeap().RequestGC(GC_REASON_YOUNG); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (generation.seqnum() == before && std::chrono::steady_clock::now() < deadline) {
            ProcessSafepointIfRequested(ThreadLocal::GetThreadLocalData());
            std::this_thread::yield();
        }
        // A running mutator has returned from mark-start's real safepoint;
        // mark-end must rendezvous with it before reset can consume this set.
        result.observedMark = generation.is_phase_mark();
        for (size_t i = 0; i < result.roots; ++i) {
            ZForwarding* forwarding = generation.forwarding(starts[i]);
            result.markReceipts += forwarding != nullptr && forwarding->find(starts[i]) != 0;
        }
        std::fprintf(stderr, "MARK1315_PRECONDITION previous_members=%zu roots=%zu phase_mark=%d\n",
                     result.previousMembers, result.roots, result.observedMark);
        std::fprintf(stderr, "MARK1315_TARGET executed=1 retained_receipts=%zu expected=%zu\n",
                     result.markReceipts, result.previousMembers);
        releaseReader.set_value();
        rootReader.join();
        mutator->EnterSaferegion(false);
        collector.join();
        LoaderManager::GetInstance()->UnregisterStaticRoots(reinterpret_cast<Uptr>(blockedRoots), 1);
        if (!wasSafe) { mutator->LeaveSaferegion(); }
        for (size_t i = 0; i < result.roots; ++i) {
            Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(roots[i]);
        }
        mutator->SetManagedContext(true);
        return nullptr;
    }
    for (size_t i = 0; i < result.roots; ++i) {
        ZForwarding* forwarding = Heap::GetHeap().young().forwarding_table().get(starts[i]);
        if (forwarding != nullptr) {
            ++result.published;
            result.completed += forwarding->is_done() && forwarding->_ref_count.load() == 0 &&
                forwarding->find(starts[i]) != 0;
            ZPage* now = Heap::page(starts[i]);
            result.retired += now == nullptr;
            if (result.verifyRetirement) {
                const bool inPlace = forwarding->in_place();
                const bool slotEmpty = now == nullptr;
                const bool samePage = now != nullptr && now == sourcePages[i];
                const bool startMatches = now != nullptr && now->GetRegionStart() == sourceStarts[i];
                const int role = slotEmpty ? -1 : static_cast<int>(now->GetRegionRole());
                std::fprintf(stderr,
                             "SOURCE_PAGE index=%zu in_place=%d slot_empty=%d same_page=%d start_matches=%d role=%d\n",
                             i, inPlace ? 1 : 0, slotEmpty ? 1 : 0, samePage ? 1 : 0, startMatches ? 1 : 0, role);
                if (inPlace) {
                    ++result.inPlaceCount;
                    result.inPlaceRetained += startMatches ? 1 : 0;
                } else {
                    ++result.copyCount;
                    result.copyLeft += ((slotEmpty || !samePage)) ? 1 : 0;
                }
                // Observe the remap result before a second barrier consumes it.
                // Otherwise a disconnected lookup fails in that barrier before
                // the test can assert which product result was wrong.
                BaseObject* resolved = ZGeneration::young()->relocate_or_remap_object(
                    reinterpret_cast<BaseObject*>(starts[i]));
                const MAddress target = reinterpret_cast<MAddress>(resolved);
                result.receipts += target != starts[i] && Heap::page(target) != nullptr &&
                    (result.verifyPin ? static_cast<MArray*>(resolved)->ConvertToCArray()[0] == i + 1 :
                     *reinterpret_cast<uint64_t*>(target + TYPEINFO_PTR_SIZE) == i + 1);
                BaseObject* const remapped = ZGeneration::young()->remap_object(
                    reinterpret_cast<BaseObject*>(starts[i]));
                const MAddress remapTarget = reinterpret_cast<MAddress>(remapped);
                result.remapReceipts += remapTarget != starts[i] && Heap::page(remapTarget) != nullptr &&
                    (result.verifyPin ? static_cast<MArray*>(remapped)->ConvertToCArray()[0] == i + 1 :
                     *reinterpret_cast<uint64_t*>(remapTarget + TYPEINFO_PTR_SIZE) == i + 1);
            }

        }
        if (!result.verifyRetirement) {
            result.retained += Heap::GetHeap().cross_vm().export_roots().GetExportRoot(roots[i]) != nullptr;
            Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(roots[i]);
        }
        // The retirement test leaves root cleanup to FiniCJRuntime, after its
        // assertions. A failing remap must not be consumed by cleanup first.
    }
    for (auto id : {Generation::Old, Generation::Young}) {
        auto& queue = (*(*ZGeneration::generation(static_cast<ZGenerationId>(id))).relocate().queue());
        std::lock_guard<std::mutex> guard(queue.lock);
        result.pending += queue.queue.length();
    }
    if (result.verifyRetirement) {
        snapshot(result.usedAfter, result.mappedAfter, result.generationAfter, result.mappedGenerationAfter);
        // Real mutator allocation must be able to consume the returned source
        // range. Read forwarding results above before reusing that range.
        alignas(TypeInfo) static unsigned char byteStorage[sizeof(TypeInfo)]{};
        alignas(TypeInfo) static unsigned char arrayStorage[sizeof(TypeInfo)]{};
        auto* byteType = reinterpret_cast<TypeInfo*>(byteStorage);
        auto* arrayType = reinterpret_cast<TypeInfo*>(arrayStorage);
        byteType->SetType(TypeKind::TYPE_KIND_UINT8);
        byteType->SetInstanceSize(1);
        arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        arrayType->SetComponentTypeInfo(byteType);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(byteStorage), sizeof(byteStorage));
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(arrayStorage), sizeof(arrayStorage));
        for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && result.reused == 0; ++i) {
            BaseObject* object = (result.verifyPin || result.verifyCritical) ? static_cast<BaseObject*>(MCC_NewArray8(arrayType, 4096))
                                                 : static_cast<BaseObject*>(MCC_NewObject(type, 4096));
            const uintptr_t address = reinterpret_cast<uintptr_t>(object);
            for (size_t j = 0; j < result.roots; ++j) {
                result.reused += (address >> ZGranuleSizeShift) == (starts[j] >> ZGranuleSizeShift);
            }
            if (result.verifyPin && result.reused != 0) {
                auto* array = static_cast<MArray*>(object);
                result.pinExpected = reinterpret_cast<uintptr_t>(array->ConvertToCArray());
                void* raw = MCC_AcquireRawData(array, &result.pinCopied);
                result.pinObserved = reinterpret_cast<uintptr_t>(raw);
                MCC_ReleaseRawData(array, raw);
            }
        }
    }
    mutator->SetManagedContext(true);
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZForwardingPublication, SelectionPublishesPreparedForwardingOnce)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "FORWARDING_SELECTION_TARGET roots=%zu published=%zu retained=%zu\n",
                 result.roots, result.published, result.retained);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(result.retained, 3u);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// The product mutator allocates and roots the objects; RequestGC owns selection,
// copying, detach and retirement. No manually installed forwarding is involved.
GC_RUNTIME_OTHER_VM_TEST(ZRelocationRetirement, CopiedSourceLeavesPageTable)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyRetirement = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "SOURCE_RETIREMENT_TARGET roots=%zu selected=%zu retired=%zu remapped_payloads=%zu\n",
                 result.roots, result.published, result.retired, result.receipts);
    std::fprintf(stderr,
                 "SOURCE_CLASS_TARGET in_place=%zu retained=%zu copy=%zu left=%zu\n",
                 result.inPlaceCount, result.inPlaceRetained, result.copyCount, result.copyLeft);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(result.inPlaceCount + result.copyCount, result.published);
    GC_EXPECT_EQ(result.inPlaceRetained, result.inPlaceCount);
    GC_EXPECT_EQ(result.copyLeft, result.copyCount);
    std::fprintf(stderr, "FORWARD_RESULT_TARGET relocated=%zu remapped=%zu expected=%zu\n",
                 result.receipts, result.remapReceipts, result.published);
    GC_EXPECT_EQ(result.receipts, result.published);
    GC_EXPECT_EQ(result.remapReceipts, result.published);
    std::fprintf(stderr, "SOURCE_MEMORY_TARGET used_before=%zu used_after=%zu mapped_before=%zu mapped_after=%zu reused=%zu\n",
                 result.usedBefore, result.usedAfter, result.mappedBefore, result.mappedAfter, result.reused);
    GC_EXPECT_EQ(result.usedAfter + result.mappedBefore, result.usedBefore + result.mappedAfter);
    for (size_t i = 0; i < 2; ++i) {
        std::fprintf(stderr, "SOURCE_GENERATION_TARGET id=%zu before=%zu after=%zu mapped_before=%zu mapped_after=%zu\n",
                     i, result.generationBefore[i], result.generationAfter[i],
                     result.mappedGenerationBefore[i], result.mappedGenerationAfter[i]);
        GC_EXPECT_EQ(result.generationAfter[i] + result.mappedGenerationBefore[i],
                     result.generationBefore[i] + result.mappedGenerationAfter[i]);
    }
    GC_EXPECT_TRUE(result.reused > 0);
    GC_EXPECT_EQ(result.roots, 3u);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct OldCyclePhaseResult {
    uintptr_t before{0};
    uintptr_t after{0};
    bool relocate{false};
};
void* RunRealOldCycle(void* context)
{
    auto& result = *static_cast<OldCyclePhaseResult*>(context);
    result.before = ZPointerRemappedOldMask;
    Mutator::GetMutator()->SetManagedContext(false);
    Heap::GetHeap().RequestGC(GC_REASON_USER);
    result.after = ZPointerRemappedOldMask;
    result.relocate = Heap::GetHeap().old().is_phase_relocate();
    Mutator::GetMutator()->SetManagedContext(true);
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZGenerationPhases, OldCycleFlipsRemapMaskOnce)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    OldCyclePhaseResult result;
    CJThreadHandle handle = RunCJTask(RunRealOldCycle, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "OLD_CYCLE_FLIP_TARGET before=%#lx after=%#lx expected=%#lx relocate=%d\n",
                 result.before, result.after, result.before ^ ZPointerRemappedMask, result.relocate);
    GC_EXPECT_EQ(result.after, result.before ^ ZPointerRemappedMask);
    GC_EXPECT_TRUE(result.relocate);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, NewArrayOnReusedSourceKeepsDecodedAddress)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyRetirement = true;
    result.verifyPin = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "PIN_DECODED_TARGET expected=%#lx observed=%#lx reused=%zu copied=%d\n",
                 result.pinExpected, result.pinObserved, result.reused, result.pinCopied);
    GC_EXPECT_EQ(result.pinObserved, result.pinExpected);
    GC_EXPECT_TRUE(result.pinExpected != 0 && result.reused > 0);
    GC_EXPECT_FALSE(result.pinCopied);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// ZGC zJNICritical.cpp:102-129: a JavaThread that finds JNI critical blocked
// transitions to blocked before taking the condition lock. Use RunCJTask so
// the waiter enters through the product MCC_AcquireRawData path.
// An acquirer whose array lies on a page of an open selection does not hold it:
// it waits in saferegion (so a stop-the-world completes meanwhile) until the
// selection closes, then holds the reloaded array.
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, AcquireOnOpenSelectionWaitsInSaferegion)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    JNICriticalBlockedEnterResult result;
    CJThreadHandle handle = RunCJTask(AcquireWhileJNICriticalBlocked, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result.ready.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < readyDeadline) {
        std::this_thread::yield();
    }
    GC_EXPECT_TRUE(result.ready.load(std::memory_order_acquire));
    ZPage* selected = Heap::page(static_cast<MAddress>(result.array.load(std::memory_order_acquire)));
    const uint64_t retreatsBefore = ZJNICritical::retreats();
    GC_EXPECT_EQ(ZJNICritical::open_selection_for_test(ZGenerationId::young, &selected, 1), static_cast<size_t>(0));
    result.proceed.store(true, std::memory_order_release);
    while (!result.entering.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::atomic<bool> stwFinished{false};
    std::thread collector([&] {
        ScopedStopTheWorld stw("jni-critical-blocked-enter", false);
        stwFinished.store(true, std::memory_order_release);
    });
    const auto stwDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stwFinished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < stwDeadline) {
        std::this_thread::yield();
    }
    const bool stwFinishedWhileBlocked = stwFinished.load(std::memory_order_acquire);
    const bool acquiredWhileBlocked = result.acquired.load(std::memory_order_acquire);
    std::fprintf(stderr, "JNI_BLOCKED_ENTER_TARGET stw=%d acquired=%d count=%lld\n",
                 stwFinishedWhileBlocked ? 1 : 0, acquiredWhileBlocked ? 1 : 0,
                 static_cast<long long>(ZJNICritical::count_snapshot()));
    const uint64_t retreatsWhileOpen = ZJNICritical::retreats() - retreatsBefore;
    ZJNICritical::close_selection(ZGenerationId::young);
    collector.join();
    const auto acquireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!result.acquired.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < acquireDeadline) {
        std::this_thread::yield();
    }
    std::fprintf(stderr, "JNI_BLOCKED_ENTER_RESTORED acquired=%d saferegion=%d\n",
                 result.acquired.load(std::memory_order_acquire) ? 1 : 0,
                 result.saferegionAfterAcquire.load(std::memory_order_acquire) ? 1 : 0);
    SignalOnExit finishTask(result.finish);
    GC_EXPECT_TRUE(stwFinishedWhileBlocked);
    GC_EXPECT_FALSE(acquiredWhileBlocked);
    GC_EXPECT_EQ(retreatsWhileOpen, static_cast<uint64_t>(1));
    GC_EXPECT_TRUE(result.acquired.load(std::memory_order_acquire));
    GC_EXPECT_FALSE(result.saferegionAfterAcquire.load(std::memory_order_acquire));
    result.finish.store(true, std::memory_order_release);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_FALSE(result.copied);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

#if defined(MRT_TESTABLE_INTERNALS)
// ZGC zRelocate.cpp:1036-1047: a real worker owns detach/free/done.
// Pages come from the product allocator and the driver runs the actual tasks.
GC_RUNTIME_OTHER_VM_TEST(RelocateWorkers, RuntimeCollectionCompletesSelectedPages)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyRetirement = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "ACTUAL_FORWARD_TASK_TARGET selected=%zu completed=%zu pending=%zu receipts=%zu\n",
                 result.published, result.completed, result.pending, result.receipts);
    GC_EXPECT_EQ(result.completed, result.published);
    GC_EXPECT_EQ(result.receipts, result.published);
    GC_EXPECT_EQ(result.pending, 0u);
    GC_EXPECT_TRUE(result.published > 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
#endif

// ZGC zCollectedHeap.cpp:275-280 and zGeneration.cpp:474-486.
// A holder that stays inside across a young collection costs exactly its own
// page: the collection completes without waiting, the other live pages are still
// relocated, the held array keeps its address and C writes through the raw
// pointer are visible to managed code before release. After release the next
// young collection moves the array again (positive control for the exclusion).
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, RawHolderExcludesOnlyItsPage)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyCritical = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "JNI_PAGE_EXCLUSION_TARGET held_finished=%d held_moved=%d held_forwardings=%zu "
                 "deselected=%llu write_visible=%d released_finished=%d released_moved=%d roots=%zu\n",
                 result.collectionFinishedWhileHeld, result.movedWhileHeld, result.forwardingsWhileHeld,
                 static_cast<unsigned long long>(result.deselectedWhileHeld), result.heldWriteVisible,
                 result.collectionFinished, result.movedAfterRelease, result.roots);
    GC_EXPECT_TRUE(result.collectionFinishedWhileHeld);
    GC_EXPECT_FALSE(result.movedWhileHeld);
    GC_EXPECT_TRUE(result.forwardingsWhileHeld > 0);
    GC_EXPECT_EQ(result.deselectedWhileHeld, static_cast<uint64_t>(1));
    GC_EXPECT_TRUE(result.heldWriteVisible);
    GC_EXPECT_TRUE(result.collectionFinished);
    GC_EXPECT_TRUE(result.movedAfterRelease);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

#include "ObjectModel/MethodInfo.h"
#include "ObjectModel/FieldInfo.h"
namespace MapleRuntime {
extern "C" void* MCC_GetParameterAnnotations(ParameterInfo*, TypeInfo*);
extern "C" void* MCC_GetMethodAnnotations(MethodInfo*, TypeInfo*);
extern "C" void* MCC_GetInstanceFieldAnnotations(InstanceFieldInfo*, TypeInfo*);
extern "C" void* MCC_GetStaticFieldAnnotations(StaticFieldInfo*, TypeInfo*);
extern "C" void* MCC_GetTypeInfoAnnotations(TypeInfo*, TypeInfo*);
}
namespace {
struct AnnotationResult {
    unsigned entry = 0;
    TypeInfo* type = nullptr;
    uintptr_t before = 0;
    uintptr_t after = 0;
    uintptr_t returned = 0;
    bool called = false;
    bool copied = false;
};
thread_local AnnotationResult* annotationResult;
extern "C" void AnnotationCollect(uintptr_t* result)
{
    auto& r = *annotationResult;
    r.called = true;
    auto* mutator = Mutator::GetMutator();
    // Capture the registered slot once, independently of GC's process_head
    // visitor. The destructive arm changes only GC refresh, never this read.
    RootSlot* registeredSlot = nullptr;
    mutator->VisitMutatorRoots([&](RootSlot& root) {
        BaseObject* object = to_object(safe(root.LoadPlain()));
        if (object != nullptr && object->GetTypeInfo() == r.type) {
            registeredSlot = &root;
        }
    });
    r.before = registeredSlot == nullptr ? 0 : raw(registeredSlot->LoadPlain());
    std::fprintf(stderr, "ANNOTATION_HANDLE_PRECONDITION registered=%d before=%zx\n",
        registeredSlot != nullptr, r.before);
    {
        alignas(TypeInfo) static unsigned char garbageStorage[sizeof(TypeInfo)]{};
        auto* garbage = reinterpret_cast<TypeInfo*>(garbageStorage);
        garbage->SetType(TypeKind::TYPE_KIND_CLASS);
        garbage->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(garbageStorage), sizeof(garbageStorage));
        U64 roots[3]{};
        size_t count = 0;
        ZPage* previous = Heap::page(r.before);
        for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && count < 3; ++i) {
            auto* object = MCC_NewObject(garbage, 4096);
            auto* page = Heap::page(reinterpret_cast<uintptr_t>(object));
            if (page != previous) {
                roots[count++] = Heap::GetHeap().cross_vm().export_roots().RegisterExportRoot(object);
                previous = page;
            }
        }
        Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
        r.after = registeredSlot == nullptr ? 0 : raw(registeredSlot->LoadPlain());
        auto* forwarding = Heap::GetHeap().young().forwarding_table().get(r.before);
        std::fprintf(stderr, "ANNOTATION_FORWARDING from=%zx winner=%zx root=%zx\n", r.before,
            forwarding == nullptr ? 0 : forwarding->find(r.before), r.after);
        for (size_t i = 0; i < count; ++i) { Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(roots[i]); }
    }
    result[0] = 0;
    result[1] = 581;
    result[2] = 584;
}
void* RunAnnotation(void* context)
{
    auto& r = *static_cast<AnnotationResult*>(context);
    annotationResult = &r;
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_STRUCT);
    type->SetInstanceSize(3 * sizeof(uintptr_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    r.type = type;
    // Metadata uses the compiler's packed ABI, with the annotation code pointer
    // in the declared field. No product access hook or alternate stub is used.
    uintptr_t callback = reinterpret_cast<uintptr_t>(&AnnotationCollect);
    void* object = nullptr;
    if (r.entry == 0) {
        ParameterInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + sizeof(metadata) - sizeof(callback), &callback, sizeof(callback));
        object = MCC_GetParameterAnnotations(&metadata, type);
    } else if (r.entry == 1) {
        MethodInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + 32, &callback, sizeof(callback));
        object = MCC_GetMethodAnnotations(&metadata, type);
    } else if (r.entry == 2) {
        InstanceFieldInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + sizeof(metadata) - sizeof(callback), &callback, sizeof(callback));
        object = MCC_GetInstanceFieldAnnotations(&metadata, type);
    } else if (r.entry == 3) {
        StaticFieldInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + sizeof(metadata) - sizeof(callback), &callback, sizeof(callback));
        object = MCC_GetStaticFieldAnnotations(&metadata, type);
    } else {
        ReflectInfo metadata{};
        std::memcpy(reinterpret_cast<char*>(&metadata) + 24, &callback, sizeof(callback));
        alignas(TypeInfo) unsigned char annotatedStorage[sizeof(TypeInfo)]{};
        auto* annotated = reinterpret_cast<TypeInfo*>(annotatedStorage);
        annotated->SetType(TypeKind::TYPE_KIND_CLASS);
        annotated->SetFlag(FLAG_REFLECTION);
        annotated->SetReflectInfo(&metadata);
        object = MCC_GetTypeInfoAnnotations(annotated, type);
    }
    r.returned = reinterpret_cast<uintptr_t>(object);
    if (r.returned == r.after && r.after != 0) {
        auto* data = reinterpret_cast<uintptr_t*>(r.returned + TYPEINFO_PTR_SIZE);
        r.copied = data[1] == 581 && data[2] == 584;
    }
    return nullptr;
}
void CheckAnnotation(unsigned entry)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    AnnotationResult result;
    result.entry = entry;
    auto task = RunCJTask(RunAnnotation, &result);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "ANNOTATION_HANDLE_TARGET entry=%u called=%d before=%zx after=%zx returned=%zx copied=%d\n",
        entry, result.called, result.before, result.after, result.returned, result.copied);
    GC_EXPECT_TRUE(result.called && result.before != 0 && result.after != 0);
    GC_EXPECT_TRUE(result.before != result.after);
    GC_EXPECT_TRUE(result.returned == result.after && result.copied);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, Parameter) { CheckAnnotation(0); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, Method) { CheckAnnotation(1); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, InstanceField) { CheckAnnotation(2); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, StaticField) { CheckAnnotation(3); }
GC_RUNTIME_OTHER_VM_TEST(NativeAnnotationHandle, Type) { CheckAnnotation(4); }

#include "ObjectManager.inline.h"
#include "Heap/z/zRootsIterator.hpp"
#include "Heap/z/zAccess.hpp"
namespace MapleRuntime {
extern "C" void* MCC_ApplyCJStaticMethod(MethodInfo*, void*, TypeInfo*);
}
namespace {
void CollectSparsePages()
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(4096 - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    U64 roots[3]{};
    size_t count = 0;
    ZPage* previous = nullptr;
    for (size_t i = 0; i < 4 * ZPageSizeSmall / 4096 && count < 3; ++i) {
        auto* object = MCC_NewObject(type, 4096);
        auto* page = Heap::page(reinterpret_cast<uintptr_t>(object));
        if (page != previous) {
            roots[count++] = Heap::GetHeap().cross_vm().export_roots().RegisterExportRoot(object);
            previous = page;
        }
    }
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG);
    for (size_t i = 0; i < count; ++i) { Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(roots[i]); }
}
struct ArgumentResult {
    TypeInfo* argumentType = nullptr;
    TypeInfo* receiverType = nullptr;
    uintptr_t receiverBefore = 0;
    uintptr_t receiverAfter = 0;
    uintptr_t argumentWord = 0;
    uintptr_t argumentWordAfter = 0;
    uintptr_t referentBefore = 0;
    uintptr_t referentAfter = 0;
    uintptr_t returned = 0;
    U64 sourceRoot = 0;
    U64 referentRoot = 0;
    bool called = false;
    bool argumentOnStack = false;
    bool argumentValue = false;
    bool retainedValue = false;
};
thread_local ArgumentResult* argumentResult;
extern "C" void InitializeReflectedReceiver(MObject* receiver, void* argument, TypeInfo*)
{
    auto& r = *argumentResult;
    r.called = true;
    r.receiverBefore = reinterpret_cast<uintptr_t>(receiver);
    if (receiver != nullptr) { receiver->Store<U64>(TYPEINFO_PTR_SIZE, 584); }
    auto& exportRoots = Heap::GetHeap().cross_vm().export_roots();
    if (argument != nullptr) {
        // A value parameter is the caller's uncolored record on the CJThread
        // stack (ScopedAllocBuffer::StageValueRecord), not a heap box payload.
        r.argumentOnStack = Mutator::GetMutator()->IsStackAddr(reinterpret_cast<uintptr_t>(argument));
        r.argumentWord = static_cast<uintptr_t*>(argument)[0];
        r.referentBefore = reinterpret_cast<uintptr_t>(exportRoots.GetExportRoot(r.referentRoot));
        r.argumentValue = static_cast<U64*>(argument)[1] == 581;
    }
    CollectSparsePages();
    auto* referent = exportRoots.GetExportRoot(r.referentRoot);
    r.referentAfter = reinterpret_cast<uintptr_t>(referent);
    // The runtime names the record's reference with a native frame root, so
    // the collection heals the record word itself while this callee runs.
    if (argument != nullptr) { r.argumentWordAfter = static_cast<uintptr_t*>(argument)[0]; }
    r.retainedValue = referent != nullptr && reinterpret_cast<U64*>(referent)[1] == 583;
    Mutator::GetMutator()->VisitMutatorRoots([&](RootSlot& root) {
        auto* object = to_object(safe(root.LoadPlain()));
        if (object == nullptr || Mutator::GetMutator()->IsStackAddr(reinterpret_cast<uintptr_t>(object))) { return; }
        if (object->GetTypeInfo() == r.receiverType) {
            r.receiverAfter = reinterpret_cast<uintptr_t>(object);
        }
    });
}
void* RunArguments(void* context)
{
    auto& r = *static_cast<ArgumentResult*>(context);
    argumentResult = &r;
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[5][sizeof(TypeInfo)]{};
    TypeInfo* type[5];
    for (unsigned i = 0; i < 5; ++i) {
        type[i] = reinterpret_cast<TypeInfo*>(storage[i]);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage[i]), sizeof(TypeInfo));
    }
    type[0]->SetType(TypeKind::TYPE_KIND_STRUCT); type[0]->SetInstanceSize(16);
    type[0]->SetFlagHasRefField();
    GCTib bitmap{}; bitmap.tag = SIGN_BIT | 1; type[0]->SetGCTib(bitmap);
    type[1]->SetType(TypeKind::TYPE_KIND_CLASS); type[1]->SetInstanceSize(8);
    type[2]->SetType(TypeKind::TYPE_KIND_UNIT); type[2]->SetInstanceSize(0);
    type[3]->SetType(TypeKind::TYPE_KIND_RAWARRAY); type[3]->SetComponentTypeInfo(type[1]);
    type[4]->SetType(TypeKind::TYPE_KIND_CLASS); type[4]->SetInstanceSize(8);
    r.argumentType = type[0]; r.receiverType = type[1];
    auto* referent = MCC_NewObject(type[4], 16);
    referent->Store<U64>(TYPEINFO_PTR_SIZE, 583);
    r.referentRoot = Heap::GetHeap().cross_vm().export_roots().RegisterExportRoot(referent);
    auto* source = MCC_NewObject(type[0], 24);
    source->Store<U64>(TYPEINFO_PTR_SIZE + sizeof(Uptr), 581);
    r.sourceRoot = Heap::GetHeap().cross_vm().export_roots().RegisterExportRoot(source);
    HeapAccess<>::oop_store(&(source->GetRefField(TYPEINFO_PTR_SIZE)), referent);
    auto* array = ObjectManager::NewObjArray(1, type[3]);
    array->SetRefElement(0, source);
    ParameterInfo parameter{}; parameter.SetType(type[0]);
    MethodInfo method{};
    method.SetMethodName("init");
    method.SetActualParameterInfos(reinterpret_cast<Uptr>(&parameter));
    method.SetDeclaringTypeInfo(type[1]);
    auto set = [&](size_t offset, auto value) {
        std::memcpy(reinterpret_cast<char*>(&method) + offset, &value, sizeof(value));
    };
    set(12, U16(1)); set(16, reinterpret_cast<Uptr>(&InitializeReflectedReceiver)); set(24, type[2]);
    CJArray args{};
    StorePlain(RootSlotAt(&args.rawPtr), from_object(array)); args.length = 1;
    r.returned = reinterpret_cast<uintptr_t>(MCC_ApplyCJStaticMethod(&method, &args, nullptr));
    Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(r.sourceRoot);
    Heap::GetHeap().cross_vm().export_roots().RemoveExportRoot(r.referentRoot);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeArgumentHandle, CallbackRetainsReceiverAndArguments)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ArgumentResult result;
    auto task = RunCJTask(RunArguments, &result);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &taskResult), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "ARGUMENT_HANDLE_TARGET called=%d receiver=%zx/%zx returned=%zx stack=%d word=%zx/%zx "
        "referent=%zx/%zx value=%d retained=%d\n",
        result.called, result.receiverBefore, result.receiverAfter, result.returned, result.argumentOnStack,
        result.argumentWord, result.argumentWordAfter, result.referentBefore, result.referentAfter,
        result.argumentValue, result.retainedValue);
    GC_EXPECT_TRUE(result.called && result.argumentOnStack && result.argumentValue && result.referentBefore != 0 &&
        result.argumentWord == result.referentBefore && result.referentAfter != 0 &&
        result.referentBefore != result.referentAfter && result.argumentWordAfter == result.referentAfter &&
        result.retainedValue);
    GC_EXPECT_TRUE(result.receiverBefore != 0 && result.receiverAfter != 0 &&
        result.receiverBefore != result.receiverAfter && result.returned == result.receiverAfter);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct ReturnResult {
    uintptr_t before = 0; uintptr_t after = 0; uintptr_t encoded = 0; U64 marker = 0; U64 tail = 0;
};
// Use a real indirect aggregate return on each native ABI. In particular,
// AAPCS64 passes the result address in x8, not the ordinary first argument x0.
// Three words also prevent the native compiler from returning this in x0/x1.
struct ReflectedStructResult { uintptr_t reference; U64 marker; U64 tail; };
static_assert(sizeof(ReflectedStructResult) == 3 * sizeof(Uptr));
thread_local ReturnResult* returnResult;
extern "C" ReflectedStructResult ReturnReflectedStruct(TypeInfo*)
{
    auto& r = *returnResult;
    auto* mutator = Mutator::GetMutator();
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS); type->SetInstanceSize(8);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    Handle value(mutator, MCC_NewObject(type, 16));
    r.before = reinterpret_cast<uintptr_t>(value());
    CollectSparsePages();
    r.after = reinterpret_cast<uintptr_t>(value());
    // The native compiler writes the actual plain sret value to the buffer
    // supplied by ApplyCangjieMethodStub; the product boxer consumes it.
    return {r.after, 584, 585};
}
void* RunReturn(void* context)
{
    auto& r = *static_cast<ReturnResult*>(context);
    returnResult = &r;
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_STRUCT);
    type->SetInstanceSize(sizeof(ReflectedStructResult)); type->SetFlagHasRefField();
    GCTib bitmap{}; bitmap.tag = SIGN_BIT | 1; type->SetGCTib(bitmap);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    MethodInfo method{}; method.SetMethodName("return_value");
    auto set = [&](size_t offset, auto value) {
        std::memcpy(reinterpret_cast<char*>(&method) + offset, &value, sizeof(value));
    };
    set(8, U32(MODIFIER_STATIC | MODIFIER_HAS_SRET0));
    set(16, reinterpret_cast<Uptr>(&ReturnReflectedStruct)); set(24, type);
    auto* result = static_cast<MObject*>(MCC_ApplyCJStaticMethod(&method, nullptr, nullptr));
    r.encoded = raw(result->GetRefField(TYPEINFO_PTR_SIZE).GetFieldValue());
    r.marker = result->Load<U64>(TYPEINFO_PTR_SIZE + sizeof(Uptr));
    r.tail = result->Load<U64>(TYPEINFO_PTR_SIZE + 2 * sizeof(Uptr));
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeReturnHandle, HeaderlessResultAfterCollection)
{
    RuntimeParam param{}; param.heapParam.heapSize = 512 * 1024; param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ReturnResult result;
    auto task = RunCJTask(RunReturn, &result);
    void* value = nullptr; GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK); ReleaseHandle(task);
    const uintptr_t expected = raw(ZAddress::store_good(to_zaddress(result.after)));
    std::fprintf(stderr, "RETURN_HANDLE_TARGET before=%zx after=%zx encoded=%zx expected=%zx marker=%llu tail=%llu\n",
        result.before, result.after, result.encoded, expected,
        (unsigned long long)result.marker, (unsigned long long)result.tail);
    GC_EXPECT_TRUE(result.before != 0 && result.before != result.after && result.encoded == expected &&
        result.marker == 584 && result.tail == 585);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
extern "C" void GcUnitCJSRet16Body(void* output)
{
    auto& r = *returnResult;
    auto* mutator = Mutator::GetMutator();
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS); type->SetInstanceSize(8);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    Handle value(mutator, MCC_NewObject(type, 16));
    r.before = reinterpret_cast<uintptr_t>(value());
    CollectSparsePages();
    r.after = reinterpret_cast<uintptr_t>(value());
    auto* words = static_cast<uintptr_t*>(output);
    words[0] = r.after;
    words[1] = 584;
}

void* RunCJReturn16(void* context)
{
    auto& r = *static_cast<ReturnResult*>(context);
    returnResult = &r;
    Mutator::GetMutator()->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_STRUCT); type->SetInstanceSize(16); type->SetFlagHasRefField();
    GCTib bitmap{}; bitmap.tag = SIGN_BIT | 1; type->SetGCTib(bitmap);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    MethodInfo method{}; method.SetMethodName("return_cj_16byte_value");
    auto set = [&](size_t offset, auto value) {
        std::memcpy(reinterpret_cast<char*>(&method) + offset, &value, sizeof(value));
    };
    set(8, U32(MODIFIER_STATIC | MODIFIER_HAS_SRET0));
    set(16, reinterpret_cast<Uptr>(&GcUnitCJSRet16Entry)); set(24, type);
    auto* result = static_cast<MObject*>(MCC_ApplyCJStaticMethod(&method, nullptr, nullptr));
    r.encoded = raw(result->GetRefField(TYPEINFO_PTR_SIZE).GetFieldValue());
    r.marker = result->Load<U64>(TYPEINFO_PTR_SIZE + sizeof(Uptr));
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(NativeReturnHandle, Cangjie16ByteResultAfterCollection)
{
    RuntimeParam param{}; param.heapParam.heapSize = 512 * 1024; param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ReturnResult result;
    auto task = RunCJTask(RunCJReturn16, &result);
    void* value = nullptr; GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK); ReleaseHandle(task);
    const uintptr_t expected = raw(ZAddress::store_good(to_zaddress(result.after)));
    std::fprintf(stderr, "RETURN_CJ16_TARGET before=%zx after=%zx encoded=%zx expected=%zx marker=%llu\n",
        result.before, result.after, result.encoded, expected, (unsigned long long)result.marker);
    GC_EXPECT_TRUE(result.before != 0 && result.before != result.after &&
        result.encoded == expected && result.marker == 584);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct StableHandleResult { uintptr_t before = 0; uintptr_t after = 0; };
void* GrowHandleArea(void* context)
{
    auto& result = *static_cast<StableHandleResult*>(context);
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    HandleMark mark(*mutator);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(Uptr));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* object = MCC_NewObject(type, 2 * sizeof(Uptr));
    Handle first(mutator, object);
    result.before = reinterpret_cast<uintptr_t>(first());
    // Cross more than one storage block without changing the first Handle.
    for (unsigned i = 0; i < 128; ++i) { Handle next(mutator, object); }
    result.after = reinterpret_cast<uintptr_t>(first());
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(NativeHandle, SlotsStayStableAcrossGrowth)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    StableHandleResult result;
    auto task = RunCJTask(GrowHandleArea, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "HANDLE_SLOT_TARGET before=%zx after=%zx added=128\n", result.before, result.after);
    GC_EXPECT_TRUE(result.before != 0 && result.before == result.after);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// ZGC zPage.inline.hpp:180-186 and zGeneration.inline.hpp:46-48.
// Hold only the aggregate snapshot lock. The live-object query is imported
// from the product SO; its page-state result must complete while it is held.
#include "gc_generation_test.hpp"
namespace {
struct SequenceReadResult {
    bool collected = false;
    BaseObject* object = nullptr;
    U64 root = 0;
};
void* AllocateForSequenceRead(void* context)
{
    auto& result = *static_cast<SequenceReadResult*>(context);
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    const size_t size = result.collected ? ZObjectSizeLimitMedium + ZGranuleSize : 32;
    type->SetInstanceSize(size - TYPEINFO_PTR_SIZE);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    result.object = static_cast<BaseObject*>(MCC_NewObject(type, size));
    result.root = Heap::GetHeap().cross_vm().export_roots().RegisterExportRoot(result.object);
    if (result.collected) {
        Mutator::GetMutator()->SetManagedContext(false);
        Heap::GetHeap().RequestGC(GC_REASON_USER);
        Heap::GetHeap().RequestGC(GC_REASON_USER);
        result.object = Heap::GetHeap().cross_vm().export_roots().GetExportRoot(result.root);
        Mutator::GetMutator()->SetManagedContext(true);
    }
    return nullptr;
}
}

namespace {
void CheckLiveQueryWithGenerationSequence(bool collected)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    SequenceReadResult result;
    result.collected = collected;
    CJThreadHandle handle = RunCJTask(AllocateForSequenceRead, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    GC_EXPECT_TRUE(result.object != nullptr);
    ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(result.object));
    ZGeneration& generation = *page->generation();
    const auto expected = generation.seqnum();
    const bool live = Heap::GetHeap().IsSurvivedObject(result.object);
    const bool allocating = page->IsAllocating();
    const bool relocatable = page->IsRelocatable();
    const uint32_t sequence = generation.seqnum();
    std::fprintf(stderr, "SEQNUM_LIVE_ASSERT live=%u allocating=%u relocatable=%u sequence=%u expected=%u\n",
        live, allocating, relocatable, sequence, expected);
    GC_EXPECT_TRUE(live && allocating == !collected && relocatable == collected);
    GC_EXPECT_EQ(sequence, expected);
}
}
GC_RUNTIME_OTHER_VM_TEST(ZPageSequence, LiveQueryUsesGenerationSequence)
{
    CheckLiveQueryWithGenerationSequence(false);
}
GC_RUNTIME_OTHER_VM_TEST(ZPageSequence, MarkedLiveQueryUsesGenerationSequence)
{
    CheckLiveQueryWithGenerationSequence(true);
}

namespace {
struct CycleSequenceResult {
    uint32_t youngBefore = 0, youngAfter = 0, oldBefore = 0, oldAfter = 0;
};
void* CollectForSequenceRead(void* context)
{
    auto& result = *static_cast<CycleSequenceResult*>(context);
    auto& heap = Heap::GetHeap();
    Mutator::GetMutator()->SetManagedContext(false);
    result.youngBefore = heap.young().seqnum();
    result.oldBefore = heap.old().seqnum();
    heap.RequestGC(GC_REASON_USER);
    result.youngAfter = heap.young().seqnum();
    result.oldAfter = heap.old().seqnum();
    Mutator::GetMutator()->SetManagedContext(true);
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZPageSequence, CollectionPublishesBothGenerationSequences)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CycleSequenceResult result;
    CJThreadHandle handle = RunCJTask(CollectForSequenceRead, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    std::fprintf(stderr, "SEQNUM_PUBLISH_ASSERT young=%u->%u old=%u->%u \n",
        result.youngBefore, result.youngAfter, result.oldBefore, result.oldAfter);
    // Explicit major request: young preclean + full roots; one old mark start.
    // ZGC zDriver.cpp:270-279,416-428; zGeneration.cpp:871,1231.
    GC_EXPECT_EQ(result.youngAfter, result.youngBefore + 2);
    GC_EXPECT_EQ(result.oldAfter, result.oldBefore + 1);
}

namespace {
struct NestedRawResult {
    bool block = false;
    std::atomic<uintptr_t> outerArray{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> proceed{false};
    std::atomic<bool> nested{false};
    std::atomic<bool> finish{false};
    int64_t first = 0, second = 0, innerReleased = 0, final = 0;
    int64_t nullReleased = 0, emptyReleased = 0, stackReleased = 0;
};

void* NestedRawTask(void* context)
{
    auto& result = *static_cast<NestedRawResult*>(context);
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* byteType = reinterpret_cast<TypeInfo*>(types[0]);
    auto* arrayType = reinterpret_cast<TypeInfo*>(types[1]);
    byteType->SetType(TypeKind::TYPE_KIND_UINT8);
    byteType->SetInstanceSize(1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(byteType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(types), sizeof(types));
    HandleMark mark(*Mutator::GetMutator());
    Handle array(Mutator::GetMutator(), MCC_NewArray8(arrayType, 8));
    Handle empty(Mutator::GetMutator(), MCC_NewArray8(arrayType, 0));
    auto* a = static_cast<MArray*>(array());
    void* outer = MCC_AcquireRawData(a, nullptr);
    result.first = ZJNICritical::count_snapshot();
    result.outerArray.store(reinterpret_cast<uintptr_t>(a), std::memory_order_release);
    result.entered.store(true, std::memory_order_release);
    if (result.block) {
        while (!result.proceed.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    }
    void* inner = MCC_AcquireRawData(a, nullptr);
    result.second = ZJNICritical::count_snapshot();
    MCC_ReleaseRawData(a, inner);
    result.innerReleased = ZJNICritical::count_snapshot();
    void* nullRaw = MCC_AcquireRawData(nullptr, nullptr);
    MCC_ReleaseRawData(nullptr, nullRaw);
    result.nullReleased = ZJNICritical::count_snapshot();
    auto* e = static_cast<MArray*>(empty());
    void* emptyRaw = MCC_AcquireRawData(e, nullptr);
    MCC_ReleaseRawData(e, emptyRaw);
    result.emptyReleased = ZJNICritical::count_snapshot();
    alignas(MArray) unsigned char storage[sizeof(MArray) + 8]{};
    auto* stackArray = reinterpret_cast<MArray*>(storage);
    void* stackRaw = MCC_AcquireRawData(stackArray, nullptr);
    MCC_ReleaseRawData(stackArray, stackRaw);
    result.stackReleased = ZJNICritical::count_snapshot();
    result.nested.store(true, std::memory_order_release);
    if (result.block) {
        while (!result.finish.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    }
    MCC_ReleaseRawData(a, outer);
    result.final = ZJNICritical::count_snapshot();
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, NestedRawAcquireAndEarlyReturns)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    NestedRawResult result;
    CJThreadHandle task = RunCJTask(NestedRawTask, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "NESTED_RAW_TARGET first=%lld nested=%lld inner_release=%lld null=%lld empty=%lld stack=%lld final=%lld\n",
        (long long)result.first, (long long)result.second, (long long)result.innerReleased,
        (long long)result.nullReleased, (long long)result.emptyReleased,
        (long long)result.stackReleased, (long long)result.final);
    GC_EXPECT_EQ(result.second, 1);
    GC_EXPECT_EQ(result.innerReleased, 1);
    GC_EXPECT_EQ(result.nullReleased, 1);
    GC_EXPECT_EQ(result.emptyReleased, 1);
    GC_EXPECT_EQ(result.stackReleased, 1);
    GC_EXPECT_EQ(result.final, 0);
    GC_EXPECT_EQ(result.first, 1);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
struct RawDataShapeResult {
    MIndex length = 0;
    bool zeroWidth = false;
    bool replaceRaw = false;
    void* releaseValue = nullptr;
    void* actual = nullptr;
    void* expected = nullptr;
    bool copied = true;
    int64_t acquired = -1;
    int64_t released = -1;
};

void* RawDataShapeTask(void* context)
{
    auto& result = *static_cast<RawDataShapeResult*>(context);
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* elementType = reinterpret_cast<TypeInfo*>(types[0]);
    auto* arrayType = reinterpret_cast<TypeInfo*>(types[1]);
    elementType->SetType(result.zeroWidth ? TypeKind::TYPE_KIND_UNIT : TypeKind::TYPE_KIND_UINT8);
    elementType->SetInstanceSize(result.zeroWidth ? 0 : 1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(elementType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(types), sizeof(types));
    HandleMark mark(*Mutator::GetMutator());
    Handle array(Mutator::GetMutator(), MCC_NewArray8(arrayType, result.length));
    result.actual = MCC_AcquireRawData(static_cast<MArray*>(array()), &result.copied);
    // Observe the product result after the real entry has returned. No payload
    // read is valid for zero length/width; the invariant is the base address.
    result.expected = static_cast<MArray*>(array())->ConvertToCArray();
    result.acquired = ZJNICritical::count_snapshot();
    MCC_ReleaseRawData(static_cast<MArray*>(array()),
        result.replaceRaw ? result.releaseValue : result.actual);
    result.released = ZJNICritical::count_snapshot();
    return nullptr;
}

void CheckRawDataShape(RawDataShapeResult& result)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    CJThreadHandle task = RunCJTask(RawDataShapeTask, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr,
        "RAWDATA_SHAPE_TARGET length=%zu zero_width=%d raw=%p expected=%p copied=%d acquired=%lld released=%lld\n",
        static_cast<size_t>(result.length), result.zeroWidth, result.actual, result.expected,
        result.copied, static_cast<long long>(result.acquired), static_cast<long long>(result.released));
    // Separate address and pin-lifetime witnesses; no existence assertion can
    // hide the returned-address assertion (ZGC jni.cpp:2881).
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(result.actual), reinterpret_cast<uintptr_t>(result.expected));
    GC_EXPECT_EQ(result.copied, false);
    GC_EXPECT_EQ(result.acquired, 1);
    GC_EXPECT_EQ(result.released, 0); // ZGC jni.cpp:2893 ignores carray.
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, EmptyRawArrayReturnsContentAddress)
{
    RawDataShapeResult result;
    CheckRawDataShape(result);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, ZeroWidthRawArrayReturnsContentAddress)
{
    RawDataShapeResult result;
    result.zeroWidth = true;
    result.length = 1;
    CheckRawDataShape(result);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, EmptyZeroWidthRawArrayReturnsContentAddress)
{
    RawDataShapeResult result;
    result.zeroWidth = true;
    CheckRawDataShape(result);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, ReleaseRawDataIgnoresNullData)
{
    RawDataShapeResult result;
    result.length = 8;
    result.replaceRaw = true;
    CheckRawDataShape(result);
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, ReleaseRawDataIgnoresSentinelData)
{
    RawDataShapeResult result;
    result.length = 8;
    result.replaceRaw = true;
    result.releaseValue = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    CheckRawDataShape(result);
}

namespace {
struct StaleHeaderRawResult {
    int64_t afterAcquire = -1;
    int64_t afterRelease = -1;
    bool releaseReturned = false;
};

void* StaleHeaderRawTask(void* context)
{
    auto& result = *static_cast<StaleHeaderRawResult*>(context);
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* byteType = reinterpret_cast<TypeInfo*>(types[0]);
    auto* arrayType = reinterpret_cast<TypeInfo*>(types[1]);
    byteType->SetType(TypeKind::TYPE_KIND_UINT8);
    byteType->SetInstanceSize(1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(byteType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(types), sizeof(types));
    HandleMark mark(*Mutator::GetMutator());
    Handle array(Mutator::GetMutator(), MCC_NewArray8(arrayType, 8));
    auto* a = static_cast<MArray*>(array());
    void* raw = MCC_AcquireRawData(a, nullptr);
    result.afterAcquire = ZJNICritical::count_snapshot();
    TypeInfo* saved = a->GetTypeInfo();
    // ZGC jni.cpp:2881-2887 / zCollectedHeap.cpp:279-281: unpin re-resolves the
    // reference and never dereferences the object header. A stale header
    // (moved/reclaimed from-version) must not be read by release.
    a->SetClassInfo(reinterpret_cast<TypeInfo*>(static_cast<uintptr_t>(0xdeadbeef8ULL)));
    MCC_ReleaseRawData(a, raw);
    result.releaseReturned = true;
    a->SetClassInfo(saved);
    result.afterRelease = ZJNICritical::count_snapshot();
    return nullptr;
}
}

GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, ReleaseRawDataIgnoresStaleHeader)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    StaleHeaderRawResult result;
    CJThreadHandle task = RunCJTask(StaleHeaderRawTask, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "STALE_HEADER_RELEASE_TARGET acquired=%lld released=%lld returned=%d\n",
        static_cast<long long>(result.afterAcquire), static_cast<long long>(result.afterRelease),
        result.releaseReturned ? 1 : 0);
    GC_EXPECT_TRUE(result.releaseReturned);
    GC_EXPECT_EQ(result.afterAcquire, 1);
    GC_EXPECT_EQ(result.afterRelease, 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

// Nested acquires of a mutator that already holds an array on a selected page
// are covered by that hold: they neither wait nor change the holder count.
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, NestedRawAcquireOnOpenSelection)
{
    RuntimeParam param{};
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    NestedRawResult result;
    result.block = true;
    CJThreadHandle task = RunCJTask(NestedRawTask, &result);
    while (!result.entered.load(std::memory_order_acquire)) { std::this_thread::yield(); }
    ZPage* selected = Heap::page(static_cast<MAddress>(result.outerArray.load(std::memory_order_acquire)));
    const uint64_t retreatsBefore = ZJNICritical::retreats();
    const size_t kept = ZJNICritical::open_selection_for_test(ZGenerationId::young, &selected, 1);
    result.proceed.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!result.nested.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool completed = result.nested.load(std::memory_order_acquire);
    const uint64_t retreats = ZJNICritical::retreats() - retreatsBefore;
    std::fprintf(stderr, "NESTED_SELECTION_TARGET completed=%d kept=%zu holders=%lld retreats=%llu\n",
        completed, kept, (long long)ZJNICritical::count_snapshot(), (unsigned long long)retreats);
    ZJNICritical::close_selection(ZGenerationId::young);
    const int64_t held = ZJNICritical::count_snapshot();
    result.finish.store(true, std::memory_order_release);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    GC_EXPECT_TRUE(completed);
    GC_EXPECT_EQ(kept, static_cast<size_t>(1));
    GC_EXPECT_EQ(retreats, static_cast<uint64_t>(0));
    GC_EXPECT_EQ(held, 1);
    GC_EXPECT_EQ(result.second, 1);
    GC_EXPECT_EQ(result.innerReleased, 1);
    GC_EXPECT_EQ(result.nullReleased, 1);
    GC_EXPECT_EQ(result.emptyReleased, 1);
    GC_EXPECT_EQ(result.stackReleased, 1);
    GC_EXPECT_EQ(result.final, 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
// Selection scan over per-mutator holds: exactly the selected pages that hold an
// array are kept; an overflowed (anonymous) hold keeps every selected page; holds
// released in any order leave nothing behind; an old selection lets an acquirer
// hold until its final scan, which then reports the page.
struct RawScanResult {
    bool distinct = false;
    size_t keptTwo = SIZE_MAX;
    size_t keptCovered = SIZE_MAX;
    uint64_t coveredRetreats = UINT64_MAX;
    bool coveredNeighbour = false;
    int64_t holdsOverflow = -1;
    size_t keptOverflow = SIZE_MAX;
    size_t keptOverflowReleased = SIZE_MAX;
    size_t keptAfter = SIZE_MAX;
    int64_t holdsAfter = -1;
    size_t oldOpenKept = SIZE_MAX;
    bool oldFinalHeld = false;
    uint64_t oldRetreats = UINT64_MAX;
};
void* RawScanTask(void* context)
{
    auto& result = *static_cast<RawScanResult*>(context);
    auto* mutator = Mutator::GetMutator();
    alignas(TypeInfo) static unsigned char types[2][sizeof(TypeInfo)]{};
    auto* byteType = reinterpret_cast<TypeInfo*>(types[0]);
    auto* arrayType = reinterpret_cast<TypeInfo*>(types[1]);
    byteType->SetType(TypeKind::TYPE_KIND_UINT8);
    byteType->SetInstanceSize(1);
    arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
    arrayType->SetComponentTypeInfo(byteType);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(types), sizeof(types));
    HandleMark mark(*mutator);
    std::vector<Handle> arrays;
    for (size_t i = 0; i < 2 * ZPageSizeSmall / 4096 + 32; ++i) {
        arrays.emplace_back(mutator, MCC_NewArray8(arrayType, 4096));
    }
    Heap::GetHeap().RequestGC(GC_REASON_USER);
    auto at = [&arrays](size_t i) { return static_cast<MArray*>(arrays[i]()); };
    auto pageOf = [](MArray* a) { return Heap::page(reinterpret_cast<MAddress>(a)); };
    std::vector<ZPage*> pages;
    std::vector<size_t> first;
    for (size_t i = 0; i < arrays.size() && pages.size() < 3; ++i) {
        if (std::find(pages.begin(), pages.end(), pageOf(at(i))) == pages.end()) {
            pages.push_back(pageOf(at(i)));
            first.push_back(i);
        }
    }
    result.distinct = pages.size() == 3;
    if (!result.distinct) {
        return nullptr;
    }
    void* r0 = MCC_AcquireRawData(at(first[0]), nullptr);
    void* r1 = MCC_AcquireRawData(at(first[1]), nullptr);
    result.keptTwo = ZJNICritical::open_selection_for_test(ZGenerationId::young, pages.data(), pages.size());
    ZJNICritical::close_selection(ZGenerationId::young);
    const uint64_t retreatsBefore = ZJNICritical::retreats();
    result.keptCovered = ZJNICritical::open_selection_for_test(ZGenerationId::young, &pages[0], 1);
    void* again = MCC_AcquireRawData(at(first[0]), nullptr);
    size_t neighbour = SIZE_MAX;
    for (size_t i = 0; i < arrays.size(); ++i) {
        if (i != first[0] && pageOf(at(i)) == pages[0]) {
            neighbour = i;
            break;
        }
    }
    result.coveredNeighbour = neighbour != SIZE_MAX;
    void* n = result.coveredNeighbour ? MCC_AcquireRawData(at(neighbour), nullptr) : nullptr;
    result.coveredRetreats = ZJNICritical::retreats() - retreatsBefore;
    if (result.coveredNeighbour) {
        MCC_ReleaseRawData(at(neighbour), n);
    }
    MCC_ReleaseRawData(at(first[0]), again);
    ZJNICritical::close_selection(ZGenerationId::young);
    std::vector<std::pair<size_t, void*>> extra;
    for (size_t i = 0; i < arrays.size() && extra.size() < Mutator::kRawDataSlots - 1; ++i) {
        if (pageOf(at(i)) != pages[2] && i != first[0] && i != first[1]) {
            extra.emplace_back(i, MCC_AcquireRawData(at(i), nullptr));
        }
    }
    result.holdsOverflow = ZJNICritical::holds(*mutator);
    result.keptOverflow = ZJNICritical::open_selection_for_test(ZGenerationId::young, &pages[2], 1);
    ZJNICritical::close_selection(ZGenerationId::young);
    MCC_ReleaseRawData(at(extra.back().first), extra.back().second);
    extra.pop_back();
    result.keptOverflowReleased = ZJNICritical::open_selection_for_test(ZGenerationId::young, &pages[2], 1);
    ZJNICritical::close_selection(ZGenerationId::young);
    for (const auto& held : extra) {
        MCC_ReleaseRawData(at(held.first), held.second);
    }
    MCC_ReleaseRawData(at(first[0]), r0);
    MCC_ReleaseRawData(at(first[1]), r1);
    // Old generation: until the final scan before the pause, an acquirer on a
    // selected page holds it at once; the final scan then reports that page.
    const uint64_t oldRetreatsBefore = ZJNICritical::retreats();
    result.oldOpenKept = ZJNICritical::open_selection_for_test(ZGenerationId::old, &pages[1], 1);
    void* late = MCC_AcquireRawData(at(first[1]), nullptr);
    std::vector<ZPage*> heldAtFinal;
    bool allAtFinal = true;
    ZJNICritical::finish_selection(ZGenerationId::old, heldAtFinal, allAtFinal);
    result.oldFinalHeld = heldAtFinal.size() == 1 && heldAtFinal[0] == pages[1] && !allAtFinal;
    result.oldRetreats = ZJNICritical::retreats() - oldRetreatsBefore;
    MCC_ReleaseRawData(at(first[1]), late);
    ZJNICritical::close_selection(ZGenerationId::old);
    result.keptAfter = ZJNICritical::open_selection_for_test(ZGenerationId::young, pages.data(), pages.size());
    ZJNICritical::close_selection(ZGenerationId::young);
    result.holdsAfter = ZJNICritical::holds(*mutator);
    return nullptr;
}
}
GC_RUNTIME_OTHER_VM_TEST(ZJNICritical, SelectionScanKeepsExactlyHeldPages)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    RawScanResult result;
    CJThreadHandle task = RunCJTask(RawScanTask, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "RAW_SCAN_TARGET distinct=%d kept_two=%zu kept_covered=%zu covered_retreats=%llu "
        "neighbour=%d holds_overflow=%lld kept_overflow=%zu kept_overflow_released=%zu kept_after=%zu "
        "holds_after=%lld old_open_kept=%zu old_final_held=%d old_retreats=%llu\n", result.distinct, result.keptTwo, result.keptCovered,
        (unsigned long long)result.coveredRetreats, result.coveredNeighbour, (long long)result.holdsOverflow,
        result.keptOverflow, result.keptOverflowReleased, result.keptAfter, (long long)result.holdsAfter,
        result.oldOpenKept, result.oldFinalHeld, (unsigned long long)result.oldRetreats);
    GC_EXPECT_TRUE(result.distinct);
    GC_EXPECT_EQ(result.keptTwo, static_cast<size_t>(2));
    GC_EXPECT_EQ(result.keptCovered, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.coveredRetreats, static_cast<uint64_t>(0));
    GC_EXPECT_TRUE(result.coveredNeighbour);
    GC_EXPECT_EQ(result.holdsOverflow, static_cast<int64_t>(Mutator::kRawDataSlots + 1));
    GC_EXPECT_EQ(result.keptOverflow, static_cast<size_t>(1));
    GC_EXPECT_EQ(result.keptOverflowReleased, static_cast<size_t>(0));
    GC_EXPECT_EQ(result.keptAfter, static_cast<size_t>(0));
    GC_EXPECT_EQ(result.holdsAfter, 0);
    GC_EXPECT_EQ(result.oldOpenKept, static_cast<size_t>(0));
    GC_EXPECT_TRUE(result.oldFinalHeld);
    GC_EXPECT_EQ(result.oldRetreats, static_cast<uint64_t>(0));
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(PageIdentity1315, MarkKeepsPreviousForwarding)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyPreviousMark = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    // Target precedes prerequisites: absence never hides the target verdict.
    GC_EXPECT_EQ(result.markReceipts, result.previousMembers);
    GC_EXPECT_TRUE(result.observedMark);
    GC_EXPECT_TRUE(result.previousMembers > 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(PageIdentity1315, PromoteAllRegistersUniquePages)
{
    RuntimeParam param{};
    param.heapParam.heapSize = 512 * 1024;
    param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ForwardingSelectionResult result;
    result.verifyPromotion = true;
    CJThreadHandle handle = RunCJTask(SelectRealLivePages, &result);
    GC_EXPECT_TRUE(handle != nullptr);
    void* taskResult = nullptr;
    GC_EXPECT_EQ(GetTaskRet(handle, &taskResult), E_OK);
    ReleaseHandle(handle);
    // Target: no page is registered twice (zRelocationSet.cpp:213-228).
    GC_EXPECT_EQ(result.registered, result.registeredDistinct);
    GC_EXPECT_EQ(result.promotedRoots, result.roots);
    GC_EXPECT_TRUE(result.registered > 0);
    GC_EXPECT_TRUE(result.roots > 0);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

#include "gclog_capture.hpp"
// The reused fixture enters through RunCJTask -> MCC_AcquireRawData. The log
// value comes from the product wait scope, not from a manually called timer.
GC_RUNTIME_OTHER_VM_TEST(GcLifecycleLog, JNICriticalStall)
{
    setenv("MRT_GC_LOG", "1", 1);
    GcLogCapture capture;
    ZJNICritical_AcquireOnOpenSelectionWaitsInSaferegion();
    const std::string text = capture.Finish();
    const std::string expected = "name=JNI_Critical_Stall kind=critical ";
    size_t count = 0, position = 0;
    while ((position = text.find(expected, position)) != std::string::npos) { ++count; position += expected.size(); }
    std::fprintf(stderr, "GCLOG_TARGET jni_critical_stall_records=%zu expected=1\n", count);
    GC_EXPECT_EQ(count, size_t{1});
}

namespace {
// Native ABI-consumer test: metadata models CR01's class return and the
// unchanged Unit constructor ABI. It does not claim compiler-produced metadata
// or a managed constructor/exception path has been exercised.
struct ReflectedConstructorResult {
    bool returnsReceiver = false;
    bool called = false;
    uintptr_t original = 0;
    uintptr_t replacement = 0;
    uintptr_t returned = 0;
    U64 marker = 0;
    size_t rootsBefore = 0;
    size_t rootsAfter = 0;
};
thread_local ReflectedConstructorResult* reflectedConstructorResult;
extern "C" MObject* ForwardReflectedReceiver(MObject* receiver, TypeInfo* type)
{
    auto& result = *reflectedConstructorResult;
    result.called = true;
    auto* mutator = Mutator::GetMutator();
    HandleMark mark(*mutator);
    Handle original(mutator, receiver);
    receiver->Store<U64>(TYPEINFO_PTR_SIZE, 101);
    Handle replacement(mutator, MCC_NewObject(type, TYPEINFO_PTR_SIZE + type->GetInstanceSize()));
    static_cast<MObject*>(replacement())->Store<U64>(TYPEINFO_PTR_SIZE, 202);
    CollectSparsePages();
    result.original = reinterpret_cast<uintptr_t>(original());
    result.replacement = reinterpret_cast<uintptr_t>(replacement());
    return static_cast<MObject*>(replacement());
}
void* RunReflectedConstructor(void* context)
{
    auto& result = *static_cast<ReflectedConstructorResult*>(context);
    reflectedConstructorResult = &result;
    auto* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    alignas(TypeInfo) static unsigned char storage[2][sizeof(TypeInfo)]{};
    auto* klass = reinterpret_cast<TypeInfo*>(storage[0]);
    auto* unit = reinterpret_cast<TypeInfo*>(storage[1]);
    for (auto& item : storage) {
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(item), sizeof(TypeInfo));
    }
    klass->SetType(TypeKind::TYPE_KIND_CLASS); klass->SetInstanceSize(sizeof(U64));
    unit->SetType(TypeKind::TYPE_KIND_UNIT); unit->SetInstanceSize(0);
    MethodInfo method{};
    method.SetMethodName("init");
    method.SetDeclaringTypeInfo(klass);
    // Existing emitted MethodInfo layout: entry at16, return TI at24. The only
    // varying metadata input is the actual return type, not a test-only flag.
    const Uptr entry = reinterpret_cast<Uptr>(&ForwardReflectedReceiver);
    TypeInfo* returnType = result.returnsReceiver ? klass : unit;
    std::memcpy(reinterpret_cast<char*>(&method) + 16, &entry, sizeof(entry));
    std::memcpy(reinterpret_cast<char*>(&method) + 24, &returnType, sizeof(returnType));
    result.rootsBefore = mutator->NativeFrameRootCount();
    auto* value = static_cast<MObject*>(MCC_ApplyCJStaticMethod(&method, nullptr, nullptr));
    result.rootsAfter = mutator->NativeFrameRootCount();
    result.returned = reinterpret_cast<uintptr_t>(value);
    result.marker = value == nullptr ? 0 : value->Load<U64>(TYPEINFO_PTR_SIZE);
    return nullptr;
}
void CheckReflectedConstructor(bool returnsReceiver)
{
    RuntimeParam param{}; param.heapParam.heapSize = 512 * 1024; param.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    ReflectedConstructorResult result; result.returnsReceiver = returnsReceiver;
    auto task = RunCJTask(RunReflectedConstructor, &result);
    void* value = nullptr;
    GC_EXPECT_EQ(GetTaskRet(task, &value), E_OK);
    ReleaseHandle(task);
    std::fprintf(stderr, "REFLECTION_CTOR_TARGET executed=%d ref_abi=%d original=%zx replacement=%zx returned=%zx marker=%llu roots=%zu/%zu\n",
        result.called, returnsReceiver, result.original, result.replacement, result.returned,
        static_cast<unsigned long long>(result.marker), result.rootsBefore, result.rootsAfter);
    GC_EXPECT_TRUE(result.called && result.original != 0 && result.replacement != 0 && result.original != result.replacement);
    GC_EXPECT_EQ(result.returned, returnsReceiver ? result.replacement : result.original);
    GC_EXPECT_EQ(result.marker, returnsReceiver ? U64(202) : U64(101));
    GC_EXPECT_EQ(result.rootsBefore, result.rootsAfter);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}
GC_RUNTIME_OTHER_VM_TEST(ReflectedConstructor, ClassReturnForwardsReceiver)
{
    CheckReflectedConstructor(true);
}
GC_RUNTIME_OTHER_VM_TEST(ReflectedConstructor, UnitAbiIgnoresRegisterResult)
{
    CheckReflectedConstructor(false);
}
