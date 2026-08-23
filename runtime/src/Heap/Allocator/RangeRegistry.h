// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ALLOC_RANGE_REGISTRY_H
#define MRT_ALLOC_RANGE_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace MapleRuntime {

// A half-open address interval. A zero-sized range is the null result returned
// when a claim cannot be satisfied.
class Range {
public:
    Range() = default;
    Range(uintptr_t start, size_t size);

    bool IsNull() const;
    bool IsValid() const;
    uintptr_t Start() const;
    uintptr_t End() const;
    size_t Size() const;

    bool Contains(const Range& other) const;
    bool AdjacentTo(const Range& other) const;

    bool GrowFromFront(size_t size);
    bool GrowFromBack(size_t size);
    Range ShrinkFromFront(size_t size);
    Range ShrinkFromBack(size_t size);

    Range Partition(size_t offset, size_t partitionSize) const;
    Range FirstPart(size_t splitOffset) const;
    Range LastPart(size_t splitOffset) const;

    bool operator==(const Range& other) const;
    bool operator!=(const Range& other) const;

private:
    uintptr_t start{ 0 };
    size_t size{ 0 };
};

// Sorted ownership ledger for address intervals. Successful insertion keeps
// the ledger disjoint and coalesces every adjacent pair. Claims are first-fit
// from the selected address end, matching ZRangeRegistry's low/high variants.
class RangeRegistry {
public:
    using PrepareCallback = void (*)(const Range& range, void* context);
    using ResizeCallback = void (*)(const Range& from, const Range& to, void* context);

    struct Callbacks {
        PrepareCallback prepareForHandOut{ nullptr };
        PrepareCallback prepareForHandBack{ nullptr };
        ResizeCallback grow{ nullptr };
        ResizeCallback shrink{ nullptr };
        void* context{ nullptr };
    };

    RangeRegistry() = default;
    ~RangeRegistry() = default;

    RangeRegistry(const RangeRegistry&) = delete;
    RangeRegistry(RangeRegistry&&) = delete;
    RangeRegistry& operator=(const RangeRegistry&) = delete;
    RangeRegistry& operator=(RangeRegistry&&) = delete;

    // Callbacks run while the registry lock is held and must not re-enter this
    // registry. RegisterRange is for initial ownership and does not emit the
    // hand-back callback; Insert represents ownership being handed back.
    void RegisterCallbacks(const Callbacks& callbacks);
    bool RegisterRange(const Range& range);
    bool Insert(const Range& range);

    Range ClaimLow(size_t size);
    Range ClaimHigh(size_t size);

    bool IsEmpty() const;
    bool IsContiguous() const;
    bool Contains(const Range& range) const;
    size_t TotalSize() const;
    std::vector<Range> Snapshot() const;

private:
    bool InsertLocked(const Range& range, bool prepareForHandBack);
    void GrowFromFrontLocked(Range& range, size_t size);
    void GrowFromBackLocked(Range& range, size_t size);
    Range ShrinkFromFrontLocked(Range& range, size_t size);
    Range ShrinkFromBackLocked(Range& range, size_t size);
    void PrepareForHandOutLocked(const Range& range) const;

    mutable std::mutex lock;
    std::vector<Range> ranges;
    Callbacks callbacks;
};

} // namespace MapleRuntime

#endif // MRT_ALLOC_RANGE_REGISTRY_H
