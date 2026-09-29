// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#ifndef MRT_Z_UNCOLORED_ROOT_HPP
#define MRT_Z_UNCOLORED_ROOT_HPP

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zIterator.hpp"

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

    static zaddress_unsafe* cast(RefField<>* p);
    using RootFunction = void (*)(zaddress_unsafe*, uintptr_t);
    using ObjectFunction = void (*)(zaddress);
};

// zUncoloredRoot.hpp:87-94: the VM slot adapter dispatches to the root operation.
class ZUncoloredRootClosure : public OopClosure {
private:
    void do_oop(RefField<>* p) final;
public:
    virtual void do_root(zaddress_unsafe* p) = 0;
};

class ZUncoloredRootMarkOopClosure : public ZUncoloredRootClosure {
private:
    const uintptr_t _color;
public:
    explicit ZUncoloredRootMarkOopClosure(uintptr_t color);
    void do_root(zaddress_unsafe* p) override;
};

class ZUncoloredRootMarkYoungOopClosure : public ZUncoloredRootClosure {
private:
    const uintptr_t _color;
public:
    explicit ZUncoloredRootMarkYoungOopClosure(uintptr_t color);
    void do_root(zaddress_unsafe* p) override;
};

class ZUncoloredRootProcessOopClosure : public ZUncoloredRootClosure {
private:
    const uintptr_t _color;
public:
    explicit ZUncoloredRootProcessOopClosure(uintptr_t color);
    void do_root(zaddress_unsafe* p) override;
};

class ZUncoloredRootProcessWeakOopClosure : public ZUncoloredRootClosure {
private:
    const uintptr_t _color;
public:
    explicit ZUncoloredRootProcessWeakOopClosure(uintptr_t color);
    void do_root(zaddress_unsafe* p) override;
};

class ZUncoloredRootProcessNoKeepaliveOopClosure : public ZUncoloredRootClosure {
private:
    const uintptr_t _color;
public:
    explicit ZUncoloredRootProcessNoKeepaliveOopClosure(uintptr_t color);
    void do_root(zaddress_unsafe* p) override;
};
} // namespace MapleRuntime

#endif
