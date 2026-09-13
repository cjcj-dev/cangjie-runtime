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

inline bool RegionInfo::PageOwnerVerifyCountOnly()
    {
        static const bool countOnly = []() {
            const char* value = std::getenv("MRT_GCV2_VERIFY_PAGE_OWNER");
            return value != nullptr && std::strcmp(value, "count") == 0;
        }();
        return countOnly;
    }

inline std::atomic<size_t>& RegionInfo::PageOwnerMismatchAttempts()
    {
        static std::atomic<size_t> count{0};
        return count;
    }

inline std::atomic<size_t>& RegionInfo::PageOwnerMismatchFirstPaints()
    {
        static std::atomic<size_t> count{0};
        return count;
    }

inline void RegionInfo::ReportPageOwnerVerifyCounts()
    {
        std::fprintf(stderr, "[GCV2][page-owner] point=atexit mismatch_attempts=%zu first_paints=%zu mode=%s\n",
                     PageOwnerMismatchAttempts().load(std::memory_order_relaxed),
                     PageOwnerMismatchFirstPaints().load(std::memory_order_relaxed),
                     PageOwnerVerifyCountOnly() ? "count" : "assert");
        std::fflush(stderr);
    }

inline void RegionInfo::EnsurePageOwnerVerifyAtexit()
    {
        static const bool installed = []() {
            std::atexit([]() { ReportPageOwnerVerifyCounts(); });
            return true;
        }();
        (void)installed;
    }

template<Generation G>
inline void RegionInfo::NotePageOwnerFirstPaint() const
    {
        if (UNLIKELY(!MarkFaceMatchesOwner<G>())) {
            PageOwnerMismatchFirstPaints().fetch_add(1, std::memory_order_relaxed);
        }
    }

inline void RegionInfo::ReportMarkEpochCounts(const char* point)
    {
        const size_t stale = markEpochStaleReadCount.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[GCV2][mark-epoch] point=%s stale_read=%zu\n",
                     point != nullptr ? point : "?", stale);
        std::fflush(stderr);
    }

inline void RegionInfo::EnsureMarkEpochAtexit()
    {
        bool expected = false;
        if (markEpochAtexitInstalled.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::atexit([]() { ReportMarkEpochCounts("atexit"); });
        }
    }

inline BaseObject* RegionInfo::GetRouteForProbe(BaseObject* fromObj)
    {
        OptionalRouteTicket ticket = AdmitForRoute(fromObj);
        if (!ticket) {
            return nullptr;
        }
        return GetRoute(ticket.value());
    }

inline ATTR_COLD ATTR_NO_INLINE void RegionInfo::ReportTypeInfoInHeap(const BaseObject* obj, TypeInfo* tip, size_t objSize,
                                                       MAddress regionStart, MAddress regionEnd) const
    {
        size_t n = tipInHeapHits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            GCPhase phase = Heap::GetHeap().GetGCPhase();
            LOG(RTLOG_ERROR,
                "[GCV2][tipguard][TYPEINFO_IN_HEAP] obj=%p tip=%p objSize=%zu region=%p regionStart=%#zx "
                "regionEnd=%#zx allocPtr=%#zx regionType=%u young=%u phase=%u "
                "(default=count; fatal=MRT_GCV2_TIPINHEAP_FATAL=1)",
                obj, tip, objSize, this, regionStart, regionEnd, GetRegionAllocPtr(),
                static_cast<unsigned>(GetRegionType()), static_cast<unsigned>(IsYoungRegion()),
                static_cast<unsigned>(phase));
        } else if ((n & 0x3ffU) == 0) {
            LOG(RTLOG_ERROR, "[GCV2][tipguard][TYPEINFO_IN_HEAP_COUNT] total=%zu", n);
        }
    }

NO_RETURN inline ATTR_COLD ATTR_NO_INLINE void RegionInfo::ReportInvalidObjectSize(
        const BaseObject* obj, size_t objSize, MAddress regionStart, MAddress regionEnd) const
    {
        MAddress objAddr = reinterpret_cast<MAddress>(obj);
        size_t bitCapacity = (regionEnd - regionStart) / kMarkedBytesPerBit;
        size_t bitIndex = objAddr >= regionStart ? (objAddr - regionStart) / kMarkedBytesPerBit :
                                                   std::numeric_limits<size_t>::max();
        GCPhase phase = Heap::GetHeap().GetGCPhase();
        LOG(RTLOG_FATAL,
            "[GCV2][sizeguard][INVALID_OBJECT_SIZE] obj=%p objSize=%zu region=%p regionStart=%#zx "
            "regionEnd=%#zx allocPtr=%#zx regionType=%u unitRole=%u young=%u phase=%u bitCap=%zu bitIdx=%zu align=%zu",
            obj, objSize, this, regionStart, regionEnd, GetRegionAllocPtr(), static_cast<unsigned>(GetRegionType()),
            static_cast<unsigned>(GetUnitRole()), static_cast<unsigned>(IsYoungRegion()),
            static_cast<unsigned>(phase), bitCapacity, bitIndex, kMarkedBytesPerBit);
        std::abort();
    }
}
