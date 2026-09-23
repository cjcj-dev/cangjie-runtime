#ifndef MRT_WEAK_HANDLE_INLINE_H
#define MRT_WEAK_HANDLE_INLINE_H

#include "Common/WeakHandle.h"
#include "Heap/z/zAccess.hpp"
#include "Base/Log.h"

namespace MapleRuntime {
inline BaseObject* WeakHandle::resolve() const
{
    CHECK_DETAIL(!is_null(), "WeakHandle resolve on empty handle");
    return NativeAccess<ON_PHANTOM_OOP_REF>::oop_load(obj);
}

inline BaseObject* WeakHandle::peek() const
{
    CHECK_DETAIL(!is_null(), "WeakHandle peek on empty handle");
    return NativeAccess<ON_PHANTOM_OOP_REF | AS_NO_KEEPALIVE>::oop_load(obj);
}

inline void WeakHandle::replace(BaseObject* withObj)
{
    CHECK_DETAIL(!is_empty(), "WeakHandle replace on empty handle");
    NativeAccess<ON_PHANTOM_OOP_REF>::oop_store(obj, withObj);
}
} // namespace MapleRuntime
#endif
