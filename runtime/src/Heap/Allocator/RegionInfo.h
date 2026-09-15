// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zPage.hpp"

namespace MapleRuntime {
inline unsigned RegionInfo::RelocateObserve() const
    {
        auto owner = ForwardingTable::RetainPageOwner(const_cast<RegionInfo*>(this));
        if (!owner) {
            return 0;
        }
        unsigned v = 1;
        if (owner->is_claimed()) {
            v |= 2;
        }
        if (owner->is_done()) {
            v |= 4;
        }
        if (owner->in_place()) {
            v |= 8;
        }
        return v;
    }

inline std::atomic<uint64_t>& RegionInfo::EnrolBeforeFlip()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

inline std::atomic<uint64_t>& RegionInfo::EnrolAfterFlip()
    {
        static std::atomic<uint64_t> n{ 0 };
        return n;
    }

inline LiveInfo* RegionInfo::GetLiveInfo0ForProbe() const
    {
        const ZForwarding::FromPageView* from = GetFromPageView();
        return from == nullptr ? nullptr : from->liveInfo;
    }

NO_RETURN inline ATTR_COLD ATTR_NO_INLINE void RegionInfo::ReportInvalidObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const
    {
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        size_t bitCapacity = (regionEnd - regionStart) / kMarkedBytesPerBit;
        size_t bitIndex = objAddr >= regionStart ? (objAddr - regionStart) / kMarkedBytesPerBit :
                                                   std::numeric_limits<size_t>::max();
        GCPhase phase = Heap::GetHeap().GetGCPhase(GCCycleGeneration::OLD);
        LOG(RTLOG_FATAL,
            "[GCV2][sizeguard][INVALID_OBJECT_SIZE] obj=%p objSize=%zu region=%p regionStart=%#zx "
            "regionEnd=%#zx allocPtr=%#zx regionType=%u unitRole=%u young=%u phase=%u bitCap=%zu bitIdx=%zu align=%zu",
            obj, objSize, this, regionStart, regionEnd, GetRegionAllocPtr(), static_cast<unsigned>(GetRegionType()),
            static_cast<unsigned>(GetUnitRole()), static_cast<unsigned>(IsYoungRegion()),
            static_cast<unsigned>(phase), bitCapacity, bitIndex, kMarkedBytesPerBit);
        std::abort();
    }
}
