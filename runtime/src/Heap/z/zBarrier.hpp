// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BARRIER_H
#define MRT_BARRIER_H

#include <atomic>
#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "ObjectModel/Field.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
enum class HandVerdict : uint8_t;

class AllStatic {
    AllStatic() = delete;
    AllStatic(const AllStatic&) = delete;
    AllStatic& operator=(const AllStatic&) = delete;
};

using ZBarrierFastPath = bool (*)(zpointer);
using ZBarrierColor = zpointer (*)(zaddress, zpointer);

class ZBarrier : public AllStatic {
public:
    enum class RefSlotKind : U8 { STRONG, WEAK_REFERENT };
    static BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field);
    static bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef);
    template<bool forward>
    static bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                      BaseObject*& toObj);
    static bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
                                         bool allowNull = false);

    static HandVerdict JudgeHandOutTarget(BaseObject* target);
    [[noreturn]] static void FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits);
    static BaseObject* ValidateCurrentValue(BaseObject* target);
    static void CheckStoreGoodTarget(const char* consumer, BaseObject* target);
    static RefField<> GetAndTryTagRefField(BaseObject* target);



    static void MarkYoungGoodBarrierOnOopField(NativeSlot& field);
    static void MarkFinalizableBarrierOnRoot(NativeSlot& field);
    static void MarkBarrierOnOldOopField(RefField<>& field, bool finalizable);
    static void MarkBarrierOnYoungOopField(RefField<>& field);
    static zaddress RemsetBarrierOnOopField(RefField<>& field);





    static zpointer load_atomic(volatile zpointer* p);
    static ZGeneration* remap_generation(zpointer ptr);
    static void remap_young_relocated(volatile zpointer* p, zpointer o);
    static zaddress make_load_good(zpointer ptr);
    static zaddress make_load_good_no_relocate(zpointer ptr);
    static void remember(volatile zpointer* p);
    static void mark_and_remember(volatile zpointer* p, zaddress addr);
    static void store_barrier_on_heap_oop_field(volatile zpointer* p, bool heal);
    static void store_barrier_on_native_oop_field(volatile zpointer* p, bool heal);
    static void no_keep_alive_store_barrier_on_heap_oop_field(volatile zpointer* p);
    static zaddress heap_store_slow_path(volatile zpointer* p, zaddress addr, zpointer prev, bool heal);
    static zaddress no_keep_alive_heap_store_slow_path(volatile zpointer* p, zaddress addr);
    static zaddress native_store_slow_path(zaddress addr);
    static zaddress load_barrier_on_oop_field(volatile zpointer* p);
    static zaddress load_barrier_on_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress keep_alive_load_barrier_on_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress no_keep_alive_load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress blocking_keep_alive_load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress blocking_keep_alive_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress blocking_load_barrier_on_weak_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress blocking_load_barrier_on_phantom_oop_field_preloaded(volatile zpointer* p, zpointer o);
    static zaddress blocking_load_barrier_on_weak_slow_path(volatile zpointer* p, zaddress addr);
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
    static void verify_on_weak(volatile zpointer* p);
#else
    static void verify_on_weak(volatile zpointer*) {}
#endif
    static bool clean_barrier_on_phantom_oop_field(volatile zpointer* p);
    static void load_barrier_on_oop_array(volatile zpointer* p, size_t length);




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
                           RefField<>& field, zpointer observed);
    static bool IsFinalizableGoodFastPath(zpointer value);
    static zpointer ColorFinalizableGood(zaddress address, zpointer previous);
    static zaddress MarkFinalizableSlowPath(zaddress address);
    static zaddress MarkFinalizableFromOldSlowPath(zaddress address);
    static bool IsMarkGoodFastPath(zpointer value);
    static bool IsStoreGoodOrNullAnyFastPath(zpointer value);
    static zpointer ColorMarkGood(zaddress address, zpointer previous);
    static zpointer ColorStoreGood(zaddress address, zpointer previous);
    static zpointer ColorRemsetGood(zaddress address, zpointer previous);
    static zaddress MarkSlowPath(zaddress address);
    static zaddress MarkFromOldSlowPath(zaddress address);
    static zaddress MarkFromYoungSlowPath(zaddress address);
    template<bool resurrect, bool gcThread, bool follow, bool finalizable>
    static void Mark(zaddress addr);
    static void MarkBarrierOnOopField(RefField<>& field, bool finalizable);
    static bool IsMarkYoungGoodFastPath(zpointer value);
    static zpointer ColorMarkYoungGood(zaddress address, zpointer previous);
    static zaddress MarkYoungSlowPath(zaddress address);
    static void MarkIfYoung(zaddress address);
    template<bool resurrect, bool gcThread, bool follow>
    static void MarkYoung(zaddress address);

    static zaddress relocate_or_remap(zaddress_unsafe addr, ZGeneration* generation);
    static zaddress remap(zaddress_unsafe addr, ZGeneration* generation);
    static zaddress keep_alive_slow_path(zaddress addr);
    static zaddress blocking_keep_alive_on_weak_slow_path(volatile zpointer* p, zaddress addr);
    static zaddress blocking_keep_alive_on_phantom_slow_path(volatile zpointer* p, zaddress addr);
    static zaddress blocking_load_barrier_on_phantom_slow_path(volatile zpointer* p, zaddress addr);
    static zpointer ColorLoadGood(zaddress address, zpointer previous);
    static zaddress promote_slow_path(zaddress addr);
    static void promote_barrier_on_young_oop_field(volatile zpointer* p);
private:
};

} // namespace MapleRuntime
#endif // ~MRT_BARRIER_H
