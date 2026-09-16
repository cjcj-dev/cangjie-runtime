// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#ifndef MRT_Z_UNCOLORED_ROOT_HPP
#define MRT_Z_UNCOLORED_ROOT_HPP

#include "Heap/z/zAddress.hpp"

namespace MapleRuntime {
class ZUncoloredRoot {
public:
    template<typename ObjectFunctionT>
    static void barrier(ObjectFunctionT function, zaddress_unsafe* p, uintptr_t color);
    static zaddress make_load_good(zaddress_unsafe addr, uintptr_t color);

    static void mark_object(zaddress addr);
    static void mark_invisible_object(zaddress addr);
    static void keep_alive_object(zaddress addr);
    static void mark_young_object(zaddress addr);

    static void mark(zaddress_unsafe* p, uintptr_t color);
    static void mark_young(zaddress_unsafe* p, uintptr_t color);
    static void process(zaddress_unsafe* p, uintptr_t color);
    static void process_invisible(zaddress_unsafe* p, uintptr_t color);
    static void process_weak(zaddress_unsafe* p, uintptr_t color);
    static void process_no_keepalive(zaddress_unsafe* p, uintptr_t color);

    static zaddress_unsafe* cast(BaseObject** p);
};
} // namespace MapleRuntime

#include "Heap/z/zUncoloredRoot.inline.hpp"
#endif
