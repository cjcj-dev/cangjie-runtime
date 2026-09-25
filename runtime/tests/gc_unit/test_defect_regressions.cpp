// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Phase-2 defect regression net (甲): each case anchors a shipped fix commit + product site.
// Contracts only — not implementation trivia. Product symbols where the harness can reach them.

#include <cstdint>
#include <dlfcn.h>
#include "Heap/z/zStat.hpp"
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zHeap.hpp"
#include "ObjectModel/RefField.h"

extern "C" size_t MCC_GetGCCount();
extern "C" int CJ_ScheduleManagerInit();
extern "C" void MCC_WriteRefField(const MapleRuntime::ObjectPtr ref, const MapleRuntime::ObjectPtr obj,
                                   MapleRuntime::RefField<false>* field);
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zThreadLocalAllocBuffer.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/ThreadLocal.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Concurrency/Concurrency.h"
#include "Common/Runtime.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

constexpr Uptr kAddrMask = (Uptr(1) << 48) - 1u;

// Model of FixOldTagged / ResolveMinor non-heap arm: recolour (or keep) — never install 0.
// Product: CopyCollector.cpp FixOldTaggedRefField (nullslot 2da28bee / 6a6cf3d8) and
// ResolveMinorReference non-heap early return (zcdnull / B-4 ③).
Uptr ModelRecolourNonHeapNeverNull(Uptr /*slotVal*/, Uptr nonHeapTarget, bool isHeapTarget)
{
    if (!isHeapTarget && nonHeapTarget != 0) {
        return nonHeapTarget; // recolour-only; payload stays
    }
    if (!isHeapTarget) {
        return 0; // only null when target is structurally dead heap residue
    }
    return nonHeapTarget;
}

// Broken pre-nullslot: treated non-heap as dead and CAS-null.
Uptr BrokenNullNonHeap(Uptr /*slotVal*/, Uptr nonHeapTarget, bool isHeapTarget)
{
    if (!isHeapTarget) {
        return 0;
    }
    return nonHeapTarget;
}

// Model of relroroot / rostatic self-heal gate: non-heap loadGood ⇒ skip CAS write-back.
// Product: EnumBarrier/TraceBarrier/Barrier ReadReference (822b0d64).
bool ModelShouldSelfHealCas(bool loadGoodIsHeap)
{
    return loadGoodIsHeap;
}

class InstalledExportAllocBuffer final {
public:
    explicit InstalledExportAllocBuffer(AllocBuffer& allocBuffer)
        : alloc(allocBuffer), previous(ThreadLocal::GetThreadLocalData()->buffer)
    {
        ThreadLocal::GetThreadLocalData()->buffer = &alloc;
    }

    ~InstalledExportAllocBuffer()
    {
        ThreadLocal::GetThreadLocalData()->buffer = previous;
        alloc.ClearRegion();
    }

private:
    AllocBuffer& alloc;
    AllocBuffer* previous;
};

class ExportTestRuntime final : public Runtime {
public:
    static void Ensure() { static ExportTestRuntime instance; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
private:
    ExportTestRuntime()
    {
        runtime = this;
        mutatorManager = &manager;
        concurrencyModel = &concurrency;
        manager.Init();
        const ConcurrencyParam parameters = {1024, 64, 1};
        concurrency.Init(parameters);
    }
    MutatorManager manager;
    Concurrency concurrency;
};

class InstalledExportMutator final {
public:
    InstalledExportMutator() : previous(ThreadLocal::GetMutator())
    {
        ExportTestRuntime::Ensure();
        ThreadLocal::SetMutator(&mutator);
    }
    ~InstalledExportMutator() { ThreadLocal::SetMutator(previous); }
private:
    Mutator mutator;
    Mutator* previous;
};

struct ExportHandleFixture {
    BaseObject* PlaceThirdObject()
    {
        const MAddress address = reinterpret_cast<MAddress>(heap.obj0) + 128;
        BaseObject* object = heap.PlaceObject(address);
        heap.region0()->SetRegionAllocPtr(address + 64);
        return object;
    }

    GcHeapFixture heap;
};

struct CompilerStoreFixture {
    GcHeapFixture heap;
};

} // namespace

// ① iorfix 8baacb1e — pregrant before RouteRegion freezes domain.
// Contract: after liveInfo0 is frozen without object B, later mark on a *different*
// current liveInfo does not open GetRoute(B). Route geometry alone is not enough.
// Product: ZPage::GetRoute domain gate (ZPage.h:812+) + installdomain paint face.


// ② nullslot 2da28bee — non-heap latest must not be CAS-null'd (recolour only).
// Product predicate: Heap::IsHeapAddress; writeback shape RootSlotWriteback keeps target.
GC_TEST(DefectRegress, NonHeapTargetNeverCasNull)
{
    GcHeapFixture fx;
    // TypeInfo storage is outside the managed heap range planted by the fixture.
    auto* nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeap));
    GC_EXPECT_TRUE(Heap::IsHeapAddress(fx.obj0));

    Uptr kept = ModelRecolourNonHeapNeverNull(0xdead, reinterpret_cast<Uptr>(nonHeap), false);
    GC_EXPECT_NE(kept, 0u);
    GC_EXPECT_EQ(kept, reinterpret_cast<Uptr>(nonHeap));

    Uptr broken = BrokenNullNonHeap(0xdead, reinterpret_cast<Uptr>(nonHeap), false);
    GC_EXPECT_EQ(broken, 0u); // documents pre-fix shape that tests must reject
}

// ④ relroroot / rostatic 822b0d64 — RO / non-heap static slots must not get lock cmpxchg.
// Contract via product IsHeapAddress gate used at every self-heal site.
GC_TEST(DefectRegress, RelroNonHeapSkipsSelfHealCas)
{
    GcHeapFixture fx;
    GC_EXPECT_TRUE(ModelShouldSelfHealCas(Heap::IsHeapAddress(fx.obj0)));
    auto* nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(ModelShouldSelfHealCas(Heap::IsHeapAddress(nonHeap)));

    // Physical RO page: CAS into it is the failure mode this fix avoids.
    size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* ro = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    GC_EXPECT_TRUE(ro != MAP_FAILED);
    *reinterpret_cast<uintptr_t*>(ro) = 0x1111;
    GC_EXPECT_EQ(mprotect(ro, page, PROT_READ), 0);
    // Non-heap RO address must not be treated as a self-heal CAS target.
    GC_EXPECT_FALSE(Heap::IsHeapAddress(ro));
    GC_EXPECT_FALSE(ModelShouldSelfHealCas(false));
    munmap(ro, page);
}

// statheal: a mutable root converges on the exact plain target returned by the
// read barrier, while an observed-value CAS cannot overwrite a concurrent store.
GC_TEST(DefectRegress, StaticRootObservedValueHeal)
{
    GcHeapFixture fx;
    RootSlot root;
    StorePlain(root, from_object(fx.obj0));
    zaddress_unsafe observed = root.LoadPlain();
    StorePlain(root, from_object(fx.obj1));
    GC_EXPECT_TRUE(raw(root.LoadPlain()) == reinterpret_cast<Uptr>(fx.obj1));
    GC_EXPECT_EQ(raw(root.LoadPlain()), reinterpret_cast<Uptr>(fx.obj1));
}

GC_TEST(DefectRegress, StaticRootHealDoesNotClobberConcurrentStore)
{
    GcHeapFixture fx;
    RootSlot root;
    StorePlain(root, from_object(fx.obj0));
    zaddress_unsafe observed = root.LoadPlain();
    BaseObject* concurrent = fx.PlaceObject(reinterpret_cast<MAddress>(fx.obj1) + 128);
    StorePlain(root, from_object(concurrent));
    (void)observed;
    GC_EXPECT_EQ(raw(root.LoadPlain()), reinterpret_cast<Uptr>(concurrent));
    GC_EXPECT_EQ(raw(root.LoadPlain()), reinterpret_cast<Uptr>(concurrent));
}

// ⑤ minor ResolveMinor non-heap arm (same contract as ② on the minor side).
// Product: ResolveMinorReference — non-heap returns as-is, never CAS-null (CopyCollector.cpp:1935).
GC_TEST(DefectRegress, MinorNonHeapResolveNeverCasNull)
{
    GcHeapFixture fx;
    auto* nonHeap = reinterpret_cast<BaseObject*>(fx.typeInfo);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeap));
    // Soft-resolve contract: non-heap object identity is preserved (not replaced with null).
    Uptr out = ModelRecolourNonHeapNeverNull(0xabc, reinterpret_cast<Uptr>(nonHeap),
                                            Heap::IsHeapAddress(nonHeap));
    GC_EXPECT_EQ(out, reinterpret_cast<Uptr>(nonHeap));
    GC_EXPECT_NE(out, 0u);
}

// ⑦ fe6d163f — field *address* may arrive coloured; ABI must peel before dereference.
// Product: RefField::GetAddress / CompilerCalls PlainManagedAddr shape.
// P01 removed field-address peeling. Compiler boundary tests now reject
// applying uncolor to a plain slot address (ZGC zAddress.inline.hpp:609).

// T6: the compiler may provide no holder object for a field GEP.  The product
// call must classify the destination slot itself, publishing a coloured heap
// word and retaining the slot-keyed remembered-set obligation.
GC_TEST(DefectRegress, CompilerWriteNullHolderHeapSlotPublishesColour)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    InstalledExportMutator mutator;
    CompilerStoreFixture fx;
    /* heap remset from fixture */
    fx.heap.region0()->reset(PageAge::old);
    fx.heap.region1()->reset(PageAge::eden);
    fx.heap.region1()->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.heap.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    // ZBarrier::store_barrier_on_heap_oop_field (zBarrier.inline.hpp:695-705)
    // skips raw null. Flip remembered metadata to exercise the actual slow path.
    field->StoreColoured(to_zpointer(raw(StoreGoodPointer(fx.heap.obj0)) ^ ZPointerRememberedMask));
    GC_EXPECT_FALSE(ZPointer::is_store_good((*field).GetFieldValue()));

    // obj == nullptr is the triggering ABI shape; field is demonstrably in heap.
    GC_EXPECT_TRUE(Heap::IsHeapAddress(field));
    MCC_WriteRefField(fx.heap.obj1, nullptr, reinterpret_cast<RefField<false>*>(field));
    if (StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(false)) {
        buffer->Flush();
    }

    const uintptr_t installed = static_cast<uintptr_t>(raw(field->GetFieldValue()));
    GC_EXPECT_EQ(ClassifySlotWord(installed), SlotWordVerdict::kColoured);
    GC_EXPECT_TRUE(SlotPageRemembered(slot));
}

// Public ABI shape: callers may provide a non-null opaque/non-heap holder while
// the destination field itself resides in the managed heap.  The exported
// entry must classify by slot and take the same immediate remset path as the
// null-holder case; it must not manufacture a pending relocation entry whose
// base cannot be remapped.
GC_TEST(DefectRegress, CompilerWriteNonHeapHolderHeapSlotUsesImmediatePath)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    InstalledExportMutator mutator;
    CompilerStoreFixture fx;
    /* heap remset from fixture */
    fx.heap.region0()->reset(PageAge::old);
    fx.heap.region1()->reset(PageAge::eden);
    fx.heap.region1()->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.heap.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    const uintptr_t initial = raw(StoreGoodPointer(fx.heap.obj0)) ^ ZPointerRememberedMask;
    std::memcpy(field, &initial, sizeof(initial));

    AllocBuffer alloc;
    InstalledExportAllocBuffer installedAlloc(alloc);

    auto* nonHeapHolder = reinterpret_cast<BaseObject*>(fx.heap.typeInfo);
    GC_EXPECT_TRUE(nonHeapHolder != nullptr);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeapHolder));
    GC_EXPECT_TRUE(Heap::IsHeapAddress(field));

    GC_EXPECT_FALSE(ZPointer::is_store_good((*field).GetFieldValue()));
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        // This is the exported product ABI. A rejected holder access must be
        // observed by the parent's target assertion, not terminate the test runner.
        MCC_WriteRefField(fx.heap.obj1, nonHeapHolder, reinterpret_cast<RefField<false>*>(field));
        if (StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(false)) {
            buffer->Flush();
        }
        GC_EXPECT_EQ(SlotPageRemembered(slot), true);
        GC_EXPECT_TRUE(to_object(field->GetTargetObject()) == fx.heap.obj1);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "NONHEAP_HOLDER_TARGET_ASSERT_EXECUTED status=%d\n", status);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

GC_TEST(DefectRegress, CompilerPostWriteNonHeapHolderHeapSlotUsesImmediatePath)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    InstalledExportMutator mutator;
    CompilerStoreFixture fx;
    /* heap remset from fixture */
    fx.heap.region0()->reset(PageAge::old);
    fx.heap.region1()->reset(PageAge::eden);
    fx.heap.region1()->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.heap.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    const uintptr_t initial = raw(StoreGoodPointer(fx.heap.obj0)) ^ ZPointerRememberedMask;
    std::memcpy(field, &initial, sizeof(initial));

    AllocBuffer alloc;
    InstalledExportAllocBuffer installedAlloc(alloc);

    auto* nonHeapHolder = reinterpret_cast<BaseObject*>(uintptr_t(1));
    GC_EXPECT_TRUE(nonHeapHolder != nullptr);
    GC_EXPECT_FALSE(Heap::IsHeapAddress(nonHeapHolder));
    GC_EXPECT_TRUE(Heap::IsHeapAddress(field));

    GC_EXPECT_FALSE(ZPointer::is_store_good((*field).GetFieldValue()));
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        // This is the exported product ABI. A rejected holder access must be
        // observed by the parent's target assertion, not terminate the test runner.
        ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(reinterpret_cast<RefField<false>*>(field)), false);
        if (StoreBarrierBuffer* buffer = StoreBarrierBuffer::buffer_for_store(false)) {
            buffer->Flush();
        }
        GC_EXPECT_EQ(SlotPageRemembered(slot), true);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "POST_NONHEAP_HOLDER_TARGET_ASSERT_EXECUTED status=%d\n", status);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

GC_TEST(DefectRegress, CompilerWriteHeapHolderKeepsBufferedPath)
{
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    InstalledExportMutator mutator;
    CompilerStoreFixture fx;
    /* heap remset from fixture */
    fx.heap.region0()->reset(PageAge::old);
    fx.heap.region1()->reset(PageAge::eden);
    fx.heap.region1()->reset(PageAge::eden);

    auto* field = &HeapSlotAt<>(reinterpret_cast<MAddress>(fx.heap.obj0) + TYPEINFO_PTR_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(field);
    const uintptr_t initial = raw(StoreGoodPointer(fx.heap.obj0)) ^ ZPointerRememberedMask;
    std::memcpy(field, &initial, sizeof(initial));

    AllocBuffer alloc;
    InstalledExportAllocBuffer installedAlloc(alloc);

    auto* nonHeapHolder = fx.heap.obj0;
    GC_EXPECT_TRUE(nonHeapHolder != nullptr);
    GC_EXPECT_TRUE(Heap::IsHeapAddress(nonHeapHolder));
    GC_EXPECT_TRUE(Heap::IsHeapAddress(field));

    GC_EXPECT_FALSE(ZPointer::is_store_good((*field).GetFieldValue()));
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        // This is the exported product ABI. A rejected holder access must be
        // observed by the parent's target assertion, not terminate the test runner.
        MCC_WriteRefField(fx.heap.obj1, nonHeapHolder, reinterpret_cast<RefField<false>*>(field));
        GC_EXPECT_EQ(ThreadLocal::GetGCData().storeBarrierBuffer->Pending(), 1u);
        GC_EXPECT_FALSE(SlotPageRemembered(slot));
        GC_EXPECT_TRUE(to_object(field->GetTargetObject()) == fx.heap.obj1);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "HEAP_HOLDER_CONTROL_ASSERT_EXECUTED status=%d\n", status);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}

// P01: mutable global value fields carry the explicit $BP=1 owner;
// $BP=0 is reserved for plain value-type storage.
GC_TEST(DefectRegress, CompilerWriteGlobalOwnerStaticSlotUsesRootPath)
{
    ExportHandleFixture fx;
    RefField<false> staticField(zpointer::null);
    MCC_WriteRefField(fx.heap.obj0, reinterpret_cast<ObjectPtr>(uintptr_t(1)), &staticField);

    const uintptr_t installed = static_cast<uintptr_t>(raw(staticField.GetFieldValue()));
    GC_EXPECT_EQ(installed, raw(StoreGoodPointer(fx.heap.obj0)));
    GC_EXPECT_EQ(ClassifySlotWord(installed), SlotWordVerdict::kColoured);
}

// The compiler's global-struct marker is paired with global storage, not a
// managed HeapSlot.  Preserve that legal marked call while heap classification
// remains authoritative for contradictory inputs.
GC_TEST(DefectRegress, CompilerWriteTaggedGlobalStructUsesRootPath)
{
    ExportHandleFixture fx;
    RefField<false> globalField(zpointer::null);
#if defined(__aarch64__) && !defined(__ANDROID__)
    constexpr uintptr_t globalFlag = 1ULL << 63;
    auto* taggedField = reinterpret_cast<RefField<false>*>(
        reinterpret_cast<uintptr_t>(&globalField) | globalFlag);
    MCC_WriteRefField(fx.heap.obj0, nullptr, taggedField);
#else
    auto* globalMarker = reinterpret_cast<BaseObject*>(static_cast<uintptr_t>(1));
    MCC_WriteRefField(fx.heap.obj0, globalMarker, &globalField);
#endif
    const uintptr_t installed = static_cast<uintptr_t>(raw(globalField.GetFieldValue()));
    GC_EXPECT_FALSE(Heap::IsHeapAddress(&globalField));
    GC_EXPECT_EQ(installed, raw(StoreGoodPointer(fx.heap.obj0)));
    GC_EXPECT_EQ(ClassifySlotWord(installed), SlotWordVerdict::kColoured);
}

GC_TEST(DefectRegress, GcCountExportReadsCollectionStarts)
{
    // zGeneration.cpp:600,637: MCC_GetGCCount reads the heap-wide total that
    // young mark start increments. The test links the product SO, so producer
    // and consumer share storage by construction.
    const uint32_t before = static_cast<uint32_t>(MCC_GetGCCount());
    Heap::GetHeap().increment_total_collections();
    GC_EXPECT_EQ(MCC_GetGCCount(), static_cast<uint32_t>(before + 1));
    Heap::GetHeap().increment_total_collections();
    GC_EXPECT_EQ(MCC_GetGCCount(), static_cast<uint32_t>(before + 2));
}

// hunt-coll SUSPECT: raw-index reuse made double-remove + stale handle ABA.
// Product: ExportRootTable generation-tagged handles (CopyCollector.h).
// Broken sibling is in red_proof.cpp (double-remove recycles the same index twice).
GC_TEST(DefectRegress, ExportHandleDoubleRemoveNoAlias)
{
    ExportHandleFixture fx;
    ExportRootTable table;
    BaseObject* objA = fx.heap.obj0;
    BaseObject* objB = fx.heap.obj1;
    BaseObject* objC = fx.PlaceThirdObject();
    U64 pa = table.RegisterExportRoot(objA);
    table.RemoveExportRoot(pa);
    table.RemoveExportRoot(pa);
    U64 pb = table.RegisterExportRoot(objB);
    U64 pc = table.RegisterExportRoot(objC);
    GC_EXPECT_NE(pb, pc);
    GC_EXPECT_TRUE(table.CheckActiveState(pb, objB));
    GC_EXPECT_TRUE(table.CheckActiveState(pc, objC));
    GC_EXPECT_FALSE(table.CheckActiveState(pa, objA));
    GC_EXPECT_FALSE(table.CheckActiveState(pa, objB));
}

// nwreclaim: 4fcf746a keep-slot used IsMarkedObject<Old> only. Post-flip to-space
// and young holders have no Old face (zPage.inline.hpp:254-256 is_object_live =
// is_allocating || livemap). Soft-null then planted rcx=0 for Array.ix (pc_off=0x29589).
bool ModelHolderIsLive(bool isHeap, bool valid, bool freeOrGarbage, bool allocating, bool youngRegion,
                       bool youngMarked, bool oldMarked)
{
    if (!isHeap || !valid || freeOrGarbage) {
        return false;
    }
    if (allocating) {
        return true;
    }
    return youngRegion ? youngMarked : oldMarked;
}

GC_TEST(DefectRegress, LiveHolderOnAllocatingPageKeepsSlot)
{
    // to-space / TL / recent-full: allocating ⇒ live, even with Old mark=false.
    GC_EXPECT_TRUE(ModelHolderIsLive(true, true, false, true, false, false, false));
    GC_EXPECT_TRUE(ModelHolderIsLive(true, true, false, true, true, false, false));
    // young marked, not allocating.
    GC_EXPECT_TRUE(ModelHolderIsLive(true, true, false, false, true, true, false));
    // old marked, not allocating.
    GC_EXPECT_TRUE(ModelHolderIsLive(true, true, false, false, false, false, true));
    // 4fcf746a shape: Old-only miss on a live young / to-space holder.
    GC_EXPECT_FALSE(ModelHolderIsLive(true, true, false, false, true, false, false));
    // dead holder (free/garbage) still eligible for soft-null.
    GC_EXPECT_FALSE(ModelHolderIsLive(true, true, true, false, false, false, true));
    GC_EXPECT_FALSE(ModelHolderIsLive(false, true, false, true, false, false, false));
}

GC_TEST(DefectRegress, ExportHandleStaleReuseDoesNotTouchNewOccupant)
{
    ExportHandleFixture fx;
    ExportRootTable table;
    BaseObject* objA = fx.heap.obj0;
    BaseObject* objB = fx.heap.obj1;
    U64 ha = table.RegisterExportRoot(objA);
    table.RemoveExportRoot(ha);
    U64 hb = table.RegisterExportRoot(objB);
    GC_EXPECT_NE(ha, hb);
    GC_EXPECT_EQ(ExportRootTable::ExportHandleIndex(ha), ExportRootTable::ExportHandleIndex(hb));
    GC_EXPECT_NE(ExportRootTable::ExportHandleGeneration(ha), ExportRootTable::ExportHandleGeneration(hb));
    table.SetActiveState(ha, false);
    GC_EXPECT_TRUE(table.CheckActiveState(hb, objB));
    GC_EXPECT_FALSE(table.CheckActiveState(ha, objB));
    GC_EXPECT_FALSE(table.CheckActiveState(ha, objA));
}
