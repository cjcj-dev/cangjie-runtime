#ifndef MRT_Z_ACCESS_RUNTIME_DISPATCH_HPP
#define MRT_Z_ACCESS_RUNTIME_DISPATCH_HPP

#include "Base/Log.h"
#include "Heap/z/zBarrierSet.inline.hpp"

namespace MapleRuntime {
namespace AccessInternal {
enum BarrierType {
    BARRIER_LOAD_HEAP,
    BARRIER_LOAD_NATIVE,
    BARRIER_STORE_HEAP,
    BARRIER_STORE_NATIVE,
    BARRIER_LOAD_AT,
    BARRIER_STORE_AT,
    BARRIER_ATOMIC_XCHG_HEAP,
    BARRIER_ATOMIC_XCHG_NATIVE,
    BARRIER_ATOMIC_XCHG_AT,
    BARRIER_ATOMIC_CMPXCHG_HEAP,
    BARRIER_ATOMIC_CMPXCHG_NATIVE,
    BARRIER_ATOMIC_CMPXCHG_AT,
    BARRIER_VALUE_COPY,
    BARRIER_OOP_ARRAYCOPY,
    BARRIER_VALUE_ARRAYCOPY,
    BARRIER_POINTER_ARRAYCOPY
};

template<typename BarrierSetT, BarrierType barrier_type, DecoratorSet decorators>
struct PostRuntimeDispatch;

template<DecoratorSet decorators, typename FunctionPointerT, BarrierType barrier_type>
struct BarrierResolver {
    static FunctionPointerT resolve_barrier_gc()
    {
        BarrierSet* barrier_set = BarrierSet::barrier_set();
        CHECK_DETAIL(barrier_set != nullptr, "GC barriers invoked before BarrierSet is set");
        switch (barrier_set->kind()) {
            case BarrierSet::ZBarrierSetKind:
                return PostRuntimeDispatch<
                    typename BarrierSet::GetType<BarrierSet::ZBarrierSetKind>::type::AccessBarrier<decorators>,
                    barrier_type, decorators>::access_barrier;
            default:
                CHECK_DETAIL(false, "BarrierSet AccessBarrier resolving not implemented");
                return nullptr;
        }
    }

    static FunctionPointerT resolve_barrier()
    {
        return resolve_barrier_gc();
    }
};

template<DecoratorSet decorators, BarrierType barrier_type>
struct RuntimeDispatch;

#define MRT_ACCESS_RUNTIME_DISPATCH(kind, result, method, parameters, arguments) \
    template<typename BarrierSetT, DecoratorSet decorators> \
    struct PostRuntimeDispatch<BarrierSetT, kind, decorators> { \
        static result access_barrier parameters { return BarrierSetT::method arguments; } \
    }; \
    template<DecoratorSet decorators> \
    struct RuntimeDispatch<decorators, kind> { \
        using func_t = result (*) parameters; \
        static result init parameters { \
            func_t function = BarrierResolver<decorators, func_t, kind>::resolve_barrier(); \
            _func = function; \
            return function arguments; \
        } \
        inline static func_t _func = init; \
        static result method parameters { return _func arguments; } \
    };

MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_LOAD_HEAP, BaseObject*, oop_load_in_heap,
    (volatile zpointer* field), (field))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_LOAD_NATIVE, BaseObject*, oop_load_not_in_heap,
    (volatile zpointer* field), (field))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_STORE_HEAP, void, oop_store_in_heap,
    (volatile zpointer* field, BaseObject* value), (field, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_STORE_NATIVE, void, oop_store_not_in_heap,
    (volatile zpointer* field, BaseObject* value), (field, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_LOAD_AT, BaseObject*, oop_load_in_heap_at,
    (BaseObject* base, ptrdiff_t offset), (base, offset))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_STORE_AT, void, oop_store_in_heap_at,
    (BaseObject* base, ptrdiff_t offset, BaseObject* value), (base, offset, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_ATOMIC_XCHG_HEAP, BaseObject*, oop_atomic_xchg_in_heap,
    (volatile zpointer* field, BaseObject* value), (field, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_ATOMIC_XCHG_NATIVE, BaseObject*, oop_atomic_xchg_not_in_heap,
    (volatile zpointer* field, BaseObject* value), (field, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_ATOMIC_XCHG_AT, BaseObject*, oop_atomic_xchg_in_heap_at,
    (BaseObject* base, ptrdiff_t offset, BaseObject* value), (base, offset, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_ATOMIC_CMPXCHG_HEAP, BaseObject*, oop_atomic_cmpxchg_in_heap,
    (volatile zpointer* field, BaseObject* compare, BaseObject* value), (field, compare, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_ATOMIC_CMPXCHG_NATIVE, BaseObject*, oop_atomic_cmpxchg_not_in_heap,
    (volatile zpointer* field, BaseObject* compare, BaseObject* value), (field, compare, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_ATOMIC_CMPXCHG_AT, BaseObject*, oop_atomic_cmpxchg_in_heap_at,
    (BaseObject* base, ptrdiff_t offset, BaseObject* compare, BaseObject* value), (base, offset, compare, value))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_VALUE_COPY, void, value_copy_in_heap,
    (const ValuePayload& src, const ValuePayload& dst), (src, dst))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_OOP_ARRAYCOPY, void, oop_arraycopy_in_heap,
    (BaseObject* srcObj, MAddress src, BaseObject* dstObj, MAddress dst, size_t length),
    (srcObj, src, dstObj, dst, length))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_VALUE_ARRAYCOPY, void, value_arraycopy_in_heap,
    (MArray* layout, MAddress src, MAddress dst, size_t length), (layout, src, dst, length))
MRT_ACCESS_RUNTIME_DISPATCH(BARRIER_POINTER_ARRAYCOPY, void, oop_arraycopy_in_heap,
    (zpointer* src, zpointer* dst, size_t length), (src, dst, length))

#undef MRT_ACCESS_RUNTIME_DISPATCH

template<DecoratorSet decorators>
struct RuntimeAccessBarrier :
    RuntimeDispatch<decorators, BARRIER_LOAD_HEAP>,
    RuntimeDispatch<decorators, BARRIER_LOAD_NATIVE>,
    RuntimeDispatch<decorators, BARRIER_STORE_HEAP>,
    RuntimeDispatch<decorators, BARRIER_STORE_NATIVE>,
    RuntimeDispatch<decorators, BARRIER_LOAD_AT>,
    RuntimeDispatch<decorators, BARRIER_STORE_AT>,
    RuntimeDispatch<decorators, BARRIER_ATOMIC_XCHG_HEAP>,
    RuntimeDispatch<decorators, BARRIER_ATOMIC_XCHG_NATIVE>,
    RuntimeDispatch<decorators, BARRIER_ATOMIC_XCHG_AT>,
    RuntimeDispatch<decorators, BARRIER_ATOMIC_CMPXCHG_HEAP>,
    RuntimeDispatch<decorators, BARRIER_ATOMIC_CMPXCHG_NATIVE>,
    RuntimeDispatch<decorators, BARRIER_ATOMIC_CMPXCHG_AT>,
    RuntimeDispatch<decorators, BARRIER_VALUE_COPY>,
    RuntimeDispatch<decorators, BARRIER_OOP_ARRAYCOPY>,
    RuntimeDispatch<decorators, BARRIER_VALUE_ARRAYCOPY>,
    RuntimeDispatch<decorators, BARRIER_POINTER_ARRAYCOPY> {
    using RuntimeDispatch<decorators, BARRIER_OOP_ARRAYCOPY>::oop_arraycopy_in_heap;
    using RuntimeDispatch<decorators, BARRIER_POINTER_ARRAYCOPY>::oop_arraycopy_in_heap;
};
}
}
#endif
