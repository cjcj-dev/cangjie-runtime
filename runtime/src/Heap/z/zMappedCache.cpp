// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMappedCache.hpp"
#include "Heap/z/zPage.hpp"
namespace MapleRuntime {
int MappedCache::SizeClass(Count count)
{
    if (count < 2) { return -1; }
    int shift = 0;
    while ((count >>= 1) > 1) { ++shift; }
    return shift;
}

void MappedCache::AddEntry(Index index, Count count)
{
    const int klass = SizeClass(count);
    Entry entry{ count, {} };
    if (klass >= 0) {
        sizeClasses[klass].push_front(index);
        entry.sizeClassPosition = sizeClasses[klass].begin();
    }
    CHECK(entries.emplace(index, entry).second);
    if (refresh) { refresh({index, count}); }
}

void MappedCache::EraseEntry(std::map<Index, Entry>::iterator entry)
{
    const int klass = SizeClass(entry->second.count);
    if (klass >= 0) { sizeClasses[klass].erase(entry->second.sizeClassPosition); }
    entries.erase(entry);
}

// zMappedCache.cpp:560: coalesce both neighbours in the same authoritative cache.
void MappedCache::Insert(Extent extent)
{
    CHECK(extent.count != 0);
    Index start = extent.index;
    Count count = extent.count;
    auto right = entries.lower_bound(start);
    CHECK(right == entries.end() || static_cast<uint64_t>(start) + count <= right->first);
    if (right != entries.begin()) {
        auto left = std::prev(right);
        CHECK(static_cast<uint64_t>(left->first) + left->second.count <= start);
        if (left->first + left->second.count == start) {
            start = left->first;
            count += left->second.count;
            EraseEntry(left);
        }
    }
    if (right != entries.end() && start + count == right->first) {
        count += right->second.count;
        EraseEntry(right);
    }
    AddEntry(start, count);
    size += extent.count;
    lastUsedNs = TimeUtil::NanoSeconds();
}

MappedCache::Extent MappedCache::Remove(Index index, Count count, bool high)
{
    auto entry = entries.find(index);
    CHECK(entry != entries.end() && count <= entry->second.count && count != 0);
    const Count remaining = entry->second.count - count;
    EraseEntry(entry);
    Extent result{ high ? index + remaining : index, count };
    if (remaining != 0) { AddEntry(high ? index : index + count, remaining); }
    size -= count;
    minSizeWatermark = std::min(size, minSizeWatermark);
    return result;
}

MappedCache::Index MappedCache::Select(Count minimum, Count maximum) const
{
    // zMappedCache.cpp:408: guaranteed size class, then descending approximate
    // best fit; only the sub-size-class tail needs an address scan.
    int guaranteed = SizeClass(maximum);
    if (maximum <= 2) { guaranteed = 0; }
    else if ((maximum & (maximum - 1)) != 0) { ++guaranteed; }
    for (int klass = guaranteed; klass < NUM_SIZE_CLASSES; ++klass) {
        if (!sizeClasses[klass].empty()) { return sizeClasses[klass].front(); }
    }
    for (int klass = SizeClass(maximum); klass >= std::max(SizeClass(minimum), 0); --klass) {
        for (Index index : sizeClasses[klass]) {
            if (entries.at(index).count >= minimum) { return index; }
        }
    }
    if (SizeClass(minimum) < 0) {
        for (const auto& entry : entries) {
            if (entry.second.count >= minimum) { return entry.first; }
        }
    }
    return UINT32_MAX;
}

MappedCache::Extent MappedCache::RemoveContiguous(Count count)
{
    CHECK(count != 0);
    if (size < count) { return {}; }
    // zMappedCache.cpp:642: small pages are selected from the lowest address.
    const Index index = count == 1 ? entries.begin()->first : Select(count, count);
    return index == UINT32_MAX ? Extent{} : Remove(index, count);
}

MappedCache::Count MappedCache::RemoveDiscontiguous(Count count, std::vector<Extent>& out)
{
    Count remaining = count;
    while (remaining != 0 && size != 0) {
        const Index index = Select(1, std::min(remaining, size));
        const Extent extent = Remove(index, std::min(remaining, entries.at(index).count));
        out.push_back(extent);
        remaining -= extent.count;
    }
    return count - remaining;
}

MappedCache::Count MappedCache::RemoveForUncommit(Count count, std::vector<Extent>& out)
{
    Count remaining = count;
    while (remaining != 0 && !entries.empty()) {
        auto last = std::prev(entries.end());
        const Extent extent = Remove(last->first, std::min(remaining, last->second.count), true);
        out.push_back(extent);
        remaining -= extent.count;
    }
    return count - remaining;
}

MappedCache::Count MappedCache::MaxExtent() const
{
    Count maximum = 0;
    for (const auto& entry : entries) { maximum = std::max(maximum, entry.second.count); }
    return maximum;
}

}
