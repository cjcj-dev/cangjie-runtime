#ifndef MRT_Z_ACCESS_BARRIER_SUPPORT_HPP
#define MRT_Z_ACCESS_BARRIER_SUPPORT_HPP

#include "Heap/z/zAccessBackend.hpp"
#include <cstddef>

namespace MapleRuntime {
class BaseObject;

class AccessBarrierSupport {
private:
    static DecoratorSet resolve_unknown_oop_ref_strength(DecoratorSet decorators, BaseObject* base, ptrdiff_t offset);

public:
    template<DecoratorSet decorators>
    static DecoratorSet resolve_possibly_unknown_oop_ref_strength(BaseObject* base, ptrdiff_t offset);
};
}

#endif
