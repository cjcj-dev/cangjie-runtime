// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BARRIER_H
#define MRT_BARRIER_H

#include "Common/BaseObject.h"
#if defined(MRT_TESTABLE_INTERNALS)
#include <functional>
#endif
#include "Common/ColourEncoding.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "ObjectModel/Field.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
class Collector;
enum class ReferenceStrength : uint8_t { Strong, Weak, Phantom };
struct ForwardingProvenance;

using ZBarrierFastPath = bool (*)(zpointer);
using ZBarrierColor = zpointer (*)(zaddress, zpointer);

class ZBarrier {
public:
    ZBarrier() = default;
    ZBarrier(const ZBarrier&) = delete;
    ZBarrier& operator=(const ZBarrier&) = delete;

#if defined(MRT_TESTABLE_INTERNALS)
    enum class FieldMarkKind { Old, Finalizable, Young, Remset };
    static std::function<void(FieldMarkKind, RefField<>&, zpointer, zaddress)> testFieldMarkResult;
#endif

    static BaseObject* ReadReference(BaseObject* obj, RefField<false>& field);
    static BaseObject* ReadStaticRef(NativeSlot& field);
    static void MarkYoungGoodBarrierOnOopField(NativeSlot& field);
    static void MarkFinalizableBarrierOnRoot(NativeSlot& field);
    static void MarkBarrierOnOldOopField(BaseObject* holder, RefField<>& field, bool finalizable);
    static void MarkBarrierOnYoungOopField(RefField<>& field);
    static zaddress RemsetBarrierOnOopField(RefField<>& field);
    static BaseObject* ReadPhantomRef(BaseObject* obj, RefField<false>& field);
    static BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field);
    static void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size);
    static void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib);

    static void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref);
    static void WriteStaticRef(NativeSlot& field, BaseObject* ref);
    static void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen);
    static void WriteStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, GCTib gctib);
    static void ReadStruct(MAddress dst, MAddress src, size_t size, GCTib gctib);
    static void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib);

    static void CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                      BaseObject* srcObj, MAddress srcField, MIndex srcSize);
    static void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                         BaseObject* srcObj, MAddress srcField, MIndex srcSize);

    static BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order);

    static void AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order);
    static BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order);
    static bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder);

    static zpointer load_atomic(volatile zpointer* p);
    static ZGeneration* remap_generation(zpointer ptr);
    static void remap_young_relocated(volatile zpointer* p, zpointer o);
    static zaddress make_load_good(zpointer ptr);
    static zaddress make_load_good_no_relocate(zpointer ptr);
    static void remember(volatile zpointer* p);
    static void mark_and_remember(volatile zpointer* p, zaddress addr);
    static void store_barrier_on_heap_oop_field(volatile zpointer* p, bool heal);
    static void store_barrier_on_native_oop_field(volatile zpointer* p, bool heal);
    static zaddress load_barrier_on_oop_field(volatile zpointer* p);
    static zaddress load_barrier_on_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static void load_barrier_on_oop_array(volatile zpointer* p, size_t length);

    static void WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref);
    static void WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen);
    static void CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                          BaseObject* srcObj, MAddress srcField, MIndex srcSize);
    static void CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                             BaseObject* srcObj, MAddress srcField, MIndex srcSize);
    static void AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                  MemoryOrder order);
    static BaseObject* AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                        MemoryOrder order);
    static bool CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                     BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder);

    static void CopyStructPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, size_t size);
    static void CopyStaticStructPlainToNonHeap(MAddress dst, MAddress src, size_t size, const GCTib gctib);
    static void CopyStructArrayPlainToNonHeap(MAddress dstField, BaseObject* srcObj, MAddress srcField, size_t srcSize);
    static void CopyRefArrayPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, MIndex dstSize, MIndex srcSize);

    __attribute__((visibility("hidden"))) static void CopyObjectStructColouredToHeap(
        BaseObject* layoutObj, MAddress layoutStart, MAddress dst, size_t dstLen,
        MAddress src, size_t srcLen);
    __attribute__((visibility("hidden"))) static void CopyStaticStructColouredToHeap(
        MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib);
    __attribute__((visibility("hidden"))) static void CopyStructArrayColouredToHeap(
        BaseObject* dstObj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen);
    __attribute__((visibility("hidden"))) static void CopyRefArrayColouredToHeap(
        MAddress dst, size_t dstLen, MAddress src, size_t srcLen);

    static void RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref,
                            zpointer prev = zpointer::null);

    static bool is_load_good_or_null_fast_path(zpointer ptr);
    static bool is_mark_good_fast_path(zpointer ptr);
    static bool is_store_good_fast_path(zpointer ptr);
    static bool is_store_good_or_null_fast_path(zpointer ptr);
    static bool is_store_good_or_null_any_fast_path(zpointer ptr);
    static bool is_mark_young_good_fast_path(zpointer ptr);
    static bool is_finalizable_good_fast_path(zpointer ptr);

    static void self_heal(ZBarrierFastPath fast_path, volatile zpointer* p, zpointer ptr, zpointer heal_ptr, bool allow_null);
    static void assert_transition_monotonicity(zpointer oldPtr, zpointer newPtr);

    template<typename SlowPath>
    static zaddress barrier(ZBarrierFastPath fast_path, SlowPath slow_path, ZBarrierColor color,
                            volatile zpointer* p, zpointer o, bool allow_null = false);

    using MarkFastPath = bool (*)(zpointer);
    using MarkColor = zpointer (*)(zaddress, zpointer);
    template<typename SlowPath>
    static zaddress MarkBarrier(MarkFastPath fast, SlowPath slow, MarkColor color,
                           RefField<>& field, zpointer observed, const ForwardingProvenance& provenance);
    static bool IsFinalizableGoodFastPath(zpointer value);
    static zpointer ColorFinalizableGood(zaddress address, zpointer previous);
    static zaddress MarkFinalizableSlowPath(zaddress address);
    static zaddress MarkFinalizableFromOldSlowPath(zaddress address);
    static bool IsMarkGoodFastPath(zpointer value);
    static bool IsStoreGoodOrNullAnyFastPath(zpointer value);
    static zpointer ColorMarkGood(zaddress address, zpointer previous);
    static zpointer ColorStoreGood(zaddress address, zpointer previous);
    static zpointer ColorRemsetGood(zaddress address, zpointer previous);
    static zaddress MarkFromOldSlowPath(zaddress address);
    static zaddress MarkFromYoungSlowPath(zaddress address);
    static bool IsMarkYoungGoodFastPath(zpointer value);
    static zpointer ColorMarkYoungGood(zaddress address, zpointer previous);
    static zaddress MarkYoungSlowPath(zaddress address);
    static void MarkIfYoung(zaddress address);
    static void MarkYoung(zaddress address);
    template<bool atomic>
    static void NativeStoreBarrier(RefField<atomic>& field, bool heal);
    template<bool atomic>
    static BaseObject* LoadBarrier(BaseObject* obj, RefField<atomic>& field, zpointer observed,
                            ReferenceStrength strength);
    template<bool atomic>
    static void StoreBarrier(BaseObject* obj, RefField<atomic>& field, bool heal,
                      ReferenceStrength strength = ReferenceStrength::Strong);

    static zaddress relocate_or_remap(zaddress_unsafe addr, ZGeneration* generation);
    static zaddress remap(zaddress_unsafe addr, ZGeneration* generation);
    static zaddress load_good_slow_path(zaddress addr);
    static zaddress keep_alive_slow_path(zaddress addr);
    static zaddress blocking_keep_alive_on_weak_slow_path(zaddress addr);
    static zaddress blocking_keep_alive_on_phantom_slow_path(zaddress addr);
    static zpointer ColorLoadGood(zaddress address, zpointer previous);
};

using Barrier = ZBarrier;
} // namespace MapleRuntime
#endif // ~MRT_BARRIER_H
