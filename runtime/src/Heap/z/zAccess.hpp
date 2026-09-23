#ifndef MRT_Z_ACCESS_HPP
#define MRT_Z_ACCESS_HPP

#include "Heap/z/zBarrierSet.hpp"

namespace MapleRuntime {
// oops/access.hpp:262-306. Static storage/strength decorators select one backend.
template<DecoratorSet decorators = DECORATORS_NONE>
class Access {
    static constexpr DecoratorSet fixed = decorators | ((decorators & OOP_REF_MASK) ? 0 : ON_STRONG_OOP_REF);
    using Barrier = ZBarrierSet::AccessBarrier<fixed>;
    using Raw = RawAccessBarrier<fixed>;
public:
    template<typename P> static BaseObject* oop_load(P* p)
    {
        auto* field = reinterpret_cast<volatile zpointer*>(p);
        if constexpr (decorators & AS_RAW) { return reinterpret_cast<BaseObject*>(raw(Raw::load(field))); }
        else if constexpr (decorators & IN_HEAP) { return Barrier::oop_load_in_heap(field); }
        else { return Barrier::oop_load_not_in_heap(field); }
    }
    template<typename P> static void oop_store(P* p, BaseObject* value)
    {
        auto* field = reinterpret_cast<volatile zpointer*>(p);
        if constexpr (decorators & AS_RAW) { Raw::store(field, to_zpointer(reinterpret_cast<uintptr_t>(value))); }
        else if constexpr (decorators & IN_HEAP) { Barrier::oop_store_in_heap(field, value); }
        else { Barrier::oop_store_not_in_heap(field, value); }
    }
    static BaseObject* oop_load_at(BaseObject* base, ptrdiff_t offset)
    {
        if constexpr (decorators & AS_RAW) {
            return oop_load(reinterpret_cast<volatile zpointer*>(reinterpret_cast<uintptr_t>(base) + offset));
        } else { return Barrier::oop_load_in_heap_at(base, offset); }
    }
    static void oop_store_at(BaseObject* base, ptrdiff_t offset, BaseObject* value)
    {
        if constexpr (decorators & AS_RAW) {
            oop_store(reinterpret_cast<volatile zpointer*>(reinterpret_cast<uintptr_t>(base) + offset), value);
        } else { Barrier::oop_store_in_heap_at(base, offset, value); }
    }
    static BaseObject* oop_atomic_xchg_at(BaseObject* base, ptrdiff_t offset, BaseObject* value)
    {
        if constexpr (decorators & AS_RAW) {
            return oop_atomic_xchg(reinterpret_cast<volatile zpointer*>(reinterpret_cast<uintptr_t>(base) + offset), value);
        } else { return Barrier::oop_atomic_xchg_in_heap_at(base, offset, value); }
    }
    static BaseObject* oop_atomic_cmpxchg_at(BaseObject* base, ptrdiff_t offset, BaseObject* compare, BaseObject* value)
    {
        if constexpr (decorators & AS_RAW) {
            return oop_atomic_cmpxchg(reinterpret_cast<volatile zpointer*>(reinterpret_cast<uintptr_t>(base) + offset), compare, value);
        } else { return Barrier::oop_atomic_cmpxchg_in_heap_at(base, offset, compare, value); }
    }
    template<typename P> static BaseObject* oop_atomic_xchg(P* p, BaseObject* value)
    {
        auto* field = reinterpret_cast<volatile zpointer*>(p);
        if constexpr (decorators & AS_RAW) { return reinterpret_cast<BaseObject*>(raw(Raw::atomic_xchg(field, to_zpointer(reinterpret_cast<uintptr_t>(value))))); }
        else if constexpr (decorators & IN_HEAP) { return Barrier::oop_atomic_xchg_in_heap(field, value); }
        else { return Barrier::oop_atomic_xchg_not_in_heap(field, value); }
    }
    template<typename P> static BaseObject* oop_atomic_cmpxchg(P* p, BaseObject* compare, BaseObject* value)
    {
        auto* field = reinterpret_cast<volatile zpointer*>(p);
        if constexpr (decorators & AS_RAW) { return reinterpret_cast<BaseObject*>(raw(Raw::atomic_cmpxchg(field,
            to_zpointer(reinterpret_cast<uintptr_t>(compare)), to_zpointer(reinterpret_cast<uintptr_t>(value))))); }
        else if constexpr (decorators & IN_HEAP) { return Barrier::oop_atomic_cmpxchg_in_heap(field, compare, value); }
        else { return Barrier::oop_atomic_cmpxchg_not_in_heap(field, compare, value); }
    }
    static void value_copy(const ValuePayload& src, const ValuePayload& dst) { Barrier::value_copy_in_heap(src, dst); }
    static void oop_arraycopy(BaseObject* srcObj, MAddress src, size_t srcSize,
                              BaseObject* dstObj, MAddress dst, size_t dstSize)
    { Barrier::oop_arraycopy_in_heap(srcObj, src, srcSize, dstObj, dst, dstSize); }
    static void value_arraycopy(BaseObject* srcObj, MAddress src, size_t srcSize,
                                BaseObject* dstObj, MAddress dst, size_t dstSize)
    { Barrier::value_arraycopy_in_heap(srcObj, src, srcSize, dstObj, dst, dstSize); }
    static void oop_arraycopy(zpointer* src, zpointer* dst, size_t length) { Barrier::oop_arraycopy_in_heap(src, dst, length); }
};
template<DecoratorSet decorators = DECORATORS_NONE>
class HeapAccess : public Access<IN_HEAP | decorators> {};
template<DecoratorSet decorators = DECORATORS_NONE>
class NativeAccess : public Access<IN_NATIVE | decorators> {};
template<DecoratorSet decorators = DECORATORS_NONE>
class RawAccess : public Access<AS_RAW | decorators> {};
} // namespace MapleRuntime
#endif
