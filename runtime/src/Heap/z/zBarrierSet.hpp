// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#ifndef MRT_Z_BARRIER_SET_HPP
#define MRT_Z_BARRIER_SET_HPP

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zThreadLocalData.hpp"

namespace MapleRuntime {
class MArray;
class ZBarrierSet {
public:
    static bool barrier_needed(bool isReference);
    static void on_thread_attach(ThreadGCData& data, Mutator* owner, ThreadLocalData* native, zaddress_unsafe* root);
    static void on_thread_detach(ThreadGCData& data);
    static void on_thread_destroy(ThreadGCData& data);

    static zaddress oop_load_in_heap(volatile zpointer* p);
    static void oop_store_in_heap(volatile zpointer* p, zaddress value);
    static zaddress oop_xchg_in_heap(volatile zpointer* p, zaddress value);
    class AccessBarrier {
    public:
        static zaddress oop_copy_one_barriers(volatile zpointer* dst, volatile zpointer* src);
        static void oop_copy_one(volatile zpointer* dst, volatile zpointer* src);
        static void oop_clear_one(volatile zpointer* dst);
        static void oop_arraycopy_in_heap_no_check_cast(zpointer* dst, zpointer* src, size_t length);
        // Cangjie inline structs contain primitive bytes as well as oop slots.
        static void struct_copy_one(MArray* layout, MAddress dst, MAddress src);
        static void struct_arraycopy_in_heap_no_check_cast(MArray* layout, MAddress dst, MAddress src, size_t length);
    };
};

class ZBarrierSetRuntime {
public:
    static BaseObject* load_barrier_on_oop_field_preloaded(BaseObject* o, volatile zpointer* p);
    static BaseObject* load_barrier_on_weak_oop_field_preloaded(BaseObject* o, volatile zpointer* p);
    static BaseObject* load_barrier_on_phantom_oop_field_preloaded(BaseObject* o, volatile zpointer* p);
    static void store_barrier_on_oop_field_with_healing(volatile zpointer* p);
    static void store_barrier_on_oop_field_without_healing(volatile zpointer* p);
    static void store_barrier_on_oop_field_without_healing_no_keep_alive(volatile zpointer* p);
    static void store_barrier_on_native_oop_field_without_healing(volatile zpointer* p);
    static void load_barrier_on_oop_array(volatile zpointer* p, size_t length);

    static void* load_barrier_on_oop_field_preloaded_addr();
    static void* load_barrier_on_weak_oop_field_preloaded_addr();
    static void* load_barrier_on_phantom_oop_field_preloaded_addr();
    static void* store_barrier_on_oop_field_with_healing_addr();
    static void* store_barrier_on_oop_field_without_healing_addr();
    static void* store_barrier_on_native_oop_field_without_healing_addr();
    static void* load_barrier_on_oop_array_addr();
};
} // namespace MapleRuntime

#include "Heap/z/zBarrierSet.inline.hpp"
#endif
