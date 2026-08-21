// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_SLOT_LIST_H
#define MRT_SLOT_LIST_H

#include "Common/BaseObject.h"
#include "Heap/Collector/ManagedObjectGate.h"

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
        if (!ClearExtraContent(slot)) {
            // Do not link an uncleared, rejected slot: it would later be handed back as
            // pinned allocation storage. The slot stays stranded until region reclamation.
            return;
        }
        if (head != nullptr && MetadataWordHasColour(reinterpret_cast<Uptr>(head))) {
            head = nullptr;
        }
        headSlot->next = head;
        head = headSlot;
    }

    void Clear() { head = nullptr; }

    // Clear the rest memory of slot object if the slot object size is greater than ObjectSlot(16 Bytes).
    bool ClearExtraContent(BaseObject* slot)
    {
        if (!PlausibleManagedObjectGate("SlotList::ClearExtraContent", slot)) {
            // The caller treats false as "do not add to the free-slot list". Stale payload
            // remains uncleared, and this slot's individual reuse opportunity is lost.
            return false;
        }
        size_t size = slot->GetSize() - sizeof(ObjectSlot);
        if (size > 0) {
            MAddress start = reinterpret_cast<uintptr_t>(slot) + sizeof(ObjectSlot);
            CHECK_E((memset_s(reinterpret_cast<void*>(start), size, 0, size) != EOK), "memset_s fail");
        }
        return true;
    }

private:
    friend struct FreePinnedSlotLists;
    friend struct SlotListTestAccess;
    static bool MetadataWordHasColour(Uptr bits)
    {
        return bits != raw(uncolor_bits(to_zpointer(bits)));
    }

    uintptr_t PopFront(size_t size)
    {
        // getsizetrace / ZGC: ObjectSlot::next overlays Future payload+8, a
        // store-good heap ref (ColourMask.h bit56). That word is not a
        // successor slot. zPage::reset (zPage.cpp:103-121) recycles by
        // resetting page metadata, never by following a coloured oop as
        // a free-list next. Uncolor-and-hand-out would revive a live
        // Future. Drop the metadata chain; bump alloc still works.
        // AllocPinnedFromFreeList (RegionManager.cpp:3611) is the soak caller.
        if (head == nullptr) {
            return 0;
        }
        Uptr rawHead = reinterpret_cast<Uptr>(head);
        if (MetadataWordHasColour(rawHead)) {
            head = nullptr;
            return 0;
        }
        BaseObject* obj = from_region_addr(rawHead);
        if (!PlausibleManagedObjectGate("SlotList::PopFront", obj) || size != obj->GetSize()) {
            return 0;
        }
        ObjectSlot* allocSlot = head;
        Uptr rawNext = reinterpret_cast<Uptr>(allocSlot->next);
        head = MetadataWordHasColour(rawNext) ? nullptr : allocSlot->next;
        allocSlot->next = nullptr;
        return reinterpret_cast<uintptr_t>(allocSlot);
    }

    ObjectSlot* head = nullptr;
};
} // namespace MapleRuntime
#endif // MRT_SLOT_LIST_H
