// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Mutator/ThreadLocal.h"


namespace MapleRuntime {
// ZMark::mark_object (zMark.inline.hpp:48-87). Input is already current.
template<bool resurrect, bool gcThread, bool follow, bool finalizable>
inline void ZMark::MarkObject(zaddress address)
{
    (void)to_object(address); // ZMark entry validates the current oop before the page query.
    ZDiagIdentityStale("mark.MarkObject.push", raw(address), 0, "none");
    ZPage* page = Heap::page(raw(address));
    if (page->IsAllocating()) {
        return;
    }

    const bool markBeforePush = gcThread;
    bool incLive = false;
    if (markBeforePush) {
        if (!page->mark_object(address, finalizable, incLive)) {
            return;
        }
    } else {
        if (page->is_object_marked(address, finalizable)) {
            return;
        }
    }

    if (resurrect) {
        terminate.SetResurrected(true);
    }
    MarkThreadLocalStacks& stacks = Stacks();
    const size_t stripe = stripes.StripeForAddress(raw(address));
    const MarkStackEntry entry(untype(ZAddress::offset(address)), !markBeforePush, incLive, follow, finalizable);
    CHECK(page->IsYoungRegion() == (generation == MarkingStacks::MarkingGeneration::YOUNG));
    const bool publish = !gcThread;
    stacks.Push(stripes, stripe, entry, publish);
}
}
