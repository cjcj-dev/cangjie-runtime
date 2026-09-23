#ifndef MRT_Z_ACCESS_HPP
#define MRT_Z_ACCESS_HPP

#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"

namespace MapleRuntime {
enum DecoratorSet : uint64_t {
    ON_PHANTOM_OOP_REF = 1ull << 0,
    AS_NO_KEEPALIVE = 1ull << 1,
};

template<uint64_t Decorators>
class NativeAccess {
public:
    static BaseObject* oop_load(NativeSlot* p);
    static void oop_store(NativeSlot* p, BaseObject* value);
};

template<uint64_t Decorators>
inline BaseObject* NativeAccess<Decorators>::oop_load(NativeSlot* p)
{
    volatile zpointer* field = reinterpret_cast<volatile zpointer*>(p);
    const zpointer observed = ZBarrier::load_atomic(field);
    if ((Decorators & AS_NO_KEEPALIVE) != 0) {
        return to_object(ZBarrier::no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(field, observed));
    }
    return to_object(ZBarrier::load_barrier_on_phantom_oop_field_preloaded(field, observed));
}

template<uint64_t Decorators>
inline void NativeAccess<Decorators>::oop_store(NativeSlot* p, BaseObject* value)
{
    volatile zpointer* field = reinterpret_cast<volatile zpointer*>(p);
    *const_cast<zpointer*>(field) = ZAddress::store_good(from_object(value));
}
} // namespace MapleRuntime
#endif
