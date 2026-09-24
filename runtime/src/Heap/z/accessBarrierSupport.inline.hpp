#ifndef MRT_Z_ACCESS_BARRIER_SUPPORT_INLINE_HPP
#define MRT_Z_ACCESS_BARRIER_SUPPORT_INLINE_HPP

#include "Heap/z/accessBarrierSupport.hpp"

namespace MapleRuntime {
template<DecoratorSet decorators>
inline DecoratorSet AccessBarrierSupport::resolve_possibly_unknown_oop_ref_strength(BaseObject* base, ptrdiff_t offset)
{
    if constexpr ((decorators & ON_UNKNOWN_OOP_REF) == 0) {
        return decorators;
    } else {
        return resolve_unknown_oop_ref_strength(decorators, base, offset);
    }
}
}

#endif
