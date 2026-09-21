// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#pragma once
#include "Heap/z/zUncoloredRoot.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zVerify.hpp"
#include "Heap/z/zBarrier.inline.hpp"

namespace MapleRuntime {
template<typename ObjectFunctionT>
inline void ZUncoloredRoot::barrier(ObjectFunctionT function, zaddress_unsafe* p, uintptr_t color)
{
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
    z_verify_safepoints_are_blocked();
#endif
    const zaddress_unsafe addr = *p;
    if (is_null(addr)) {
        return;
    }
    const zaddress loadGood = make_load_good(addr, color);
    function(loadGood);
    *p = to_zaddress_unsafe(untype(loadGood));
}

inline zaddress ZUncoloredRoot::make_load_good(zaddress_unsafe addr, uintptr_t color)
{
    const zpointer colorPtr = ZAddress::color(zaddress::null, color);
    ZDiagIdentityStale("uncolored.make_load_good", untype(addr), color, "arg-color");
    if (!ZPointer::is_load_good(colorPtr)) {
        return ZBarrier::relocate_or_remap(addr, ZBarrier::remap_generation(colorPtr));
    }
    return safe(addr);
}

inline void ZUncoloredRoot::mark_object(zaddress addr)
{
    ZDiagIdentityStale("uncolored.mark_object", untype(addr), 0, "none");
    ZBarrier::Mark<false, false, true, false>(addr);
}

inline void ZUncoloredRoot::mark_invisible_object(zaddress addr)
{
    ZBarrier::Mark<false, false, false, false>(addr);
}

inline void ZUncoloredRoot::keep_alive_object(zaddress addr)
{
    ZBarrier::Mark<true, false, true, false>(addr);
}

inline void ZUncoloredRoot::mark_young_object(zaddress addr)
{
    ZBarrier::MarkIfYoung(addr);
}

inline void ZUncoloredRoot::mark(zaddress_unsafe* p, uintptr_t color)
{
    barrier(mark_object, p, color);
}

inline void ZUncoloredRoot::mark_young(zaddress_unsafe* p, uintptr_t color)
{
    barrier(mark_young_object, p, color);
}

inline void ZUncoloredRoot::process(zaddress_unsafe* p, uintptr_t color)
{
    barrier(mark_object, p, color);
}

inline void ZUncoloredRoot::process_invisible(zaddress_unsafe* p, uintptr_t color)
{
    barrier(mark_invisible_object, p, color);
}

inline void ZUncoloredRoot::process_weak(zaddress_unsafe* p, uintptr_t color)
{
    barrier(keep_alive_object, p, color);
}

inline void ZUncoloredRoot::process_no_keepalive(zaddress_unsafe* p, uintptr_t color)
{
    barrier([](zaddress) {}, p, color);
}

inline zaddress_unsafe* ZUncoloredRoot::cast(BaseObject** p)
{
    return reinterpret_cast<zaddress_unsafe*>(p);
}
} // namespace MapleRuntime
