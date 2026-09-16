// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Old-region residue (no ZGC counterpart). RegionList is the page-list form of
// the unit-array descriptor allocator (ZPage/RegionManager); ZGC keeps
// ZList<ZPage> (zList.hpp) inside ZPageAllocator/ZPageCache. It is deleted with
// the old region mechanism (P03 independent ZPage descriptor, P05 page
// allocator); the ZGC-shaped generic list lives in Heap/z/zList.hpp.

#ifndef MRT_REGION_LIST_H
#define MRT_REGION_LIST_H

#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
class RegionList {
public:
    friend void RemoveRegionLocked(RegionList*, ZPage*);
    RegionList(const char* name) : listName(name) {}

    void PrependRegion(ZPage* region);
    void PrependRegionLocked(ZPage* region);

    void MergeRegionList(RegionList& regionList);

    const char* GetListName() const { return listName; }

    void DeleteRegion(ZPage* del)
    {
        if (del == nullptr) {
            return;
        }

        std::lock_guard<std::mutex> lock(listMutex);
        DeleteRegionLocked(del);
    }

    bool TryDeleteRegion(ZPage* del)
    {
        if (del == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(listMutex);
        if (del->GetRegionListOwner() != this) {
            return false;
        }
        DeleteRegionLocked(del);
        return true;
    }

#ifdef MRT_DEBUG
    void DumpRegionList(const char*);
#endif

    void DecCounts(size_t nRegion, size_t nUnit)
    {
        if (regionCount >= nRegion && unitCount >= nUnit) {
            regionCount -= nRegion;
            unitCount -= nUnit;
        } else {
            LOG(RTLOG_FATAL, "region list %p error count %zu-%zu %zu-%zu", regionCount, nRegion, unitCount, nUnit);
        }
    }

    void IncCounts(size_t nRegion, size_t nUnit)
    {
        CHECK((nRegion <= std::numeric_limits<size_t>::max() - regionCount) &&
              (nUnit <= std::numeric_limits<size_t>::max() - unitCount));
        regionCount += nRegion;
        unitCount += nUnit;
    }

    ZPage* GetHeadRegion() const { return listHead; }

    void ClearList()
    {
        listHead = nullptr;
        listTail = nullptr;
        regionCount = 0;
        unitCount = 0;
    }

    ZPage* GetTailRegion() { return listTail; }

    ZPage* TakeHeadRegion()
    {
        std::lock_guard<std::mutex> lg(listMutex);
        if (listHead == nullptr) { return nullptr; }
        ZPage* currentHead = listHead;
        DeleteRegionLocked(currentHead);
        return currentHead;
    }

    size_t GetUnitCount() const { return unitCount; }

    size_t GetRegionCount() const { return regionCount; }

    size_t GetAllocatedSize(bool count = false) const
    {
        if (!count) {
            return GetUnitCount() * ZPage::UNIT_SIZE;
        }
        return CountAllocatedSize();
    }

    void VisitAllRegions(const std::function<void(ZPage*)>& visitor) const
    {
        std::lock_guard<std::mutex> lock(listMutex);
        ZPage* node = listHead;
        ZPage* next = node;
        while (node != nullptr) {
            next = node->GetNextRegion();
            visitor(node);
            node = next;
        }
    }

    void VisitAllGhostRegions(const std::function<void(ZPage*)>& visitor)
    {
        // Snapshot next before the visitor. PrepareFromRegionList may
        // ReclaimRegionToMarkQuarantine → InitZPage, which clears
        // nextRegionIdx0 (the ghost successor). Walking GetNextGhostRegion
        // after that truncates the chain; undispelled from-regions then
        // fail CHECK(inGhostFromRegion==0).
        // Same shape as VisitAllRegions (RegionList.h:115-124).
        ZPage* node = listHead;
        while (node != nullptr) {
            ZPage* next = node->GetNextGhostRegion();
            visitor(node);
            node = next;
        }
    }

    void SetElementType()
    {
    }

    void ClearTraceRegionFlag()
    {
        std::lock_guard<std::mutex> lock(listMutex);
        for (ZPage *node = listHead; node != nullptr; node = node->GetNextRegion()) {
            (void)node;
        }
    }

    std::mutex& GetListMutex() { return listMutex; }

    void MoveTo(RegionList& targetList)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        targetList.AssignWith(*this);
        for (ZPage* node = targetList.listHead; node != nullptr; node = node->GetNextRegion()) {
            node->SetRegionListOwner(&targetList);
        }
        this->ClearList();
    }

    void CopyListTo(RegionList& dstList)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        // Snapshot aliases (notably ghostFromRegionList) never claim authority;
        // the source list remains the sole owner of every node.
        dstList.listHead = this->listHead;
        dstList.listTail = this->listTail;
        dstList.regionCount = this->regionCount;
        dstList.unitCount = this->unitCount;
    }

protected:
    mutable std::mutex listMutex;
    size_t regionCount = 0;
    size_t unitCount = 0;
    ZPage* listHead = nullptr; // the start region for iteration, i.e., the first region
    ZPage* listTail = nullptr; // help to merge region list
    const char* listName = nullptr;
private:
    void DeleteRegionLocked(ZPage* del);

    void AssignWith(const RegionList& srcList)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        listHead = srcList.listHead;
        listTail = srcList.listTail;
        regionCount = srcList.regionCount;
        unitCount = srcList.unitCount;
    }

    // allocated-size of to-region list must be calculated on the fly.
    size_t CountAllocatedSize() const
    {
        size_t allocCnt = 0;
        std::lock_guard<std::mutex> lock(const_cast<RegionList*>(this)->listMutex);
        for (ZPage* region = listHead; region != nullptr; region = region->GetNextRegion()) {
            allocCnt += region->GetRegionAllocatedSize();
        }
        return allocCnt;
    }

#ifdef MRT_DEBUG
    void VerifyRegion(ZPage* region)
    {
        ZPage* prev = region->GetPrevRegion();
        ZPage* next = region->GetNextRegion();
        if (prev != nullptr && prev->GetNextRegion() != region) {
            LOG(RTLOG_FATAL, "illegal region node");
        }

        if (next != nullptr && next->GetPrevRegion() != region) {
            LOG(RTLOG_FATAL, "illegal region node");
        }
    }
#endif
};

class RegionCache : public RegionList {
public:
    RegionCache(const char* name) : RegionList(name) {}

    bool TryPrependRegion(ZPage *region)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        if (active) {
            PrependRegionLocked(region);
            return true;
        }
        return false;
    }

    void ActivateRegionCache()
    {
        std::lock_guard<std::mutex> lock(listMutex);
        active = true;
    }

    void DeactivateRegionCache()
    {
        std::lock_guard<std::mutex> lock(listMutex);
        for (ZPage *node = listHead; node != nullptr; node = node->GetNextRegion()) {
            (void)node;
        }
        active = false;
    }
private:
    bool active = false;
};
} // namespace MapleRuntime
#endif // MRT_REGION_LIST_H
