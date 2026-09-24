#include "Heap/z/accessBarrierSupport.inline.hpp"
#include "Common/BaseObject.h"

namespace MapleRuntime {
DecoratorSet AccessBarrierSupport::resolve_unknown_oop_ref_strength(DecoratorSet decorators, BaseObject* base,
                                                                    ptrdiff_t offset)
{
    DecoratorSet ds = decorators & ~ON_UNKNOWN_OOP_REF;
    if (!BaseObject::is_referent_field(base, offset)) {
        ds |= ON_STRONG_OOP_REF;
    } else {
        ds |= ON_WEAK_OOP_REF;
    }
    return ds;
}
}
