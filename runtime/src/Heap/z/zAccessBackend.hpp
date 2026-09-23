#ifndef MRT_Z_ACCESS_BACKEND_HPP
#define MRT_Z_ACCESS_BACKEND_HPP

#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
using DecoratorSet = uint64_t;
constexpr DecoratorSet DECORATORS_NONE = 0;
constexpr DecoratorSet ON_PHANTOM_OOP_REF = 1ull << 0;
constexpr DecoratorSet AS_NO_KEEPALIVE = 1ull << 1;
constexpr DecoratorSet ON_STRONG_OOP_REF = 1ull << 2;
constexpr DecoratorSet ON_WEAK_OOP_REF = 1ull << 3;
constexpr DecoratorSet ON_UNKNOWN_OOP_REF = 1ull << 4;
constexpr DecoratorSet IN_HEAP = 1ull << 5;
constexpr DecoratorSet IN_NATIVE = 1ull << 6;
constexpr DecoratorSet AS_RAW = 1ull << 7;
constexpr DecoratorSet MO_UNORDERED = 1ull << 8;
constexpr DecoratorSet MO_RELAXED = 1ull << 9;
constexpr DecoratorSet MO_ACQUIRE = 1ull << 10;
constexpr DecoratorSet MO_RELEASE = 1ull << 11;
constexpr DecoratorSet MO_SEQ_CST = 1ull << 12;
constexpr DecoratorSet IS_DEST_UNINITIALIZED = 1ull << 13;
constexpr DecoratorSet OOP_REF_MASK = ON_STRONG_OOP_REF | ON_WEAK_OOP_REF | ON_PHANTOM_OOP_REF | ON_UNKNOWN_OOP_REF;

// oops/accessBackend.hpp: RawAccessBarrier owns memory ordering, never barriers.
template<DecoratorSet decorators>
class RawAccessBarrier {
    static HeapSlot<>& slot(volatile zpointer* p) { return *reinterpret_cast<HeapSlot<>*>(const_cast<zpointer*>(p)); }
public:
    static constexpr std::memory_order order()
    {
        return decorators & MO_SEQ_CST ? std::memory_order_seq_cst :
               decorators & MO_ACQUIRE ? std::memory_order_acquire :
               decorators & MO_RELEASE ? std::memory_order_release : std::memory_order_relaxed;
    }
    static zpointer load(volatile zpointer* p) { return slot(p).GetFieldValue(order()); }
    static zpointer load_in_heap(volatile zpointer* p) { return load(p); }
    static void store(volatile zpointer* p, zpointer value) { slot(p).StoreColoured(value, order()); }
    static void store_in_heap(volatile zpointer* p, zpointer value) { store(p, value); }
    static constexpr std::memory_order atomic_order()
    {
        return (decorators & (MO_UNORDERED | MO_RELAXED | MO_ACQUIRE | MO_RELEASE | MO_SEQ_CST)) ?
            order() : std::memory_order_seq_cst;
    }
    static zpointer atomic_xchg(volatile zpointer* p, zpointer value) { return slot(p).Exchange(value, atomic_order()); }
    static zpointer atomic_xchg_in_heap(volatile zpointer* p, zpointer value) { return atomic_xchg(p, value); }
    static zpointer atomic_cmpxchg(volatile zpointer* p, zpointer compare, zpointer value)
    {
        zpointer observed;
        constexpr auto failure = atomic_order() == std::memory_order_release ? std::memory_order_relaxed : atomic_order();
        slot(p).CompareExchange(compare, value, atomic_order(), failure, &observed);
        return observed;
    }
    static zpointer atomic_cmpxchg_in_heap(volatile zpointer* p, zpointer compare, zpointer value)
    { return atomic_cmpxchg(p, compare, value); }
};
} // namespace MapleRuntime
#endif
