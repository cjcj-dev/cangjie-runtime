// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// RegionList method bodies (old-directory residue; ZGC has no RegionList).
// Included once by zPageAllocator.cpp.

#include "Heap/Allocator/RegionList.h"

namespace MapleRuntime {
void RegionList::MergeRegionList(RegionList& srcList)
{
    RegionList regionList("region list cache");
    srcList.MoveTo(regionList);
    ZPage* head = regionList.GetHeadRegion();
    ZPage* tail = regionList.GetTailRegion();
    if (head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(listMutex);
    IncCounts(regionList.GetRegionCount(), regionList.GetUnitCount());
    if (listHead == nullptr) {
        listHead = head;
        listTail = tail;
    } else {
        tail->SetNextRegion(listHead);
        listHead->SetPrevRegion(tail);
        listHead = head;
    }
    for (ZPage* node = head; node != nullptr; node = node->GetNextRegion()) {
        node->SetRegionListOwner(this);
    }
}

void RegionList::PrependRegion(ZPage* region)
{
    std::lock_guard<std::mutex> lock(listMutex);
    PrependRegionLocked(region);
}

void RegionList::PrependRegionLocked(ZPage* region)
{
    if (region == nullptr) {
        return;
    }

    CHECK_DETAIL(region->GetRegionListOwner() == nullptr, "region already belongs to a list");

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx)", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd());

    region->SetRegionListOwner(this);
    region->SetPrevRegion(nullptr);
    IncCounts(1, region->GetUnitCount());
    region->SetNextRegion(listHead);
    if (listHead == nullptr) {
        MRT_ASSERT(listTail == nullptr, "PrependRegion listTail is not null");
        listTail = region;
    } else {
        listHead->SetPrevRegion(region);
    }
    listHead = region;
}

void RegionList::DeleteRegionLocked(ZPage* del)
{
    MRT_ASSERT(listHead != nullptr && listTail != nullptr, "illegal region list");
    CHECK_DETAIL(del != nullptr && del->GetRegionListOwner() == this, "region belongs to another list");

    ZPage* pre = del->GetPrevRegion();
    ZPage* next = del->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);
    del->SetRegionListOwner(nullptr);

    DLOG(REGION, "list %p (%zu, %zu)-(%zu, %zu) delete region %p@[%#zx+%zu, %#zx) type %u", this,
        regionCount, unitCount, 1llu, del->GetUnitCount(),
        del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(), 0u);

    DecCounts(1, del->GetUnitCount());

    if (listHead == del) { // delete head
        MRT_ASSERT(pre == nullptr, "Delete Region pre is not null");
        listHead = next;
        if (listHead == nullptr) { // now empty
            listTail = nullptr;
            return;
        }
    } else if (pre != nullptr) {
        pre->SetNextRegion(next);
    }

    if (listTail == del) { // delete tail
        MRT_ASSERT(next == nullptr, "Delete Region next is not null");
        listTail = pre;
        if (listTail == nullptr) { // now empty
            listHead = nullptr;
            return;
        }
    } else if (next != nullptr) {
        next->SetPrevRegion(pre);
    } else if (pre != nullptr) {
        // next was stolen (region re-homed onto another list) while this list
        // still named it. Treat del as the last node we still own.
        listTail = pre;
    }
}

}
