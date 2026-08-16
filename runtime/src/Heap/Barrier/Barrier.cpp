// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Barrier.inline.h"
#include "Base/Macros.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/HealPairDiag.h"
#include "Heap/Verify/IdleEdgeDiag.h"
#include "Heap/Verify/LoadGoodProbe.h"
#include "Heap/Verify/RemsetPhaseProbe.h"
#include "Heap/Verify/YyEdgeDiag.h"
#include "Heap/WCollector/EnumBarrier.h"
#include "Heap/WCollector/ForwardBarrier.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "Heap/WCollector/PostTraceBarrier.h"
#include "Heap/WCollector/PreforwardBarrier.h"
#include "Heap/WCollector/TraceBarrier.h"
#include "ObjectModel/Field.inline.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include <atomic>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

namespace MapleRuntime {
static_assert(!std::is_polymorphic<Barrier>::value, "Barrier must not regain virtual dispatch");

template<typename Function>
decltype(auto) Barrier::DispatchPhase(BarrierPhase barrierPhase, const Barrier& barrier, Function&& function)
{
    switch (barrierPhase) {
        case BarrierPhase::IDLE:
            return std::forward<Function>(function)(static_cast<const IdleBarrier&>(barrier));
        case BarrierPhase::ENUM:
            return std::forward<Function>(function)(static_cast<const EnumBarrier&>(barrier));
        case BarrierPhase::TRACE:
            return std::forward<Function>(function)(static_cast<const TraceBarrier&>(barrier));
        case BarrierPhase::POST_TRACE:
            return std::forward<Function>(function)(static_cast<const PostTraceBarrier&>(barrier));
        case BarrierPhase::PREFORWARD:
            return std::forward<Function>(function)(static_cast<const PreforwardBarrier&>(barrier));
        case BarrierPhase::FORWARD:
            return std::forward<Function>(function)(static_cast<const ForwardBarrier&>(barrier));
        case BarrierPhase::STW:
            break;
    }
    std::abort();
}

namespace {
#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
std::atomic<uint64_t> generationalBarrierFastPathHits { 0 };
std::atomic<uint64_t> generationalBarrierRegionLookups { 0 };
#endif

// storegood: is_store_good fast/slow path enter counts (always on, cheap atomics).
std::atomic<uint64_t> g_storeBarrierFastPath { 0 };
std::atomic<uint64_t> g_storeBarrierSlowPath { 0 };

inline void NoteStoreFastPath()
{
    g_storeBarrierFastPath.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteStoreSlowPath()
{
    g_storeBarrierSlowPath.fetch_add(1, std::memory_order_relaxed);
}

// storecov: prev is store-good for the same decoded target we are about to store
// (OpenJDK zBarrier.inline.hpp:381,703 — second write of a registered edge skips remset).
inline bool PrevIsStoreGoodForTarget(Collector& collector, RefField<> prev, BaseObject* newRef)
{
    return collector.is_store_good(prev) && to_object(prev.GetTargetObject()) == newRef;
}

inline bool HasYoungRegionsForRecording()
{
    bool hasYoungRegions = RegionInfo::HasYoungRegions();
#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
    if (hasYoungRegions) {
        generationalBarrierRegionLookups.fetch_add(1, std::memory_order_relaxed);
    } else {
        generationalBarrierFastPathHits.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    return hasYoungRegions;
}
} // namespace

extern "C" MRT_EXPORT uint64_t MRT_StoreBarrierFastPathCount()
{
    return g_storeBarrierFastPath.load(std::memory_order_relaxed);
}

extern "C" MRT_EXPORT uint64_t MRT_StoreBarrierSlowPathCount()
{
    return g_storeBarrierSlowPath.load(std::memory_order_relaxed);
}

extern "C" MRT_EXPORT void MRT_ResetStoreBarrierPathCounts()
{
    g_storeBarrierFastPath.store(0, std::memory_order_relaxed);
    g_storeBarrierSlowPath.store(0, std::memory_order_relaxed);
}

// Pre-write snapshot of (field address → decoded target) for slots that are already
// store-good. Bulk paths (WriteStruct / Copy*) destroy prev bits via memcpy/memmove,
// so the gate must sample before the store (single-field paths read prev inline).
// Nested type declared in Barrier.h; definition lives here next to the bulk gates.
struct Barrier::StoreGoodPrevSnapshot {
    void Push(MAddress addr, BaseObject* target)
    {
        if (size_ < CACHE_CAPACITY) {
            cacheAddr_[size_] = addr;
            cacheTarget_[size_] = target;
        } else {
            excessive_.emplace_back(addr, target);
        }
        ++size_;
    }

    bool Matches(MAddress addr, BaseObject* target) const
    {
        const size_t n = size_ < CACHE_CAPACITY ? size_ : CACHE_CAPACITY;
        for (size_t i = 0; i < n; ++i) {
            if (cacheAddr_[i] == addr && cacheTarget_[i] == target) {
                return true;
            }
        }
        for (const auto& e : excessive_) {
            if (e.first == addr && e.second == target) {
                return true;
            }
        }
        return false;
    }

    bool Empty() const { return size_ == 0; }

private:
    static constexpr size_t CACHE_CAPACITY = 16;
    MAddress cacheAddr_[CACHE_CAPACITY] {};
    BaseObject* cacheTarget_[CACHE_CAPACITY] {};
    size_t size_ { 0 };
    std::vector<std::pair<MAddress, BaseObject*>> excessive_;
};

#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
void Barrier::ResetGenerationalBarrierProbe()
{
    generationalBarrierFastPathHits.store(0, std::memory_order_relaxed);
    generationalBarrierRegionLookups.store(0, std::memory_order_relaxed);
}

uint64_t Barrier::GetGenerationalBarrierFastPathHits()
{
    return generationalBarrierFastPathHits.load(std::memory_order_relaxed);
}

uint64_t Barrier::GetGenerationalBarrierRegionLookups()
{
    return generationalBarrierRegionLookups.load(std::memory_order_relaxed);
}
#endif

void Barrier::WriteI8(BaseObject* obj, Field<int8_t>& field, int8_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI16(BaseObject* obj, Field<int16_t>& field, int16_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI32(BaseObject* obj, Field<int32_t>& field, int32_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteI64(BaseObject* obj, Field<int64_t>& field, int64_t val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteF32(BaseObject* obj, Field<float>& field, float val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteF64(BaseObject* obj, Field<double>& field, double val) const { field.SetFieldValue(obj, val); }

void Barrier::WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    // OpenJDK zBarrier.inline.hpp:695-706 store_barrier_on_heap_oop_field:
    // fast path = is_store_good(prev); slow path = remset/SATB work then color_store_good.
    // Our colour is applied by WriteReferenceImpl (GetAndTryTagRefField → store-good colour).
    // If the pre-store slot is already store-good for the same target, skip remset work
    // (second write of a registered edge must not re-enter RecordCrossGenEdge).
    RefField<> prev(field.GetFieldValue());
    const bool prevStoreGood = PrevIsStoreGoodForTarget(theCollector, prev, ref);
    WriteReferenceImpl(obj, field, ref);
    if (!prevStoreGood) {
        NoteStoreSlowPath();
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    } else {
        NoteStoreFastPath();
    }
    // edgemiss: narrow log when installed target is large (default-off, self-gates).
    if (ref != nullptr) {
        uint8_t bk = 0;
        switch (phase) {
            case BarrierPhase::IDLE:
                bk = 1;
                break;
            case BarrierPhase::ENUM:
                bk = 2;
                break;
            case BarrierPhase::TRACE:
                bk = 3;
                break;
            case BarrierPhase::POST_TRACE:
                bk = 4;
                break;
            case BarrierPhase::PREFORWARD:
                bk = 5;
                break;
            case BarrierPhase::FORWARD:
                bk = 6;
                break;
            case BarrierPhase::STW:
                bk = 7;
                break;
            default:
                bk = 0;
                break;
        }
        HealPairDiag::NoteEdgeWrite(obj, &field, raw(prev.GetFieldValue()),
                                    raw(field.GetFieldValue()), bk);
    }
}

void Barrier::WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.WriteReferenceImpl(obj, field, ref);
        });
    }
    DLOG(BARRIER, "write obj %p ref-field@%p: %p => %p", obj, &field, to_object(field.GetTargetObject()), ref);
    // COLOUR_WRITEBACK_AUDIT R3/批 A：规范色写回，禁 plain 灌堆。
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    field.StoreColoured(newField.GetFieldValue());
}

void Barrier::WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    // storecov: bulk path has no single "prev". Snapshot store-good (addr,target) pairs
    // before memcpy/recolour destroys the pre-store bits; RecordCrossGenEdgesInStruct
    // then skips slots whose post-write target still matches a pre-store store-good edge
    // (second bulk write of the same old→young edges must not re-enter Record).
    StoreGoodPrevSnapshot snap;
    if (obj != nullptr && Heap::IsHeapAddress(obj) && HasYoungRegionsForRecording()) {
        obj->ForEachRefInStruct(
            [this, &snap](RefField<>& field) {
                RefField<> prev(field.GetFieldValue());
                if (theCollector.is_store_good(prev)) {
                    snap.Push(reinterpret_cast<MAddress>(&field), to_object(prev.GetTargetObject()));
                }
            },
            dst, dst + dstLen);
    }
    WriteStructImpl(obj, dst, dstLen, src, srcLen);
    RecordCrossGenEdgesInStruct(obj, dst, dstLen, &snap);
}

void Barrier::WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.WriteStructImpl(obj, dst, dstLen, src, srcLen);
        });
    }
    // R9 bulk：memcpy 会把栈上 plain 整块灌进堆；post-copy 补色环模板 = PostTraceBarrier.cpp:117-128。
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [=](RefField<>& refField) {
                RefField<> oldField(refField);
                MAddress oldValue = raw(oldField.GetFieldValue());
                BaseObject* latest = ReadReference(nullptr, oldField);
                RefField<> newField = theCollector.GetAndTryTagRefField(latest);
                if (oldValue != raw(newField.GetFieldValue())) {
                    HealSlot(refField, to_zpointer(oldValue), newField.GetFieldValue(),
                             HealSite::BarrierWriteStructRecolour);
                }
            },
            dst, dst + dstLen);
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    CHECK_EQ(srcLen, dstLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), dstLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), srcLen);
#endif
}

void Barrier::WriteStaticRef(RootSlot& field, BaseObject* ref) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.WriteStaticRef(field, ref);
        });
    }
    WriteStaticRefPlain(field, ref);
}

void Barrier::WriteStaticRefPlain(RootSlot& field, BaseObject* ref) const
{
    DLOG(BARRIER, "write (barrier) static ref@%p: %p", &field, ref);
    StorePlain(field, from_object(ref));
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), ref);
#endif
}

void Barrier::WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.WriteStaticStruct(dst, dstLen, src, srcLen, gctib);
        });
    }
    // R9 bulk：静态槽 barrier 可见；post-copy 解析转发（STACK_ROOTS_STAY_PLAIN：写回 plain）。
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstLen, reinterpret_cast<void*>(src), srcLen) == EOK,
                 "memcpy_s failed");
    ResolveStaticStructRoots(dst, gctib);
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstLen < srcLen ? dstLen : srcLen);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dst), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(src), copyLen);
#endif
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    gctib.ForEachBitmapWord(dst, [this](RefField<>& field) {
        RecordCrossGenEdge(nullptr, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    });
#endif
}

BaseObject* Barrier::ReadReference(BaseObject* obj, RefField<false>& field) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.ReadReference(obj, field);
        });
    }
    BaseObject* toVersion = nullptr;
    if (theCollector.TryUpdateRefField(obj, field, toVersion)) {
        return toVersion;
    } else {
        BaseObject* target = to_object(field.GetTargetObject());
        return target;
    }
}

BaseObject* Barrier::ReadWeakRef(BaseObject* obj, RefField<false>& field) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.ReadWeakRef(obj, field);
        });
    }
    BaseObject* toVersion = nullptr;
    if (theCollector.TryUpdateRefField(obj, field, toVersion)) {
        return toVersion;
    } else {
        BaseObject* target = to_object(field.GetTargetObject());
        return target;
    }
}

BaseObject* Barrier::ReadStaticRef(ReadOnlyRootSlot& field) const
{
    zaddress_unsafe observed = field.LoadPlain();
    if (is_null(observed)) {
        if (UNLIKELY(LoadGoodProbe::Enabled())) {
            LoadGoodProbe::NoteNull(LoadGoodProbe::kFaceRoot);
        }
        return nullptr;
    }
    // Decode any legacy colour bits at an external ABI boundary without exposing
    // the RootSlot storage as a HeapSlot.
    HeapSlot<> observedBits(to_zpointer(raw(observed)));
    BaseObject* target = to_object(observedBits.GetTargetObject());
    // Hoisted out of the `if` below purely so the probe can record the same decision
    // production makes. Short-circuit order and effects are unchanged.
    const bool ghost =
        target != nullptr && Heap::IsHeapAddress(target) && theCollector.IsGhostFromObject(target);
    if (UNLIKELY(LoadGoodProbe::Enabled())) {
        LoadGoodProbe::NoteRead(LoadGoodProbe::kFaceRoot, raw(observed), ghost,
                                theCollector.is_load_good(observedBits));
    }
    if (ghost) {
        target = theCollector.FindLatestVersion(target);
    }
    return target;
}

// barrier for atomic operation.
void Barrier::AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    RefField<> prev(field.GetFieldValue(order));
    const bool prevStoreGood = PrevIsStoreGoodForTarget(theCollector, prev, ref);
    AtomicWriteReferenceImpl(obj, field, ref, order);
    if (!prevStoreGood) {
        NoteStoreSlowPath();
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    } else {
        NoteStoreFastPath();
    }
}

void Barrier::AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.AtomicWriteReferenceImpl(obj, field, ref, order);
        });
    }
    RefField<> newField = theCollector.GetAndTryTagRefField(ref);
    if (obj != nullptr) {
        DLOG(BARRIER, "atomic write obj %p<%p>(%zu) ref@%p: %#zx -> %p", obj, obj->GetTypeInfo(), obj->GetSize(),
             &field, raw(field.GetFieldValue()), ref);
    } else {
        DLOG(BARRIER, "atomic write static ref@%p: %#zx -> %p", &field, raw(field.GetFieldValue()), ref);
    }

    field.StoreColoured(newField.GetFieldValue(), order);
}

BaseObject* Barrier::AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                         MemoryOrder order) const
{
    // storecov: gate on the actual pre-swap slot bits (not an "expected" value — swap has
    // none). Same shape as WriteReference / AtomicWriteReference: prev store-good for newRef
    // ⇒ edge already registered when the slot first became store-good.
    RefField<> prev(field.GetFieldValue(order));
    const bool prevStoreGood = PrevIsStoreGoodForTarget(theCollector, prev, newRef);
    BaseObject* oldRef = AtomicSwapReferenceImpl(obj, field, newRef, order);
    if (!prevStoreGood) {
        NoteStoreSlowPath();
        RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
    } else {
        NoteStoreFastPath();
    }
    return oldRef;
}

BaseObject* Barrier::AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* newRef,
                                             MemoryOrder order) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.AtomicSwapReferenceImpl(obj, field, newRef, order);
        });
    }
    RefField<> coloured = theCollector.GetAndTryTagRefField(newRef);
    MAddress oldValue = raw(field.Exchange(coloured.GetFieldValue(), order));
    RefField<> oldField(oldValue);
    BaseObject* oldRef = ReadReference(nullptr, oldField);
    DLOG(BARRIER, "atomic swap obj %p<%p>(%zu) ref-field@%p: old %#zx(%p), new %#zx(%p)", obj, obj->GetTypeInfo(),
         obj->GetSize(), &field, oldValue, oldRef, raw(field.GetFieldValue()), newRef);
    return oldRef;
}

BaseObject* Barrier::AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.AtomicReadReference(obj, field, order);
        });
    }
    RefField<false> tmpField(field.GetFieldValue(order));
    if (theCollector.IsOldPointer(tmpField)) {
        BaseObject* toVersion = ReadReference(nullptr, tmpField);
        // R10：治愈写也必须带规范色，禁 plain SetTargetObject。
        RefField<> healed = theCollector.GetAndTryTagRefField(toVersion);
        field.StoreColoured(healed.GetFieldValue());
        DLOG(BARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(tmpField.GetFieldValue()), toVersion);
        return toVersion;
    }

    BaseObject* target = to_object(tmpField.GetTargetObject());
    DLOG(BARRIER, "atomic read obj %p ref@%p: %#zx -> %p", obj, &field, raw(tmpField.GetFieldValue()), target);
    return target;
}

bool Barrier::CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                      MemoryOrder succOrder, MemoryOrder failOrder) const
{
    // storecov: CAS has an expected (oldRef) and a stored (newRef). The store-good gate
    // uses the actual pre-CAS slot bits (same as WriteReference prev), compared to newRef
    // — not to oldRef. On success with prev already store-good for newRef, the write is a
    // same-target refresh (typically oldRef==newRef) and remset work is redundant.
    // Failed CAS stores nothing ⇒ no Record (unchanged).
    RefField<> prev(field.GetFieldValue(std::memory_order_relaxed));
    const bool prevStoreGood = PrevIsStoreGoodForTarget(theCollector, prev, newRef);
    bool success = CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
    if (success) {
        if (!prevStoreGood) {
            NoteStoreSlowPath();
            RecordCrossGenEdge(obj, reinterpret_cast<MAddress>(&field), to_object(field.GetTargetObject()));
        } else {
            NoteStoreFastPath();
        }
    }
    return success;
}

bool Barrier::CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                          BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.CompareAndSwapReferenceImpl(obj, field, oldRef, newRef, succOrder, failOrder);
        });
    }
    // Compare on decoded object identity; CAS on observed raw bits (colour-aware).
    // Shape matches EnumBarrier.cpp:259-280 / IdleBarrier.cpp:121-138. Plain expected vs
    // coloured slot bits always fail (COLOUR_WRITEBACK_AUDIT R1).
    // Retries are bounded (kCasAttempts in ColourMask.h). ZGC terminates a self-healing
    // retry because its colours form a monotone lattice; ours do not -- a reader may
    // self-heal this very slot on every load, so the observed bits keep changing while the
    // decoded identity stays oldRef, and an unbounded loop never lands the exchange.
    // natural_wave spun 47 minutes of user time in two spinning threads before this bound
    // existed. Exhausting the budget reports failure, which CAS callers already handle.
    MAddress oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
    RefField<false> oldField(oldFieldValue);
    BaseObject* oldVersion = ReadReference(nullptr, oldField);

    for (int attempt = 0; attempt < kCasAttempts && oldVersion == oldRef; ++attempt) {
        // Recolour per attempt: a phase may flip mid-retry, and writing last epoch's colour
        // would hand the next reader a value its mask calls bad.
        RefField<> newField = theCollector.GetAndTryTagRefField(newRef);
        if (HealSlot(field, to_zpointer(oldFieldValue), newField.GetFieldValue(),
                     HealSite::BarrierCompareAndSwapReference, HealNull::Allow, succOrder, failOrder)) {
            DLOG(BARRIER, "cas 1 for obj %p reffield@%p: old %#zx->%p, expect %p, new %p", obj, &field,
                 oldFieldValue, oldVersion, oldRef, newRef);
            return true;
        }
        oldFieldValue = raw(field.GetFieldValue(std::memory_order_seq_cst));
        RefField<false> tmp(oldFieldValue);
        oldVersion = ReadReference(nullptr, tmp);
    }
    DLOG(BARRIER, "cas 0 for obj %p reffield@%p: old %#zx->%p, expect %p, new %p", obj, &field, oldFieldValue,
         oldVersion, oldRef, newRef);
    return false;
}

void Barrier::CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj, MAddress srcField,
                           MIndex srcSize) const
{
    // storecov: bulk ref-array — snapshot store-good slots before memmove/recolour.
    StoreGoodPrevSnapshot snap;
    if (dstObj != nullptr && Heap::IsHeapAddress(dstObj) && HasYoungRegionsForRecording()) {
        MAddress end = dstField + dstSize;
        for (MAddress current = dstField; current + sizeof(HeapSlot<>) <= end; current += sizeof(HeapSlot<>)) {
            HeapSlot<>& slot = HeapSlotAt<>(current);
            RefField<> prev(slot.GetFieldValue());
            if (theCollector.is_store_good(prev)) {
                snap.Push(current, to_object(prev.GetTargetObject()));
            }
        }
    }
    CopyRefArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
    RecordCrossGenEdgesInRefArray(dstObj, dstField, dstSize, &snap);
}

void Barrier::CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                               MAddress srcField, MIndex srcSize) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.CopyRefArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
        });
    }
    (void)srcObj;
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");
    // R9：堆 dst 上 memmove 可能灌入栈 plain；逐槽补色。非堆 dst = Y5 保持 plain。
    // heap→heap 已有色时 GetAndTryTagRefField 幂等（Y6 不新增 plain，补色也无害）。
    if (dstObj != nullptr && Heap::IsHeapAddress(dstObj)) {
        MAddress end = dstField + dstSize;
        for (MAddress cur = dstField; cur + sizeof(HeapSlot<>) <= end; cur += sizeof(HeapSlot<>)) {
            HeapSlot<>& refField = HeapSlotAt<>(cur);
            RefField<> oldField(refField);
            MAddress oldValue = raw(oldField.GetFieldValue());
            BaseObject* latest = ReadReference(nullptr, oldField);
            RefField<> newField = theCollector.GetAndTryTagRefField(latest);
            if (oldValue != raw(newField.GetFieldValue())) {
                HealSlot(refField, to_zpointer(oldValue), newField.GetFieldValue(),
                         HealSite::BarrierCopyRefArrayRecolour);
            }
        }
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void Barrier::CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                              MAddress srcField, MIndex srcSize) const
{
    // storecov: bulk struct-array — same pre-store snapshot gate as WriteStruct.
    StoreGoodPrevSnapshot snap;
    if (dstObj != nullptr && Heap::IsHeapAddress(dstObj) && HasYoungRegionsForRecording()) {
        dstObj->ForEachRefInStruct(
            [this, &snap](RefField<>& field) {
                RefField<> prev(field.GetFieldValue());
                if (theCollector.is_store_good(prev)) {
                    snap.Push(reinterpret_cast<MAddress>(&field), to_object(prev.GetTargetObject()));
                }
            },
            dstField, dstField + dstSize);
    }
    CopyStructArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
    RecordCrossGenEdgesInStruct(dstObj, dstField, dstSize, &snap);
}

void Barrier::CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize, BaseObject* srcObj,
                                  MAddress srcField, MIndex srcSize) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.CopyStructArrayImpl(dstObj, dstField, dstSize, srcObj, srcField, srcSize);
        });
    }
    (void)srcObj;
    CHECK_DETAIL(memmove_s(reinterpret_cast<void*>(dstField), dstSize, reinterpret_cast<void*>(srcField), srcSize) ==
                     EOK,
                 "memmove_s failed");
    // R9 bulk：struct 数组 memmove 后对堆 dst 引用槽补色（模板 PostTrace WriteStruct post-copy）。
    if (dstObj != nullptr && dstObj->HasRefField() && Heap::IsHeapAddress(dstObj)) {
        RefFieldVisitor recolour = [this](RefField<false>& field) {
            RefField<> oldField(field);
            MAddress oldValue = raw(oldField.GetFieldValue());
            BaseObject* latest = ReadReference(nullptr, oldField);
            RefField<> newField = theCollector.GetAndTryTagRefField(latest);
            if (oldValue != raw(newField.GetFieldValue())) {
                HealSlot(field, to_zpointer(oldValue), newField.GetFieldValue(),
                         HealSite::BarrierCopyStructArrayRecolour);
            }
        };
        static_cast<MArray*>(dstObj)->ForEachRefFieldInRange(recolour, dstField, dstField + srcSize);
    }
#if defined(CANGJIE_TSAN_SUPPORT)
    size_t copyLen = (dstSize < srcSize ? dstSize : srcSize);
    Sanitizer::TsanWriteMemoryRange(reinterpret_cast<void*>(dstField), copyLen);
    Sanitizer::TsanReadMemoryRange(reinterpret_cast<void*>(srcField), copyLen);
#endif
}

void Barrier::FixupNonHeapStructRefs(MAddress dst, BaseObject* srcObj, MAddress src, size_t size) const
{
    // Heap→heap must keep coloured slots; only non-heap (stack sret / root buffer) goes plain.
    if (srcObj == nullptr || Heap::IsHeapAddress(dst)) {
        return;
    }
    srcObj->ForEachRefInStruct(
        [this, srcObj, dst, src, size](RefField<false>& field) {
            MAddress fieldAddr = reinterpret_cast<MAddress>(&field);
            if (fieldAddr < src || fieldAddr >= (src + size)) {
                return;
            }
            // ReadReference may self-heal the heap source (colour stays on heap).
            BaseObject* target = ReadReference(srcObj, field);
            StorePlain(RootSlotAt(dst + (fieldAddr - src)), from_object(target));
        },
        src, src + size);
}

void Barrier::FixupNonHeapStaticStructRefs(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    if (Heap::IsHeapAddress(dst)) {
        return;
    }
    gctib.ForEachBitmapWordInRange(
        src,
        [this, dst, src](RefField<>& srcField) {
            MAddress offset = reinterpret_cast<MAddress>(&srcField) - src;
            BaseObject* target = ReadReference(nullptr, srcField);
            StorePlain(RootSlotAt(dst + offset), from_object(target));
        },
        src, src + size);
}

// Post-copy fixup for a bulk write into static/global storage.
//
// What it must do: the bytes just memcpy'd may name stale (pre-forwarding) objects, so each ref
// word is resolved through the phase read barrier and the current version is stored back.
//
// What it must NOT do: store a *coloured* value. Static words are roots -- StaticRootTable
// registers them as RootSlot (TracingCollector.cpp:225-243) and WCollector::EnumAndTagRawRoot
// heals them with StorePlain (WCollector.cpp:962-1001, "the root storage itself is never exposed
// as a HeapSlot"). Colouring here is overwritten plain by the next root enumeration, and CAS on a
// static slot sits on the relroroot hazard (B-4 ⑤: those pages can be RELRO r--p).
//
// The read barrier may self-heal the slot it is handed; it is handed a *local copy* so the heal
// cannot leak colour back into the static word.
void Barrier::ResolveStaticStructRoots(MAddress dst, const GCTib gctib) const
{
    gctib.ForEachRootSlot(dst, [this](RootSlot& slot) {
        zaddress_unsafe observed = slot.LoadPlain();
        if (is_null(observed)) {
            return;
        }
        // Legacy coloured roots still exist at external ABI edges; decode, never store back.
        HeapSlot<> observedBits(to_zpointer(raw(observed)));
        BaseObject* resolved = ReadReference(nullptr, observedBits);
        if (raw(observed) != reinterpret_cast<MAddress>(resolved)) {
            StorePlain(slot, from_object(resolved));
        }
    });
}

void Barrier::ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.ReadStruct(dst, obj, src, size);
        });
    }
    size_t dstSize = size;
    size_t srcSize = size;
    if (obj != nullptr) {
        obj->ForEachRefInStruct(
            [this, obj](RefField<false>& field) {
                // MAddress bias = reinterpret_cast<MAddress>(&field) - reinterpret_cast<MAddress>(src);
                // The destination reference slot starts at dst + bias.
                BaseObject* fromVersion = to_object(field.GetTargetObject());
                (void)fromVersion;
                BaseObject* toVersion = nullptr;
                theCollector.TryUpdateRefField(obj, field, toVersion);
            },
            src, src + size);
    }

    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstSize, reinterpret_cast<void*>(src), srcSize) == EOK,
                 "read struct memcpy_s failed");
    // Non-heap dst: overwrite ref slots with plain (STACK_ROOTS_STAY_PLAIN).
    FixupNonHeapStructRefs(dst, obj, src, size);
}

void Barrier::ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const
{
    if (phase != BarrierPhase::STW) {
        return DispatchPhase(phase, *this, [&](const auto& barrier) {
            return barrier.ReadStaticStruct(dst, src, size, gctib);
        });
    }
    size_t dstSize = size;
    size_t srcSize = size;
    CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), dstSize, reinterpret_cast<void*>(src), srcSize) == EOK,
                 "read struct memcpy_s failed");
    if (!Heap::IsHeapAddress(dst)) {
        FixupNonHeapStaticStructRefs(dst, src, size, gctib);
        return;
    }
    gctib.ForEachBitmapWord(dst, [this](RefField<>& refField) {
        BaseObject* toVersion = nullptr;
        theCollector.TryUpdateRefField(nullptr, refField, toVersion);
    });
}

void Barrier::WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    // storecov: not one of the six gate sites. Heap-dst branch delegates to WriteStruct
    // (already gated). Outer Record stays unconditional (nullptr snap) for coverage;
    // remset Record is idempotent, counters stay owned by WriteStruct / single-field paths.
    WriteGenericImpl(obj, fieldPtr, src, size);
    RecordCrossGenEdgesInStruct(obj, reinterpret_cast<MAddress>(fieldPtr), size);
}

void Barrier::WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const
{
    if (phase == BarrierPhase::ENUM) {
        return static_cast<const EnumBarrier&>(*this).WriteGenericImpl(obj, fieldPtr, src, size);
    }
    if (phase == BarrierPhase::TRACE) {
        return static_cast<const TraceBarrier&>(*this).WriteGenericImpl(obj, fieldPtr, src, size);
    }
    if ((obj != nullptr && !obj->HasRefField()) || (!Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))) {
        CHECK_DETAIL(memcpy_s(fieldPtr, size,
                              reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(src) + TYPEINFO_PTR_SIZE),
                              size) == EOK,
                     "WriteGeneric memcpy_s failed");
#if defined(CANGJIE_TSAN_SUPPORT)
        if (Heap::IsHeapAddress(src)) {
            Sanitizer::TsanReadMemoryRange(
                reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(src) + TYPEINFO_PTR_SIZE), size);
        }
        if (Heap::IsHeapAddress(obj)) {
            Sanitizer::TsanWriteMemoryRange(fieldPtr, size);
        }
#endif
    } else if (!Heap::IsHeapAddress(obj) && Heap::IsHeapAddress(src)) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fieldPtr);
        MAddress srcAddr = reinterpret_cast<MAddress>(src) + TYPEINFO_PTR_SIZE;
        ReadStruct(dstAddr, src, srcAddr, size);
    } else if ((Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))||
        (Heap::IsHeapAddress(obj) && Heap::IsHeapAddress(src))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(fieldPtr);
        MAddress srcAddr = reinterpret_cast<MAddress>(src) + TYPEINFO_PTR_SIZE;
        WriteStruct(obj, dstAddr, size, srcAddr, size);
    }
}
void Barrier::ReadGeneric(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    ReadGenericImpl(dstObj, obj, fieldPtr, size);
    if (Heap::IsHeapAddress(dstObj)) {
        RecordCrossGenEdgesInStruct(dstObj, reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE, size);
    }
}

void Barrier::ReadGenericImpl(const ObjectPtr dstObj, ObjectPtr obj, void* fieldPtr, size_t size) const
{
    if (phase == BarrierPhase::ENUM) {
        return static_cast<const EnumBarrier&>(*this).ReadGenericImpl(dstObj, obj, fieldPtr, size);
    }
    if (phase == BarrierPhase::TRACE) {
        return static_cast<const TraceBarrier&>(*this).ReadGenericImpl(dstObj, obj, fieldPtr, size);
    }
    if (!Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj)) {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dstObj) + TYPEINFO_PTR_SIZE),
                              size, fieldPtr, size) == EOK,
                     "ReadGeneric memcpy_s failed");
    } else if (!Heap::IsHeapAddress(dstObj) && Heap::IsHeapAddress(obj)) {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        ReadStruct(dstAddr, obj, srcAddr, size);
    } else if ((Heap::IsHeapAddress(dstObj) && !Heap::IsHeapAddress(obj))||
        (Heap::IsHeapAddress(dstObj) && Heap::IsHeapAddress(obj))) {
        MAddress dstAddr = reinterpret_cast<MAddress>(dstObj) + TYPEINFO_PTR_SIZE;
        MAddress srcAddr = reinterpret_cast<MAddress>(fieldPtr);
        WriteStruct(dstObj, dstAddr, size, srcAddr, size);
    }
}

void Barrier::RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const
{
    using namespace RemsetPhaseProbe;
    // promoteedge gen codes: 0=unknown 1=young 2=old 3=nonheap
    constexpr uint8_t kGenUnknown = 0;
    constexpr uint8_t kGenYoung = 1;
    constexpr uint8_t kGenOld = 2;
    constexpr uint8_t kGenNonHeap = 3;
    auto genOfAddr = [](MAddress addr) -> uint8_t {
        if (addr == 0 || !Heap::IsHeapAddress(addr)) {
            return kGenNonHeap;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
        if (region == nullptr) {
            return kGenUnknown;
        }
        return region->IsYoungRegion() ? kGenYoung : kGenOld;
    };
    const bool probeOn = Enabled();
    const bool idleEdgeOn = IdleEdgeDiag::Enabled();
    // idlewrite: also stamp object-header gen so field-addr vs obj mismatch is visible.
    // Computed behind the gate: genOfAddr does an IsHeapAddress plus a region lookup, and this
    // is the write barrier's hot path -- diagnostics must cost nothing when they are off.
    const uint8_t holderObjGen = (idleEdgeOn && obj != nullptr && Heap::IsHeapAddress(obj))
        ? genOfAddr(reinterpret_cast<MAddress>(obj))
        : kGenUnknown;
    const bool forceRecord = ForceRecordEnabled();
    GCPhase phase = GCPhase::GC_PHASE_UNDEF;
    if (probeOn || idleEdgeOn) {
        phase = Heap::GetHeap().GetGCPhase();
    }

    if (!HasYoungRegionsForRecording() && !forceRecord) {
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_NO_YOUNG, false);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, genOfAddr(fieldAddress),
                                              genOfAddr(reinterpret_cast<MAddress>(ref)),
                                              static_cast<uint8_t>(REASON_NO_YOUNG), holderObjGen);
        }
        return;
    }
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_REF_NULL_OR_NONHEAP, false);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, genOfAddr(fieldAddress), kGenNonHeap,
                                              static_cast<uint8_t>(REASON_REF_NULL_OR_NONHEAP), holderObjGen);
        }
        return;
    }
    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(ref));
    if (!targetRegion->IsYoungRegion()) {
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_REF_NOT_YOUNG, false);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, genOfAddr(fieldAddress), kGenOld,
                                              static_cast<uint8_t>(REASON_REF_NOT_YOUNG), holderObjGen);
        }
        return;
    }
    // Heap holder: only record old→young (source not young).
    if (Heap::IsHeapAddress(fieldAddress)) {
        if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
            if (probeOn) {
                NoteWrite(fieldAddress, phase, REASON_HOLDER_NULL_OR_NONHEAP, false);
            }
            if (idleEdgeOn) {
                IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, kGenUnknown, kGenYoung,
                                                  static_cast<uint8_t>(REASON_HOLDER_NULL_OR_NONHEAP), holderObjGen);
            }
            return;
        }
        RegionInfo* sourceRegion = RegionInfo::GetRegionInfoAt(fieldAddress);
        if (sourceRegion->IsYoungRegion()) {
            if (probeOn) {
                NoteWrite(fieldAddress, phase, REASON_HOLDER_YOUNG, false);
            }
            if (idleEdgeOn) {
                IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, kGenYoung, kGenYoung,
                                                  static_cast<uint8_t>(REASON_HOLDER_YOUNG), holderObjGen);
            }
            if (UNLIKELY(YyEdgeDiag::Enabled())) {
                YyEdgeDiag::NoteYoungToYoung(obj, fieldAddress, ref);
            }
            // h3seed2 甲: seed the young holder object into the next minor work stack.
            // Object-level dirty list (AllocBuffer), not field remset — avoids y2yN remset bloat.
            if (obj != nullptr) {
                AllocBuffer* buffer = AllocBuffer::GetAllocBuffer();
                if (buffer != nullptr) {
                    buffer->PushY2yDirtyHolder(obj);
                }
            }
            if (UNLIKELY(YyEdgeDiag::RecordEnabled())) {
                theRememberedSet.Record(fieldAddress, /*fromMutatorBarrier=*/true);
            }
            return;
        }
        theRememberedSet.Record(fieldAddress, /*fromMutatorBarrier=*/true);
        if (probeOn) {
            NoteWrite(fieldAddress, phase, REASON_RECORDED, true);
        }
        if (idleEdgeOn) {
            IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, true, kGenOld, kGenYoung,
                                              static_cast<uint8_t>(REASON_RECORDED), holderObjGen);
        }
        return;
    }
    // Non-heap fields cannot contribute heap-region remset bits. Static/global
    // slots are visited as roots in every minor collection, so recording them in
    // the remset only makes RescanRememberedSet discard them as non-heap slots.
    (void)obj;
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    theRememberedSet.RecordStaticForCrossCheck(
        fieldAddress, reinterpret_cast<MAddress>(__builtin_return_address(0)));
#endif
    if (probeOn) {
        NoteWrite(fieldAddress, phase, REASON_HOLDER_NULL_OR_NONHEAP, false);
    }
    if (idleEdgeOn) {
        IdleEdgeDiag::NoteBarrierDecision(fieldAddress, phase, false, kGenNonHeap, kGenYoung,
                                          static_cast<uint8_t>(REASON_HOLDER_NULL_OR_NONHEAP), holderObjGen);
    }
}

void Barrier::RecordCrossGenEdgesInStruct(BaseObject* obj, MAddress start, size_t size,
                                          const StoreGoodPrevSnapshot* prevSnap) const
{
    if (!HasYoungRegionsForRecording()) {
        return;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    obj->ForEachRefInStruct(
        [this, obj, prevSnap](RefField<>& field) {
            const MAddress addr = reinterpret_cast<MAddress>(&field);
            BaseObject* ref = to_object(field.GetTargetObject());
            // storecov: when a pre-store snapshot is supplied, skip remset for slots that
            // were already store-good for this target; count fast/slow only on gated calls
            // (nullptr snap = legacy always-Record, no counter noise for ReadGeneric).
            if (prevSnap != nullptr) {
                if (prevSnap->Matches(addr, ref)) {
                    NoteStoreFastPath();
                    return;
                }
                NoteStoreSlowPath();
            }
            RecordCrossGenEdge(obj, addr, ref);
        },
        start, start + size);
}

void Barrier::RecordCrossGenEdgesInRefArray(BaseObject* obj, MAddress start, size_t size,
                                            const StoreGoodPrevSnapshot* prevSnap) const
{
    if (!HasYoungRegionsForRecording()) {
        return;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    MAddress end = start + size;
    for (MAddress current = start; current + sizeof(HeapSlot<>) <= end; current += sizeof(HeapSlot<>)) {
        HeapSlot<>& field = HeapSlotAt<>(current);
        BaseObject* ref = to_object(field.GetTargetObject());
        if (prevSnap != nullptr) {
            if (prevSnap->Matches(current, ref)) {
                NoteStoreFastPath();
                continue;
            }
            NoteStoreSlowPath();
        }
        RecordCrossGenEdge(obj, current, ref);
    }
}

} // namespace MapleRuntime
