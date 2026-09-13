// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATE_H
#define MRT_RELOCATE_H

#include "Heap/Allocator/PageAllocator.h"

namespace MapleRuntime {
namespace detail {

// A single algorithm body serves both compile-time shapes below.  The default
// product inlines it through ForwardTask::Execute; the testable shape calls it
// from the exported out-of-line Execute instantiated in RegionManager.cpp.
template<Generation G>
inline void ExecuteForwardTask(RegionManager& regionManager, RegionList& fromRegionList)
{
    while (true) {
        // zRelocate.cpp:1193-1203: serve a mutator's requested receipt
        // before advancing the ordinary relocation iterator.
        RelocationRequestQueue::Selection selected =
            regionManager.GetRelocationRequestQueue().SelectBeforeOrdinary([&fromRegionList]() -> void* {
                return fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            });
        if (!selected) {
            selected = regionManager.GetRelocationRequestQueue().SynchronizePoll();
            if (selected.workersDone) {
                break;
            }
            if (!selected) {
                continue;
            }
        }
        if (!selected.is_request()) {
            RegionInfo* region = static_cast<RegionInfo*>(selected.ordinary);
            regionManager.ForwardClaimedPage<G>(region, ForwardingTable::RetainPageOwner(region));
            continue;
        }

        RegionInfo* region = static_cast<RegionInfo*>(selected.request->owner());
#if defined(MRT_GCV2_REGION_WAIT_DIAG)
        static std::atomic<size_t> g_regionWaitClaim{ 0 };
        const size_t claimN = g_regionWaitClaim.fetch_add(1, std::memory_order_relaxed) + 1;
        if (claimN <= 8 || (claimN & (claimN - 1)) == 0) {
            LOG(RTLOG_ERROR,
                "[GCV2][region-wait-claim] n=%zu from=%p claim=1 pending=%zu",
                claimN, reinterpret_cast<void*>(selected.request->from()),
                regionManager.GetRelocationRequestQueue().PendingCount());
        }
#endif
        // If an ordinary iterator already removed the page, its worker will
        // lose the forwarding claim. This claimant still owns the page task.
        (void)fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
                                             RegionInfo::RegionType::LONE_FROM_REGION);
        regionManager.ForwardClaimedPage<G>(region,
            ForwardingTable::RetainPageOwner(region), true);
    }
}

} // namespace detail

// The relocation worker task submitted by DrainForwardFromRegions. Test builds
// export Work so the unit runner binds the product SO; default builds retain
// the implicit inline virtual with no MRT_EXPORT and no dynamic export.
template<Generation G>
class ForwardTask : public GCWorkerTask {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() override = default;
#if defined(MRT_TESTABLE_INTERNALS)
    MRT_EXPORT void Work(uint32_t) override;
#else
    __attribute__((visibility("hidden"))) void Work(uint32_t) override
    {
        detail::ExecuteForwardTask<G>(regionManager, fromRegionList);
    }
#endif

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};
inline bool RegionManager::RouteIsPublished(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        if (fromObj != nullptr && fromObj->IsForwarded()) {
            return true;
        }
        if (fromRegionInfo == nullptr) {
            return false;
        }
        return fromRegionInfo->IsForwardingDone();
    }

inline PublishedRoute RegionManager::FindPublishedRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        BaseObject* to = ComputeRoute(fromObj, fromRegionInfo);
        if (to == nullptr || !RouteIsPublished(fromObj, fromRegionInfo)) {
            return PublishedRoute{ nullptr };
        }
        return PublishedRoute{ to };
    }

inline PublishedRoute RegionManager::FindPublishedRoute(BaseObject* fromObj)
    {
        RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(fromObj));
        if (fromRegionInfo == nullptr) {
            return PublishedRoute{ nullptr };
        }
        BaseObject* to = ComputeRoute(fromObj, fromRegionInfo);
        if (to == nullptr || !RouteIsPublished(fromObj, fromRegionInfo)) {
            return PublishedRoute{ nullptr };
        }
        return PublishedRoute{ to };
    }

inline bool RegionManager::RouteRegion(RegionInfo* fromRegionInfo, bool mayWait)
    {
        // fysfixb / 352ed4e8: non-ghost is a defined negative answer, not invariant break.
        // Producers that clear ghost: DispelGhostFromRegion (PrepareFromRegionList),
        // ClearGhostRegionBit (raw-pin POST_TRACE), TakeRegion reuse. Consumers
        // (ForwardRegion / TryForwardObject) may still hold a region* after the
        // carrier retired or after liveBytes==0 skipped install (pre-a2e7ee37).
        // Soft-null matches RouteObject's GetGhostFromRegionAt==null path.
        if (UNLIKELY(!fromRegionInfo->IsGhostFromRegion())) {
            VLOG(REPORT,
                 "[GCV2][ghost-softnull] region=%p start=%#zx live=%zu route=%u young=%u "
                 "auth=%u — RouteRegion soft-miss (ghost cleared or never installed)",
                 fromRegionInfo, fromRegionInfo->GetRegionStart(), fromRegionInfo->GetLiveByteCount(),
                 static_cast<unsigned>(fromRegionInfo->RelocateObserve()),
                 static_cast<unsigned>(fromRegionInfo->IsYoungRegion()),
                 static_cast<unsigned>(fromRegionInfo->IsLiveCountAuthoritative()));
            return false;
        }
        // zRelocate.cpp:1155-1158 claimant runs page work; consumers wait (zRelocate.cpp:403-409).
        auto owner = ForwardingTable::RetainPageOwner(fromRegionInfo);
        if (owner && owner->is_done()) {
            return !owner->in_place();
        }
        if (owner && ZForwardingLife::CurrentPageWork() == owner.get()) {
            if (RelocateClaimedPage(fromRegionInfo)) {
                return true;
            }
            owner->set_in_place();
            return false;
        }
        if (!mayWait) {
            return false;
        }
        while (true) {
            owner = ForwardingTable::RetainPageOwner(fromRegionInfo);
            if (owner && owner->is_done()) {
                return !owner->in_place();
            }
            if (owner && ZForwardingLife::CurrentPageWork() == owner.get()) {
                if (RelocateClaimedPage(fromRegionInfo)) {
                    return true;
                }
                owner->set_in_place();
                return false;
            }
            sched_yield();
        }
    }

inline BaseObject* RegionManager::ComputeRoute(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        RegionInfo::RetainScope retain(fromRegionInfo);
        if (!retain.ok()) {
            return nullptr;
        }

        return ComputeRouteBorrowed(fromObj, fromRegionInfo);
    }

inline BaseObject* RegionManager::ComputeRouteBorrowed(BaseObject* fromObj, RegionInfo* fromRegionInfo)
    {
        if (RouteRegion(fromRegionInfo, false) || fromRegionInfo->IsCompacted()) {
            OptionalRouteTicket ticket = fromRegionInfo->AdmitForRoute(fromObj);
            if (!ticket) {
                return nullptr;
            }
            BaseObject* to = fromRegionInfo->GetRoute(ticket.value());
            return to;
        }
        return nullptr;
    }

} // namespace MapleRuntime
#endif
