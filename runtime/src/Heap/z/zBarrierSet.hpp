// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#ifndef MRT_Z_BARRIER_SET_HPP
#define MRT_Z_BARRIER_SET_HPP

#include "Common/BaseObject.h"
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zAccessBackend.hpp"
#include "Heap/z/zThreadLocalData.hpp"

namespace MapleRuntime {
class MArray;
class ZBarrierSet {
public:
    static void on_slowpath_allocation_exit(BaseObject* new_obj);
    static void on_thread_attach(ThreadGCData& data, Mutator* owner, ThreadLocalData* native, zaddress_unsafe* root);
    static void on_thread_detach(ThreadGCData& data);
    static void on_thread_destroy(ThreadGCData& data);

    template<DecoratorSet decorators, typename BarrierSetT = ZBarrierSet>
    class AccessBarrier {
        using Raw = RawAccessBarrier<decorators>;
        template<DecoratorSet expected> static void verify_decorators_present();
        template<DecoratorSet expected> static void verify_decorators_absent();
        static volatile zpointer* field_addr(BaseObject* base, ptrdiff_t offset);
        static zaddress load_barrier(volatile zpointer* p, zpointer observed);
        static zaddress load_barrier_on_unknown_oop_ref(BaseObject* base, ptrdiff_t offset,
                                                       volatile zpointer* p, zpointer observed);
        static void store_barrier_heap_with_healing(volatile zpointer* p);
        static void store_barrier_heap_without_healing(volatile zpointer* p);
        static void no_keep_alive_store_barrier_heap(volatile zpointer* p);
        static void store_barrier_native_with_healing(volatile zpointer* p);
        static void store_barrier_native_without_healing(volatile zpointer* p);
    public:
        static BaseObject* oop_load_in_heap(volatile zpointer* p);
        static BaseObject* oop_load_in_heap_at(BaseObject* base, ptrdiff_t offset);
        static BaseObject* oop_load_not_in_heap(volatile zpointer* p);
        static void oop_store_in_heap(volatile zpointer* p, BaseObject* value);
        static void oop_store_in_heap_at(BaseObject* base, ptrdiff_t offset, BaseObject* value);
        static void oop_store_not_in_heap(volatile zpointer* p, BaseObject* value);
        static BaseObject* oop_atomic_cmpxchg_in_heap(volatile zpointer* p, BaseObject* compare, BaseObject* value);
        static BaseObject* oop_atomic_cmpxchg_in_heap_at(BaseObject* base, ptrdiff_t offset, BaseObject* compare, BaseObject* value);
        static BaseObject* oop_atomic_cmpxchg_not_in_heap(volatile zpointer* p, BaseObject* compare, BaseObject* value);
        static BaseObject* oop_atomic_xchg_in_heap(volatile zpointer* p, BaseObject* value);
        static BaseObject* oop_atomic_xchg_in_heap_at(BaseObject* base, ptrdiff_t offset, BaseObject* value);
        static BaseObject* oop_atomic_xchg_not_in_heap(volatile zpointer* p, BaseObject* value);
        static zaddress oop_copy_one_barriers(volatile zpointer* dst, volatile zpointer* src);
        static void oop_copy_one(volatile zpointer* dst, volatile zpointer* src);
        static void oop_clear_one(volatile zpointer* dst);
        static void oop_arraycopy_in_heap_no_check_cast(zpointer* dst, zpointer* src, size_t length);
        static void oop_arraycopy_in_heap(zpointer* src, zpointer* dst, size_t length);
        static void value_copy_in_heap(const ValuePayload& src, const ValuePayload& dst);
        static void oop_arraycopy_in_heap(BaseObject* srcObj, MAddress src, size_t srcSize,
                                         BaseObject* dstObj, MAddress dst, size_t dstSize);
        static void value_arraycopy_in_heap(BaseObject* srcObj, MAddress src, size_t srcSize,
                                           BaseObject* dstObj, MAddress dst, size_t dstSize);
        static void struct_copy_one(MArray* layout, MAddress dst, MAddress src);
        static void struct_arraycopy_in_heap_no_check_cast(MArray* layout, MAddress dst, MAddress src, size_t length);
    };
};

class ZBarrierSetRuntime {
public:
    static BaseObject* load_barrier_on_oop_field_preloaded(BaseObject* o, volatile zpointer* p);
    static BaseObject* load_barrier_on_weak_oop_field_preloaded(BaseObject* o, volatile zpointer* p);
    static void store_barrier_on_oop_field_without_healing(volatile zpointer* p);
    static void store_barrier_on_oop_field_without_healing_no_keep_alive(volatile zpointer* p);
};
} // namespace MapleRuntime

#include "Heap/z/zBarrierSet.inline.hpp"
#endif
