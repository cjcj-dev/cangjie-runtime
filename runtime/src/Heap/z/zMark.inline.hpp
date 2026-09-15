// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zMark.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Mutator/ThreadLocal.h"


namespace MapleRuntime {
// ZMark::mark_object (zMark.inline.hpp:48-87). Input is already current.
template<bool resurrect, bool gcThread, bool follow, bool finalizable>
inline void MarkDomain::MarkObject(zaddress address)
{
    RegionInfo* page = RegionInfo::GetRegionInfoAt(raw(address));
    if (page->IsAllocating()) {
        return;
    }

    const bool markBeforePush = gcThread;
    bool incLive = false;
    if (markBeforePush) {
        if (!page->MarkObject(address, finalizable, incLive)) {
            return;
        }
    } else {
        if (page->IsObjectMarked(address, finalizable)) {
            return;
        }
    }

    if (resurrect) {
        terminate.SetResurrected(true);
    }
    MarkThreadLocalStacks& stacks = Stacks();
    const size_t stripe = stripes.StripeForAddress(raw(address));
    const zoffset offset = static_cast<zoffset>(raw(address) - MarkStackEntry::HeapBase());
    const MarkStackEntry entry(offset, !markBeforePush, incLive, follow, finalizable);
    CHECK(page->IsYoungRegion() == (generation == MarkingStacks::MarkingGeneration::YOUNG));
    const bool publish = !gcThread;
    stacks.Push(stripes, stripe, entry, publish);
}
}
