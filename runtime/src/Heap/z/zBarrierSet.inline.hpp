#ifndef MRT_Z_BARRIER_SET_INLINE_HPP
#define MRT_Z_BARRIER_SET_INLINE_HPP

#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zBarrierSet.hpp"

namespace MapleRuntime {
inline zaddress ZBarrierSet::AccessBarrier::oop_copy_one_barriers(volatile zpointer* dst, volatile zpointer* src)
{
    ZBarrier::store_barrier_on_heap_oop_field(dst, false);
    return ZBarrier::load_barrier_on_oop_field(src);
}

inline void ZBarrierSet::AccessBarrier::oop_copy_one(volatile zpointer* dst, volatile zpointer* src)
{
    const zaddress obj = oop_copy_one_barriers(dst, src);
    *const_cast<zpointer*>(dst) = ZAddress::store_good(obj);
}

inline void ZBarrierSet::AccessBarrier::oop_clear_one(volatile zpointer* dst)
{
    ZBarrier::store_barrier_on_heap_oop_field(dst, false);
    *const_cast<zpointer*>(dst) = ZAddress::store_good(zaddress::null);
}

inline void ZBarrierSet::AccessBarrier::oop_arraycopy_in_heap_no_check_cast(
    zpointer* dst, zpointer* src, size_t length)
{
    // ZGC zBarrierSet.inline.hpp:395-424. These entry points permit overlap.
    if (src > dst) {
        for (const zpointer* const end = src + length; src < end; src++, dst++) {
            oop_copy_one(dst, src);
        }
        return;
    }
    if (src < dst) {
        const zpointer* const end = src;
        src += length - 1;
        dst += length - 1;
        for (; src >= end; src--, dst--) {
            oop_copy_one(dst, src);
        }
        return;
    }
    // src and dst are the same; nothing to do.
}
} // namespace MapleRuntime
#endif
