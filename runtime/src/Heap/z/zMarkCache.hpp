// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZMARKCACHE_HPP
#define MRT_ZMARKCACHE_HPP
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;
class MarkLiveCache {
public:
    explicit MarkLiveCache(size_t stripeCount);
    ~MarkLiveCache();
    MarkLiveCache(const MarkLiveCache&) = delete;
    MarkLiveCache& operator=(const MarkLiveCache&) = delete;

    void IncLive(RegionInfo* region, size_t bytes);
    void Flush();

private:
    struct Entry {
        RegionInfo* region = nullptr;
        size_t bytes = 0;
        uint32_t objects = 0;
    };

    void Evict(Entry& entry);
    size_t shift;
    static constexpr size_t CACHE_SIZE = 64;
    Entry entries[CACHE_SIZE];
};
} // namespace MapleRuntime
#endif
