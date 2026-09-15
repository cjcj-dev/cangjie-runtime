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
    BaseObject* object = to_object(address);
    RegionInfo* page = RegionInfo::GetRegionInfoAt(raw(address));
    if (page->IsAllocating()) {
        return;
    }

    const bool markBeforePush = gcThread;
    bool incLive = false;
    if (markBeforePush) {
        const bool marked = finalizable
            ? page->ResurrectObjectWithLiveClaim(object, page->GetAddressOffset(raw(address)), false, incLive)
            : page->MarkObjectByOwnerWithLiveClaim(object, object->GetSize(), false, incLive);
        if (!marked) {
            return;
        }
    } else {
        const bool marked = page->IsYoungRegion()
            ? page->IsMarkedObject(page->GetMarkView<MapleRuntime::Generation::Young>(), object)
            : (finalizable ? page->IsSurvivedObject(page->GetMarkView<MapleRuntime::Generation::Old>(),
                                                   page->GetAddressOffset(raw(address)))
                           : page->IsMarkedObject(page->GetMarkView<MapleRuntime::Generation::Old>(), object));
        if (marked) {
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
