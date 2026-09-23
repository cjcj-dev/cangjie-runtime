#ifndef MRT_Z_ACCESS_BACKEND_HPP
#define MRT_Z_ACCESS_BACKEND_HPP

#include "ObjectModel/RefField.inline.h"
#include "Heap/z/zValuePayload.hpp"
#include <algorithm>
#include <cstring>

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

namespace AccessInternal {
// utilities/copy.cpp:44-86: aligned atomic segments followed by a byte tail.
template<typename T>
inline void copy_aligned_segment(MAddress& src, MAddress& dst, size_t& remaining, uintptr_t bits)
{
    if ((bits & (sizeof(T) - 1)) == 0 && remaining >= sizeof(T)) {
        const size_t count = remaining / sizeof(T);
        auto* from = reinterpret_cast<T*>(src);
        auto* to = reinterpret_cast<T*>(dst);
        if (src < dst) {
            for (size_t i = count; i > 0; --i) {
                __atomic_store_n(to + i - 1, __atomic_load_n(from + i - 1, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            }
        } else if (src > dst) {
            for (size_t i = 0; i < count; ++i) {
                __atomic_store_n(to + i, __atomic_load_n(from + i, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            }
        }
        src += count * sizeof(T);
        dst += count * sizeof(T);
        remaining -= count * sizeof(T);
    }
}
inline void value_copy_internal(MAddress src, MAddress dst, size_t size)
{
    const uintptr_t bits = src | dst;
    copy_aligned_segment<uint64_t>(src, dst, size, bits);
    copy_aligned_segment<uint32_t>(src, dst, size, bits);
    copy_aligned_segment<uint16_t>(src, dst, size, bits);
    if (size > 0) { std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), size); }
}
} // namespace AccessInternal

// oops/accessBackend.hpp: RawAccessBarrier owns memory ordering, never barriers.
template<DecoratorSet decorators>
class RawAccessBarrier {
    static HeapSlot<>& slot(volatile zpointer* p) { return *reinterpret_cast<HeapSlot<>*>(const_cast<zpointer*>(p)); }
public:
    // oops/accessBackend.inline.hpp:225-240: oop copies are always atomic.
    static void oop_arraycopy(zpointer* src, zpointer* dst, size_t length)
    {
        if (src < dst) {
            for (size_t i = length; i > 0; --i) { store(dst + i - 1, load(src + i - 1)); }
        } else if (src > dst) {
            for (size_t i = 0; i < length; ++i) { store(dst + i, load(src + i)); }
        }
    }
    static void oop_arraycopy(BaseObject*, MAddress src, size_t srcSize,
                              BaseObject*, MAddress dst, size_t dstSize)
    {
        oop_arraycopy(reinterpret_cast<zpointer*>(src), reinterpret_cast<zpointer*>(dst),
                      std::min(srcSize, dstSize) / sizeof(zpointer));
    }
    static void value_copy(const ValuePayload& src, const ValuePayload& dst)
    {
        CHECK(src.size <= dst.size);
        AccessInternal::value_copy_internal(src.address, dst.address, src.size);
    }
    // Cangjie inline-value arrays use the same raw payload copy.
    static void value_arraycopy(BaseObject*, MAddress src, size_t srcSize,
                                BaseObject*, MAddress dst, size_t dstSize)
    {
        CHECK(srcSize <= dstSize);
        AccessInternal::value_copy_internal(src, dst, srcSize);
    }
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
