#ifndef MRT_Z_RESURRECTION_INLINE_HPP
#define MRT_Z_RESURRECTION_INLINE_HPP

#include "Heap/z/zResurrection.hpp"

namespace MapleRuntime {

inline bool ZResurrection::is_blocked()
{
    return blocked.load(std::memory_order_acquire);
}

} // namespace MapleRuntime
#endif
