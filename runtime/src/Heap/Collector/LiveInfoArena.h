// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_LIVE_INFO_ARENA_H
#define MRT_LIVE_INFO_ARENA_H

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Heap/z/zLiveMap.hpp"

namespace MapleRuntime {

class LiveInfoArena {
public:
    LiveInfoArena() = default;
    ~LiveInfoArena()
    {
        for (auto& page : liveInfosByPage) {
            for (LiveInfo* live : page.second) {
                DeleteLiveInfo(live);
            }
        }
    }

    static LiveInfoArena& GetLiveInfoArena();

    // A RegionInfo descriptor is reused in place. Keep both its current and
    // promoted-from metadata until the descriptor's page retirement completes.
    LiveInfo* AllocateLiveInfo(RegionInfo* page)
    {
        LiveInfo* live = new LiveInfo();
        std::lock_guard<std::mutex> guard(recycleMutex);
        liveInfosByPage[page].push_back(live);
        return live;
    }

    using OwnedLiveInfo = std::unique_ptr<LiveInfo, void (*)(LiveInfo*)>;

    // clone_for_promotion leaves the original young page in the relocation
    // set. Transfer its ordinary livemap out of the reusable region slot.
    OwnedLiveInfo TakePageLiveInfo(RegionInfo* page, LiveInfo* live)
    {
        if (live != nullptr) {
            std::lock_guard<std::mutex> guard(recycleMutex);
            auto pageIt = liveInfosByPage.find(page);
            CHECK(pageIt != liveInfosByPage.end());
            auto& owned = pageIt->second;
            auto it = std::find(owned.begin(), owned.end(), live);
            CHECK(it != owned.end());
            owned.erase(it);
            if (owned.empty()) {
                liveInfosByPage.erase(pageIt);
            }
        }
        return OwnedLiveInfo(live, [](LiveInfo* original) {
            GetLiveInfoArena().DeleteLiveInfo(original);
        });
    }

    // ZPageAllocator::safe_destroy_page / CHeapBitMap::~CHeapBitMap.
    // The caller has removed the page and drained its forwarding readers.
    void RecyclePageLiveInfo(RegionInfo* page)
    {
        std::vector<LiveInfo*> owned;
        {
            std::lock_guard<std::mutex> guard(recycleMutex);
            auto it = liveInfosByPage.find(page);
            if (it == liveInfosByPage.end()) {
                return;
            }
            owned.swap(it->second);
            liveInfosByPage.erase(it);
        }
        for (LiveInfo* live : owned) {
            DeleteLiveInfo(live);
        }
    }

    RegionBitmap* AllocateRegionBitmap(size_t regionSize)
    {
        const size_t bytes = RegionBitmap::GetRegionBitmapSize(regionSize);
        void* addr = std::malloc(bytes);
        CHECK(addr != nullptr);
        RegionBitmap* bitmap = new (addr) RegionBitmap(regionSize);
        return bitmap;
    }

    void RecycleRegionBitmap(RegionBitmap* bitmap)
    {
        if (bitmap == nullptr) {
            return;
        }
        std::free(bitmap);
    }

    void RetireUntilOwnerExit(LiveInfo* owner, RegionBitmap* bitmap)
    {
        if (owner == nullptr || bitmap == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> guard(recycleMutex);
        retiredByOwner[owner].push_back(bitmap);
    }

    void RecycleOwnerBitmaps(LiveInfo* owner)
    {
        if (owner == nullptr) {
            return;
        }
        std::vector<RegionBitmap*> retired;
        {
            std::lock_guard<std::mutex> guard(recycleMutex);
            auto it = retiredByOwner.find(owner);
            if (it == retiredByOwner.end()) {
                return;
            }
            retired.swap(it->second);
            retiredByOwner.erase(it);
        }
        for (RegionBitmap* bitmap : retired) {
            RecycleRegionBitmap(bitmap);
        }
    }

    RegionBitmap* PublishMatchingBitmap(RegionBitmap** slot, RegionBitmap* current, size_t regionSize,
                                        LiveInfo* owner)
    {
        if (current->CoversRegionSize(regionSize)) {
            return current;
        }
        RegionBitmap* replacement = AllocateRegionBitmap(regionSize);
        RegionBitmap* expected = current;
        if (__atomic_compare_exchange_n(slot, &expected, replacement, false, std::memory_order_seq_cst,
                                        std::memory_order_relaxed)) {
            RetireUntilOwnerExit(owner, current);
            return replacement;
        }
        RecycleRegionBitmap(replacement);
        return expected;
    }

private:
    void DeleteLiveInfo(LiveInfo* live)
    {
        RecycleOwnerBitmaps(live);
        RecycleRegionBitmap(live->GetMarkFace().bitmap);
        RecycleRegionBitmap(live->resurrectBitmap);
        RecycleRegionBitmap(live->enqueueBitmap);
        delete live;
    }

    std::mutex recycleMutex;
    std::unordered_map<LiveInfo*, std::vector<RegionBitmap*>> retiredByOwner;
    std::unordered_map<RegionInfo*, std::vector<LiveInfo*>> liveInfosByPage;
};
} // namespace MapleRuntime
#endif // MRT_LIVE_INFO_ARENA_H
