// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkCache.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zUtils.inline.hpp"
namespace MapleRuntime {
MarkLiveCache::MarkLiveCache(size_t stripeCount) : shift(MARK_STRIPE_SHIFT + Log2Exact(stripeCount)) {}

MarkLiveCache::~MarkLiveCache()
{
    Flush();
}

void MarkLiveCache::IncLive(RegionInfo* region, size_t bytes)
{
    CHECK_DETAIL(region != nullptr, "cannot cache live bytes for a null region");
    const size_t index = (region->GetRegionStart() >> shift) & (CACHE_SIZE - 1);
    Entry& entry = entries[index];
    if (entry.region != region) {
        Evict(entry);
        entry.region = region;
    }
    entry.bytes += bytes;
    ++entry.objects;
}

void MarkLiveCache::Evict(Entry& entry)
{
    if (entry.region != nullptr) {
        entry.region->AddLiveCounts(entry.objects, entry.bytes);
        entry.region = nullptr;
        entry.bytes = 0;
        entry.objects = 0;
    }
}

void MarkLiveCache::Flush()
{
    for (Entry& entry : entries) {
        Evict(entry);
    }
}


} // namespace MapleRuntime
