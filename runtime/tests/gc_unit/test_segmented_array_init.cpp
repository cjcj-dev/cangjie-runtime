// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <memory>
#include "Heap/z/zWorkers.hpp"
#if defined(__linux__)
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "gc_unittest.hpp"

#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
extern "C" void CJ_ScheduleAllCJThreadVisit(void (*visitor)(void*, void*), void* handle);
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zMarkPartialArray.hpp"
#include "Heap/z/zIterator.hpp"
#include "Heap/z/zHeapIterator.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"
#include "Heap/z/zMarkStack.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zBarrier.hpp"

namespace MapleRuntime {
extern "C" ArrayRef MCC_NewObjArray(const TypeInfo* arrayInfo, MIndex nElems);
extern "C" ArrayRef MCC_NewArray8(const TypeInfo* arrayInfo, MIndex nElems);
}

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
struct ReferenceArrayTypeInfos {
    ReferenceArrayTypeInfos()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_CLASS);
        component->SetInstanceSize(sizeof(void*));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);

        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

ReferenceArrayTypeInfos& GetReferenceArrayTypeInfos()
{
    static ReferenceArrayTypeInfos infos;
    return infos;
}

struct ByteArrayTypeInfos {
    ByteArrayTypeInfos()
    {
        std::memset(componentStorage, 0, sizeof(componentStorage));
        component = reinterpret_cast<TypeInfo*>(componentStorage);
        component->SetType(TypeKind::TYPE_KIND_UINT8);
        component->SetInstanceSize(sizeof(uint8_t));

        std::memset(arrayStorage, 0, sizeof(arrayStorage));
        array = reinterpret_cast<TypeInfo*>(arrayStorage);
        array->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        array->SetComponentTypeInfo(component);

        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(this), sizeof(*this));
    }

    alignas(TypeInfo) unsigned char componentStorage[sizeof(TypeInfo)];
    alignas(TypeInfo) unsigned char arrayStorage[sizeof(TypeInfo)];
    TypeInfo* component = nullptr;
    TypeInfo* array = nullptr;
};

ByteArrayTypeInfos& GetByteArrayTypeInfos()
{
    static ByteArrayTypeInfos infos;
    return infos;
}


constexpr MIndex kLargeRefLength = 1024 * 1024;

void* RunArrayCase(void* argument)
{
    const uintptr_t mode = reinterpret_cast<uintptr_t>(argument);
    const bool primitive = (mode & 1) != 0;
    const bool small = (mode & 2) != 0;
    const bool young = (mode & 4) != 0;
    const bool full = (mode & 8) != 0;
    const bool twice = (mode & 16) != 0;
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    if (young || full) {
        setenv("MRT_GC_UNIT_MANAGED_SEGMENTED", young ? "young" : twice ? "full2" : "full", 1);
    } else {
        unsetenv("MRT_GC_UNIT_MANAGED_SEGMENTED");
    }
    const MIndex length = small ? 16 : kLargeRefLength;
    auto& heap = Heap::GetHeap();
    const ZGenerationId generation = young ? ZGenerationId::young : ZGenerationId::old;
    const uint64_t before = heap.GetCycleSnapshot(generation).sequence;
    MArray* array = primitive ? MCC_NewArray8(GetByteArrayTypeInfos().array, length) :
                               MCC_NewObjArray(GetReferenceArrayTypeInfos().array, length);
    unsetenv("MRT_GC_UNIT_MANAGED_SEGMENTED");
    if (array == nullptr) { return reinterpret_cast<void*>(1); }
    size_t nonzero = 0;
    for (size_t i = 0; i < array->GetContentSize(); ++i) {
        nonzero += array->ConvertToCArray()[i] != 0;
    }
    const bool published = !array->IsInvisibleObject() && mutator->LoadInvisibleRoot() == nullptr;
    const uint64_t after = heap.GetCycleSnapshot(generation).sequence;
    const bool gc = !(young || full) || after > before;
    const bool lengthValid = array->GetLength() == length;
    std::fprintf(stderr, "SEGMENTED_RESULT_TARGET mode=%zu size=%zu length=%d published=%d nonzero=%zu before=%llu after=%llu gc=%d\n",
                 mode, array->GetContentSize(), lengthValid, published, nonzero,
                 (unsigned long long)before, (unsigned long long)after, gc);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>((nonzero == 0 && published && lengthValid && gc) ? 0 : 2);
}

void* RunLargePageIdentityCase(void*)
{
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    ZPage* page = array == nullptr ? nullptr : Heap::page(reinterpret_cast<uintptr_t>(array));
    const bool valid = page != nullptr && page->is_large() && page->age() == PageAge::eden;
    std::fprintf(stderr, "LARGE_PAGE_IDENTITY_TARGET eden_large=%d\n", valid);
    return reinterpret_cast<void*>(valid ? 0 : 1);
}

// ZGC zGeneration.cpp:216-258: empty pages leave the page table before
// not-selected live pages are aged. Only the export root keeps the second
// large page alive; native local values are not managed roots.
void* RunYoungSelectionLifetimeCase(void*)
{
    auto& heap = Heap::GetHeap();
    MArray* dead = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    MArray* live = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (dead == nullptr || live == nullptr) {
        return reinterpret_cast<void*>(10);
    }
    const uintptr_t deadAddress = reinterpret_cast<uintptr_t>(dead);
    const uintptr_t liveAddress = reinterpret_cast<uintptr_t>(live);
    ZPage* const deadPage = Heap::page(deadAddress);
    ZPage* const livePage = Heap::page(liveAddress);
    const bool distinctYoungPages = deadPage != livePage && deadPage->IsYoungRegion() &&
        livePage->IsYoungRegion() && deadPage->is_large() && livePage->is_large();
    const PageAge beforeAge = livePage->age();
    const U64 root = heap.RegisterExportRoot(live);
    Mutator::GetMutator()->SetManagedContext(false);
    heap.RequestGC(GC_REASON_YOUNG, false);
    // Read through the page table, never through either saved descriptor.
    const bool emptyReleased = Heap::page(deadAddress) == nullptr;
    BaseObject* const survivor = heap.GetExportObject(root);
    ZPage* const current = survivor == nullptr ? nullptr : Heap::page(reinterpret_cast<uintptr_t>(survivor));
    const bool liveProcessed = current != nullptr && current->age() != beforeAge &&
        reinterpret_cast<uintptr_t>(survivor) == liveAddress;
    std::fprintf(stderr,
        "YOUNG_SELECTION_LIFETIME_TARGET distinct_young_pages=%d empty_released=%d live_processed=%d\n",
        distinctYoungPages, emptyReleased, liveProcessed);
    heap.RemoveExportObject(root);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>((distinctYoungPages && emptyReleased && liveProcessed) ? 0 : 1);
}

// ZGC zGeneration.cpp:180-194 and zRelocate.cpp:1384-1387. A large page
// stays out of the relocation set; its raw null fields must survive promotion
// as colored nulls. Read the product result before any mutator load barrier.
void* RunFlipPromotionCase(void* argument)
{
    const bool promote = reinterpret_cast<uintptr_t>(argument) == 0;
    auto& heap = Heap::GetHeap();
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (array == nullptr) {
        return reinterpret_cast<void*>(10);
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(array);
    const U64 root = heap.RegisterExportRoot(array);
    auto* fields = reinterpret_cast<RefField<>*>(array->ConvertToCArray());
    const zpointer before = fields[0].GetFieldValue();
    // A non-null field is already colored by marking, independently of the
    // promotion barrier. It is the positive control in the debugger test.
    auto& selfField = array->GetRefField(reinterpret_cast<MAddress>(fields + 1) - address);
    ZBarrier::WriteReference(array, selfField, array);
    Mutator::GetMutator()->SetManagedContext(false);
    heap.RequestGC(promote ? GC_REASON_USER : GC_REASON_YOUNG, false);
    array = static_cast<MArray*>(heap.GetExportObject(root));
    fields = reinterpret_cast<RefField<>*>(array->ConvertToCArray());
    const zpointer after = fields[0].GetFieldValue();
    const bool sameAddress = reinterpret_cast<uintptr_t>(array) == address;
    const bool old = !Heap::page(address)->IsYoungRegion();
    // The full major cycle has since flipped old relocate colors (ZGC
    // zAddress.cpp:149-152). The strict store-good assertion belongs before
    // relocate start and is made by test_flip_promotion_gdb.py.
    const bool target = promote ? (raw(after) != 0 && is_null_any(after)) : raw(after) == raw(before);
    std::fprintf(stderr,
        "FLIP_PROMOTION_TARGET promote=%d same_address=%d old=%d before=%zx after=%zx store_good=%d target=%d\n",
        promote, sameAddress, old, raw(before), raw(after), ZPointer::is_store_good(after), target);
    heap.RemoveExportObject(root);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>((sameAddress && old == promote && target) ? 0 : 1);
}

// ZGC zGeneration.cpp:1058-1063 and zStat.cpp:1789-1798:
// an old collection publishes selector liveness before relocation starts.
void* RunOldRelocationStatisticsCase(void*)
{
    auto& heap = Heap::GetHeap();
    auto* mutator = Mutator::GetMutator();
    MArray* survivor = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (survivor == nullptr) {
        return reinterpret_cast<void*>(10);
    }
    const size_t minimumLive = survivor->GetContentSize();
    const U64 root = heap.RegisterExportRoot(survivor);
    mutator->SetManagedContext(false);
    heap.RequestGC(GC_REASON_USER, false);
    auto* current = heap.GetExportObject(root);
    const bool retainedOld = current != nullptr &&
        !Heap::page(reinterpret_cast<uintptr_t>(current))->IsYoungRegion();
    const auto input = ZGeneration::old()->StatHeap()->Stats();
    const size_t live = input.liveAtMarkEnd;
    // Report both independently: a setup check must not hide the live assertion.
    const bool liveAccount = live >= minimumLive;
    std::fprintf(stderr,
        "OLD_RELOCATION_STATS_TARGET retained_old=%d live=%zu minimum_live=%zu live_account=%d\n",
        retainedOld, live, minimumLive, liveAccount);
    heap.RemoveExportObject(root);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>((retainedOld && liveAccount) ? 0 : 1);
}

void* RunOrdinaryBirthCase(void*)
{
    auto& heap = Heap::GetHeap();
    auto* mutator = Mutator::GetMutator();
    TypeInfo* type = GetReferenceArrayTypeInfos().component;
    const size_t size = AlignUp(type->GetInstanceSize() + TYPEINFO_PTR_SIZE, size_t{8});
    MObject* dead = MObject::NewPinnedObject(type, size);
    MObject* survivor = MObject::NewPinnedObject(type, size);
    ZPage* oldPage = Heap::page(reinterpret_cast<uintptr_t>(dead));
    const bool shared = Heap::page(reinterpret_cast<uintptr_t>(survivor)) == oldPage;
    const U64 root = heap.RegisterExportRoot(survivor);
    mutator->SetManagedContext(false);
    heap.RequestGC(GC_REASON_USER, false);
    mutator->SetManagedContext(true);
    MObject* fresh = MObject::NewPinnedObject(type, size);
    ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(fresh));
    // ZGC zGeneration.cpp:205-259 permits relocation; reload the live object
    // through its root before checking that fresh eden allocation is separate.
    BaseObject* currentSurvivor = heap.GetExportObject(root);
    const bool fromNewPage = currentSurvivor != nullptr &&
        page != Heap::page(reinterpret_cast<uintptr_t>(currentSurvivor));
    const bool noExplicitMark = !page->is_marked();
    const bool allocating = page->IsAllocating();
    const bool retained = currentSurvivor != nullptr && currentSurvivor->GetTypeInfo() == type &&
        currentSurvivor->GetSize() == size;
    std::fprintf(stderr, "P1_ORDINARY_ASSERT_EXECUTED shared=%d new_page=%d allocating=%d no_bitmap=%d retained=%d\n",
                 shared, fromNewPage, allocating, noExplicitMark, retained);
    heap.RemoveExportObject(root);
    return reinterpret_cast<void*>((shared && fromNewPage && allocating && noExplicitMark && retained) ? 0 : 1);
}

void* RunVisibleArrayGraph(void*)
{
    Mutator::GetMutator()->SetManagedContext(false);
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    NativeSlot root(zpointer::null);
    ZBarrier::WriteStaticRef(root, array);
    NativeSlot* roots[] = { &root };
    Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::vector<size_t> visits(array->GetLength(), 0);
    size_t invalid = 0;
    size_t objects = 0;
    const MAddress first = reinterpret_cast<MAddress>(array->ConvertToCArray());
    {
        ScopedEnterSaferegion saferegion(false);
        ScopedStopTheWorld stw("segmented-array range graph", false);
        HeapIterator(false).Iterate([&](BaseObject* object) { objects += object == array; },
            [&](BaseObject* base, const void* slot, uintptr_t) {
                if (base != array) { return; }
                const MAddress field = reinterpret_cast<MAddress>(slot);
                if (field < first || (field - first) % sizeof(RefField<>) != 0 ||
                    (field - first) / sizeof(RefField<>) >= visits.size()) {
                    ++invalid;
                } else {
                    ++visits[(field - first) / sizeof(RefField<>)];
                }
            });
    }
    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    for (size_t count : visits) { invalid += count != 1; }
    const bool complete = objects == 1 && invalid == 0;
    std::fprintf(stderr, "SEGMENTED_GRAPH_RANGE_ASSERT objects=%zu fields=%zu invalid=%zu pass=%d\n",
                 objects, visits.size(), invalid, complete);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>(complete ? 0 : 1);
}

#if defined(MRT_TESTABLE_INTERNALS)
// Phase-unit fixture: stop after the real concurrent mark result, before
// relocation can replace pages. ZGC zGeneration.cpp:538-553,665-669.
// DriverLocker excludes automatic collections while the phase is inspected.
class YoungMarkPhase {
    DriverLocker locker;
    YoungTypeSetter type{Heap::GetHeap().young(), ZYoungType::minor};
public:
    YoungMarkPhase()
    {
        // This native task is itself a mutator. Publish its safe state before
        // requesting the product pause, as the real GC-thread caller does.
        ScopedEnterSaferegion safe(false);
        auto& young = Heap::GetHeap().young();
        young.PreGarbageCollection(true, young.Snapshot().requestIndex);
        young.pause_mark_start();
        young.concurrent_mark();
    }
    ~YoungMarkPhase()
    {
        auto& young = Heap::GetHeap().young();
        young.Workers()->set_inactive();
        young.End();
    }
};

// The only paths to the two targets are fields of a real allocated object.
// Read the product young-mark livemap before relocation; no test callback is
// installed in the iterator and no intermediate mark result is supplied.
void* RunConcreteFieldYoungMark(void*)
{
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)]{};
    TypeInfo* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(3 * sizeof(void*));
    type->SetFlagHasRefField();
    GCTib tib{};
    tib.tag = SIGN_BIT | 5;
    type->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    MObject* holder = MObject::NewPinnedObject(type, TYPEINFO_PTR_SIZE + 3 * sizeof(void*));
    MArray* targets[] = {
        MCC_NewArray8(GetByteArrayTypeInfos().array, 16),
        MCC_NewArray8(GetByteArrayTypeInfos().array, 16)
    };
    for (size_t i = 0; i < 2; ++i) {
        auto& field = holder->GetRefField(TYPEINFO_PTR_SIZE + 2 * i * sizeof(void*));
        ZBarrier::WriteReference(holder, field, targets[i]);
    }
    auto& heap = Heap::GetHeap();
    const U64 root = heap.RegisterExportRoot(holder);
    Mutator::GetMutator()->SetManagedContext(false);
    unsigned liveMask = 0;
    bool holderLive = false;
    {
        YoungMarkPhase phase;
        holderLive = Heap::page(reinterpret_cast<uintptr_t>(holder))->is_object_strongly_live(from_object(holder));
        for (size_t i = 0; i < 2; ++i) {
            if (Heap::page(reinterpret_cast<uintptr_t>(targets[i]))->is_object_strongly_live(from_object(targets[i]))) {
                liveMask |= 1u << i;
            }
        }
        std::fprintf(stderr, "FIELD_ITERATOR_YOUNG_RESULT holder_live=%d live_mask=%u expected=3\n",
                     holderLive, liveMask);
    }
    heap.RemoveExportObject(root);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>(holderLive && liveMask == 3 ? 0 : 1);
}

void* RunLargeYoungClosureCase(void*)
{
    // Construct the sole strong path before requesting GC. No unregistered
    // C++ reference is used to publish a target after marking has begun.
    MArray* holder = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    MArray* target = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    auto& field = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder->ConvertToCArray()));
    ZBarrier::WriteReference(holder, field, target);
    const bool holderYoung = Heap::page(reinterpret_cast<uintptr_t>(holder))->IsYoungRegion();
    const U64 holderRoot = Heap::GetHeap().RegisterExportRoot(holder);
    Mutator* mutator = Mutator::GetMutator();
    mutator->SetManagedContext(false);
    bool live;
    bool followed;
    {
        YoungMarkPhase phase;
        live = Heap::page(reinterpret_cast<uintptr_t>(target))->is_object_strongly_live(from_object(target));
        followed = live && to_object(field.GetTargetObject()) == target;
        std::fprintf(stderr, "LARGE_YOUNG_TARGET_LIVE_ASSERT_EXECUTED holder_young=%d live=%d followed=%d\n",
                     holderYoung, live, followed);
    }
    Heap::GetHeap().RemoveExportObject(holderRoot);
    mutator->SetManagedContext(true);
    return reinterpret_cast<void*>((holderYoung && live && followed) ? 0 : 1);
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
void* RunMarkAllocationCase(void* rawExisting)
{
    const bool existing = reinterpret_cast<uintptr_t>(rawExisting) != 0;
    // ZPage::is_object_strongly_live (zPage.inline.hpp:258-260): the page
    // predicate over the product-owned livemap.
    auto productLive = [](ZPage* page, const BaseObject* object) {
        return page->is_object_strongly_live(from_object(object));
    };
    auto& heap = Heap::GetHeap();
    auto& collector = heap;
    Mutator* mutator = Mutator::GetMutator();
    MArray* beforeSmall = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    const U64 beforeSmallRoot = heap.RegisterExportRoot(beforeSmall);
    ZPage* beforeSmallPage = Heap::page(reinterpret_cast<uintptr_t>(beforeSmall));
    MArray* target = existing ? MCC_NewArray8(GetByteArrayTypeInfos().array, 16) : nullptr;
    const U64 targetRoot = target != nullptr ? heap.RegisterExportRoot(target) : 0;
    mutator->SetManagedContext(false);
    auto markPhase = std::make_unique<YoungMarkPhase>();
    MArray* afterSmall = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    ZPage* afterSmallPage = Heap::page(reinterpret_cast<uintptr_t>(afterSmall));
    const bool retiredTLAB = afterSmallPage != beforeSmallPage && afterSmallPage->IsAllocating();
    std::fprintf(stderr, "P1_TLAB_RETIRE_ASSERT_EXECUTED different=%d birth=%llu owner=%llu\n",
                 afterSmallPage != beforeSmallPage,
                 static_cast<unsigned long long>(afterSmallPage->BirthSequence()),
                 static_cast<unsigned long long>(afterSmallPage->generation()->seqnum()));
    MArray* holder = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, kLargeRefLength);
    if (!existing) target = MCC_NewArray8(GetByteArrayTypeInfos().array, 16);
    const U64 holderRoot = heap.RegisterExportRoot(holder);
    auto& field = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder->ConvertToCArray()));
    ZBarrier::WriteReference(holder, field, target);
    ZPage* page = Heap::page(reinterpret_cast<uintptr_t>(holder));
    ZPage* targetPage = Heap::page(reinterpret_cast<uintptr_t>(target));
    const bool implicit = page->IsAllocating();
    const bool live = productLive(page, holder);
    const bool targetLive = productLive(targetPage, target);
    const bool excluded = page->IsAllocating() && !page->IsKnownYoungEmpty();
    auto& productCollector = static_cast<Heap&>(collector);
    ZMark* domain = Heap::GetHeap().young().MarkPtr();
    const size_t pendingBefore = domain->Stripes().Population() + domain->Stacks().Population();
    holder->OnFinalizerCreated();
    const size_t pendingAfter = domain->Stripes().Population() + domain->Stacks().Population();
    const bool noExplicitMark = !page->is_marked();
    const bool noPublication = pendingAfter == pendingBefore;
    std::fprintf(stderr, "P1_NEW_REGISTRATION_ASSERT_EXECUTED birth=%llu owner=%llu no_bitmap=%d "
                 "pending_before=%zu pending_after=%zu\n",
                 static_cast<unsigned long long>(page->BirthSequence()),
                 static_cast<unsigned long long>(page->generation()->seqnum()), noExplicitMark,
                 pendingBefore, pendingAfter);
    const auto during = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young);
    const auto phase = during.phase;
    std::fprintf(stderr, "MARK_ALLOC_TARGET_ASSERT_EXECUTED existing=%d phase=%u young=%d large=%d "
                 "implicit=%d live=%d target_live=%d excluded=%d\n",                   existing, static_cast<unsigned>(phase),
                 page->IsYoungRegion(), page->IsLargeRegion(), implicit, live, targetLive, excluded);
    heap.RemoveExportObject(beforeSmallRoot);
    if (existing) heap.RemoveExportObject(targetRoot);
    mutator->SetManagedContext(false);
    markPhase.reset();
    // The next real driver cycle resamples allocation watermarks and relocates.
    Heap::GetHeap().RequestGC(GC_REASON_YOUNG, false);
    holder = static_cast<MArray*>(heap.GetExportObject(holderRoot));
    page = Heap::page(reinterpret_cast<uintptr_t>(holder));
    auto& completedField = HeapSlotAt<>(reinterpret_cast<uintptr_t>(holder->ConvertToCArray()));
    BaseObject* completedTarget = ZBarrier::ReadReference(holder, completedField);
    // The request includes relocation. Check the actual field result here;
    // the mark-end predicate was observed before relocation at the success exit.
    const bool completedValue = completedTarget != nullptr &&
        static_cast<MArray*>(completedTarget)->GetLength() == 16;
    std::fprintf(stderr, "MARK_ALLOC_COMPLETED_VALUE_ASSERT_EXECUTED length_valid=%d\n", completedValue);
    // zRelocate.cpp:868-874 resets a survivor target or clones a promoted
    // page. A newly initialized target may still be allocating in its domain.
    const bool resampled = page->age() != PageAge::eden &&
                           page->BirthSequence() <= page->generation()->seqnum();
    const auto after = Heap::GetHeap().GetCycleSnapshot(ZGenerationId::young);
    const bool nextCycle = after.sequence > during.sequence;
    std::fprintf(stderr, "MARK_ALLOC_NEXT_CYCLE_ASSERT_EXECUTED before=%llu after=%llu resampled=%d\n",
                 static_cast<unsigned long long>(during.sequence),
                 static_cast<unsigned long long>(after.sequence), resampled);
    heap.RemoveExportObject(holderRoot);
    mutator->SetManagedContext(true);
    const uintptr_t status = (retiredTLAB ? 0 : 1024) | ((noExplicitMark && noPublication) ? 0 : 512) | (implicit ? 0 : 1) | (live ? 0 : 2) |
        (targetLive ? 0 : 4) | (excluded ? 0 : 8) | (nextCycle ? 0 : 16) |
        (resampled ? 0 : 32) |
        (productLive(Heap::page(reinterpret_cast<uintptr_t>(completedTarget)), completedTarget) ? 0 : 128) |
        (completedValue ? 0 : 256) |
        ((phase == ZGenerationPhase::Mark) ? 0 : 64);
    std::fprintf(stderr, "MARK_ALLOC_ASSERT_RESULT status=%zu "
                 "bits=implicit:1,live:2,target_live:4,excluded:8,next_cycle:16,resampled:32,window:64,mark_end_live:128,completed_value:256\n", status);
    return reinterpret_cast<void*>(status);
}
#endif


// Exercise the actual scheduler argument copy and its registered root visitor.
void* RunNativeTaskRootCase(void*)
{
    Mutator::GetMutator()->SetManagedContext(false);
    struct Observation {
        RootSlot* taskRoot = nullptr;
        uintptr_t native = 0;
        size_t tasks = 0;
    } observed;
    CJ_ScheduleAllCJThreadVisit([](void* argument, void* context) {
        auto& result = *static_cast<Observation*>(context);
        auto& data = *static_cast<LWTData*>(argument);
        if (data.fn != nullptr) {
            result.taskRoot = &RootSlotAt(&data.obj);
            result.native = reinterpret_cast<uintptr_t>(data.fn);
            ++result.tasks;
        }
    }, &observed);
    size_t visits = 0;
    size_t nativeRoots = 0;
    RootVisitor visitor = [&](RootSlot& root) {
        visits += &root == observed.taskRoot;
        nativeRoots += observed.native != 0 && raw(root.LoadPlain()) == observed.native;
    };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
    const bool rootsValid = observed.tasks == 1 && visits == 1 && nativeRoots == 0;
    std::fprintf(stderr, "NATIVE_TASK_ROOT_TARGET tasks=%zu obj_visits=%zu native_roots=%zu pass=%d\n",
                 observed.tasks, visits, nativeRoots, rootsValid);
    if (!rootsValid) {
        Mutator::GetMutator()->SetManagedContext(true);
        return reinterpret_cast<void*>(41);
    }
    // Positive managed-object control through the production heap iterator.
    MArray* array = MCC_NewObjArray(GetReferenceArrayTypeInfos().array, 1);
    NativeSlot root(zpointer::null);
    ZBarrier::WriteStaticRef(root, array);
    NativeSlot* roots[] = { &root };
    Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    size_t objects = 0;
    {
        ScopedEnterSaferegion saferegion(false);
        ScopedStopTheWorld stw("native task heap iteration", false);
        HeapIterator(false).Iterate([&](BaseObject* object) { objects += object == array; });
    }
    Heap::GetHeap().RequestGC(GC_REASON_USER, false);
    Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(roots), 1);
    std::fprintf(stderr, "NATIVE_TASK_HEAP_TARGET managed_visits=%zu gc_returned=1\n", objects);
    Mutator::GetMutator()->SetManagedContext(true);
    return reinterpret_cast<void*>(objects == 1 ? 0 : 42);
}

int RunRuntimeCase(CJTaskFunc task, uintptr_t argument, U32 processorCount = 1,
                   bool runtimeThread = false)
{
#if defined(__linux__)
    const pid_t child = fork();
    if (child == 0) {
        (void)setenv("cjProcessorNum", processorCount == 1 ? "1" : "2", 1);
        if (task == RunOldRelocationStatisticsCase) {
            (void)setenv("cjSoftMaxHeapSize", "128MB", 1);
        }
        RuntimeParam param {};
        param.heapParam.heapSize = 512 * 1024;
        param.coParam.processorNum = processorCount;
        if (InitCJRuntime(&param) != E_OK) {
            _exit(100);
        }
        if (runtimeThread) {
            // Use the real native runtime-thread registration for graph tests.
            auto& manager = MutatorManager::Instance();
            manager.CreateRuntimeMutator(ThreadType::GC_THREAD);
            void* result = task(reinterpret_cast<void*>(argument));
            manager.DestroyRuntimeMutator(ThreadType::GC_THREAD);
            const uintptr_t status = reinterpret_cast<uintptr_t>(result);
            if (FiniCJRuntime() != E_OK) { _exit(103); }
            _exit(status > 99 ? 99 : static_cast<int>(status));
        }
        CJThreadHandle handle = RunCJTask(task, reinterpret_cast<void*>(argument));
        if (handle == nullptr) {
            _exit(101);
        }
        void* result = nullptr;
        if (GetTaskRet(handle, &result) != E_OK) {
            _exit(102);
        }
        ReleaseHandle(handle);
        const uintptr_t status = reinterpret_cast<uintptr_t>(result);
        if (FiniCJRuntime() != E_OK) {
            _exit(103);
        }
        _exit(status > 99 ? 99 : static_cast<int>(status));
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return 120;
    }
    return WEXITSTATUS(status);
#else
    (void)task;
    (void)argument;
    (void)processorCount;
    (void)runtimeThread;
    return 0;
#endif
}
} // namespace


GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, SmallReferenceArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 2), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, SmallPrimitiveArrayKeepsFastPath)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 3), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, LargeReferenceArrayPublishesClearedPayload)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 0), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, LargePrimitiveArrayUsesSegmentedClearing)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 1), 0);
}
#if defined(MRT_PRODUCT_TESTABLE_INTERNALS)
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, EpochFlipRestartsAndRewritesPublishedBlock)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 8), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, EpochFlipRestartsAndRewritesPublishedBlockParallel)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 8, 2), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, YoungGcRepairsIncompleteArrayRoot)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 4), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, YoungGcRepairsIncompleteArrayRootParallel)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 4, 2), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, TwoGcReferenceInitializationRestartsOnlyOnce)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 24), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, TwoGcPrimitiveInitializationDoesNotRestart)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 25), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, PrimitivePayloadSurvivesFullGcWindow)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunArrayCase, 9), 0);
}
#endif
GC_RUNTIME_OTHER_VM_TEST(LargePageGeneration, AllocationPublishesYoungEden)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargePageIdentityCase, 0), 0);
}
GC_RUNTIME_OTHER_VM_TEST(P1Mark, OrdinaryAllocationRetiresPageAcrossCollection)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunOrdinaryBirthCase, 0), 0);
}
GC_RUNTIME_OTHER_VM_TEST(SegmentedArrayInit, VisibleArrayGraphUsesRangeChunks)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunVisibleArrayGraph, 0, 1, true), 0);
}
#if defined(MRT_TESTABLE_INTERNALS)
GC_RUNTIME_OTHER_VM_TEST(FieldIteratorRuntime, YoungMarkFollowsBothObjectFields)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunConcreteFieldYoungMark, 0), 0);
}
GC_RUNTIME_OTHER_VM_TEST(MarkAllocation, LargeHolderAndNewTargetAreImplicitlyLive)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunMarkAllocationCase, 0), 0);
}
GC_RUNTIME_OTHER_VM_TEST(MarkAllocation, LargeHolderKeepsRootedExistingTargetLive)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunMarkAllocationCase, 1), 0);
}
GC_RUNTIME_OTHER_VM_TEST(LargePageGeneration, ArrayRootKeepsYoungTargetLive)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunLargeYoungClosureCase, 0), 0);
}
#endif

GC_RUNTIME_OTHER_VM_TEST(OldRelocationStatistics, FullCollectionPublishesLiveInput)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunOldRelocationStatisticsCase, 0, 1, true), 0);
}

GC_RUNTIME_OTHER_VM_TEST(NativeTaskRoots, RunCJTaskKeepsNativeContextOutOfRoots)

{
    // InitCJRuntime must construct the heap with this runtime's parameters.
    GC_EXPECT_TRUE(ZCollectedHeap::heap() == nullptr);
    GC_EXPECT_EQ(RunRuntimeCase(RunNativeTaskRootCase, 0), 0);
}

GC_RUNTIME_OTHER_VM_TEST(YoungSelectionLifetime, EmptyReleasedAndLivePageAged)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunYoungSelectionLifetimeCase, 0, 1, true), 0);
}

GC_RUNTIME_OTHER_VM_TEST(FlipPromotion, NullFieldsStayColoredThroughCollection)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunFlipPromotionCase, 0, 1, true), 0);
}
GC_RUNTIME_OTHER_VM_TEST(FlipPromotion, SurvivingYoungFieldsAreNotRewritten)
{
    GC_EXPECT_EQ(RunRuntimeCase(RunFlipPromotionCase, 1, 1, true), 0);
}
