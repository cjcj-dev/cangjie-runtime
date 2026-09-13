// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zList.hpp"
namespace MapleRuntime {
void RegionList::MergeRegionList(RegionList& srcList, RegionInfo::RegionType regionType)
{
    RegionList regionList("region list cache");
    srcList.MoveTo(regionList);
    RegionInfo* head = regionList.GetHeadRegion();
    RegionInfo* tail = regionList.GetTailRegion();
    if (head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(listMutex);
    regionList.SetElementType(regionType);
    IncCounts(regionList.GetRegionCount(), regionList.GetUnitCount());
    if (listHead == nullptr) {
        listHead = head;
        listTail = tail;
    } else {
        tail->SetNextRegion(listHead);
        listHead->SetPrevRegion(tail);
        listHead = head;
    }
    for (RegionInfo* node = head; node != nullptr; node = node->GetNextRegion()) {
        node->SetRegionListOwner(this);
    }
}

void RegionList::PrependRegion(RegionInfo* region, RegionInfo::RegionType type)
{
    std::lock_guard<std::mutex> lock(listMutex);
    PrependRegionLocked(region, type);
}

void RegionList::PrependRegionLocked(RegionInfo* region, RegionInfo::RegionType type)
{
    if (region == nullptr) {
        return;
    }

    CHECK_DETAIL(region->GetRegionListOwner() == nullptr, "region already belongs to a list");

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx) type %u->%u", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType(), type);

    region->SetRegionType(type);
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

void RegionList::DeleteRegionLocked(RegionInfo* del)
{
    MRT_ASSERT(listHead != nullptr && listTail != nullptr, "illegal region list");
    CHECK_DETAIL(del != nullptr && del->GetRegionListOwner() == this, "region belongs to another list");

    RegionInfo* pre = del->GetPrevRegion();
    RegionInfo* next = del->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);
    del->SetRegionListOwner(nullptr);

    DLOG(REGION, "list %p (%zu, %zu)-(%zu, %zu) delete region %p@[%#zx+%zu, %#zx) type %u", this,
        regionCount, unitCount, 1llu, del->GetUnitCount(),
        del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(), del->GetRegionType());

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
