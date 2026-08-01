// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_SLOT_LIST_H
#define MRT_SLOT_LIST_H

#include "Common/BaseObject.h"

namespace MapleRuntime {
struct ObjectSlot {
    StateWord stateWord; // same with BaseObject::stateWord
    ObjectSlot* next;
};

class SlotList {
public:
    void PushFront(BaseObject* slot)
    {
        ObjectSlot* headSlot = reinterpret_cast<ObjectSlot*>(slot);
        ClearExtraContent(slot);
        headSlot->next = head;
        head = headSlot;
    }

    void Clear() { head = nullptr; }

    // Clear the rest memory of slot object if the slot object size is greater than ObjectSlot(16 Bytes).
    void ClearExtraContent(BaseObject* slot)
    {
        size_t size = GetSize(ProvenByPinnedSlot(slot)) - sizeof(ObjectSlot);
        if (size > 0) {
            MAddress start = reinterpret_cast<uintptr_t>(slot) + sizeof(ObjectSlot);
            CHECK_E((memset_s(reinterpret_cast<void*>(start), size, 0, size) != EOK), "memset_s fail");
        }
    }

private:
    // Popping a slot revives memory inside a region whose retained census may
    // already record that address as dead (see the census invariant at
    // RegionInfo::PreserveRetainedLiveInfo). Only the guarded revive entry —
    // RegionManager::AllocPinnedFromFreeList via FreePinnedSlotLists — may pop,
    // so a new reuse path cannot be added without meeting the invariant.
    friend struct FreePinnedSlotLists;
    uintptr_t PopFront(size_t size)
    {
        if (head == nullptr ||
            size != GetSize(ProvenByPinnedSlot(reinterpret_cast<BaseObject*>(head)))) {
            return 0;
        }
        ObjectSlot* allocSlot = head;
        head = head->next;
        allocSlot->next = nullptr;
        return reinterpret_cast<uintptr_t>(allocSlot);
    }

    ObjectSlot* head = nullptr;
};
} // namespace MapleRuntime
#endif // MRT_SLOT_LIST_H
