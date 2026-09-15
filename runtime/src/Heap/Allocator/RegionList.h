// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REGION_LIST_H
#define MRT_REGION_LIST_H

#include "Heap/z/zPage.hpp"

#include "Heap/z/zList.hpp"
namespace MapleRuntime {
class RegionCache : public RegionList {
public:
    RegionCache(const char* name) : RegionList(name) {}

    bool TryPrependRegion(RegionInfo *region, RegionInfo::RegionType type)
    {
        std::lock_guard<std::mutex> lock(listMutex);
        if (active) {
            PrependRegionLocked(region, type);
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
        for (RegionInfo *node = listHead; node != nullptr; node = node->GetNextRegion()) {
            node->SetTraceRegionFlag(0);
        }
        active = false;
    }
private:
    bool active = false;
};
} // namespace MapleRuntime
#endif // MRT_REGION_LIST_H
