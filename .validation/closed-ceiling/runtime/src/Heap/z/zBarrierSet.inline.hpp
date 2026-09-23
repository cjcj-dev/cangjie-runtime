#ifndef MRT_Z_BARRIER_SET_INLINE_HPP
#define MRT_Z_BARRIER_SET_INLINE_HPP

#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zBarrierSet.hpp"

namespace MapleRuntime {
inline zaddress ZBarrierSet::oop_copy_one_barriers(volatile zpointer* dst, volatile zpointer* src)
{
    ZBarrier::store_barrier_on_heap_oop_field(dst, false);
    return ZBarrier::load_barrier_on_oop_field(src);
}

inline void ZBarrierSet::oop_copy_one(volatile zpointer* dst, volatile zpointer* src)
{
    const zaddress obj = oop_copy_one_barriers(dst, src);
    *const_cast<zpointer*>(dst) = ZAddress::store_good(obj);
}

inline void ZBarrierSet::oop_clear_one(volatile zpointer* dst)
{
    ZBarrier::store_barrier_on_heap_oop_field(dst, false);
    *const_cast<zpointer*>(dst) = ZAddress::store_good(zaddress::null);
}
} // namespace MapleRuntime
#endif
