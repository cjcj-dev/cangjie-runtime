// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <vector>
namespace MapleRuntime {
class MappedCache {
public:
    using Index = uint32_t;
    using Count = uint32_t;
    struct Extent {
        Index index{ 0 };
        Count count{ 0 };
        bool IsNull() const { return count == 0; }
    };

    void SetRefresh(const std::function<void(Extent)>& callback) { refresh = callback; }
    void Insert(Extent extent);
    Extent RemoveContiguous(Count count);
    Count RemoveDiscontiguous(Count count, std::vector<Extent>& out);
    Count RemoveForUncommit(Count count, std::vector<Extent>& out);
    Count Size() const { return size; }
    Count MinSizeWatermark() const { return minSizeWatermark; }
    void ResetMinSizeWatermark() { minSizeWatermark = size; }
    size_t EntryCount() const { return entries.size(); }
    Count MaxExtent() const;
    uint64_t LastUsedNs() const { return lastUsedNs; }

private:
    static constexpr int NUM_SIZE_CLASSES = 31;
    struct Entry {
        Count count;
        std::list<Index>::iterator sizeClassPosition;
    };
    std::map<Index, Entry> entries;
    std::array<std::list<Index>, NUM_SIZE_CLASSES> sizeClasses;
    Count size{ 0 };
    Count minSizeWatermark{ 0 };
    uint64_t lastUsedNs{ 0 };
    std::function<void(Extent)> refresh;
    static int SizeClass(Count count);
    void AddEntry(Index index, Count count);
    void EraseEntry(std::map<Index, Entry>::iterator entry);
    Extent Remove(Index index, Count count, bool high = false);
    Index Select(Count minimum, Count maximum) const;
};

}
