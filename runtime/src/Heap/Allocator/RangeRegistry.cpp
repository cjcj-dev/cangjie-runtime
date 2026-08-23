// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Allocator/RangeRegistry.h"

#include <algorithm>
#include <limits>

namespace MapleRuntime {

namespace {
bool AddOverflows(uintptr_t start, size_t size)
{
    return size > std::numeric_limits<uintptr_t>::max() - start;
}
} // namespace

Range::Range(uintptr_t rangeStart, size_t rangeSize) : start(rangeStart), size(rangeSize) {}

bool Range::IsNull() const
{
    return size == 0;
}

bool Range::IsValid() const
{
    return !IsNull() && !AddOverflows(start, size);
}

uintptr_t Range::Start() const
{
    return start;
}

uintptr_t Range::End() const
{
    return start + size;
}

size_t Range::Size() const
{
    return size;
}

bool Range::Contains(const Range& other) const
{
    return IsValid() && other.IsValid() && start <= other.start && other.End() <= End();
}

bool Range::AdjacentTo(const Range& other) const
{
    return IsValid() && other.IsValid() && (End() == other.start || other.End() == start);
}

bool Range::GrowFromFront(size_t growSize)
{
    if (!IsValid() || growSize > start || growSize > std::numeric_limits<size_t>::max() - size) {
        return false;
    }
    start -= growSize;
    size += growSize;
    return true;
}

bool Range::GrowFromBack(size_t growSize)
{
    if (!IsValid() || AddOverflows(End(), growSize)) {
        return false;
    }
    size += growSize;
    return true;
}

Range Range::ShrinkFromFront(size_t shrinkSize)
{
    if (!IsValid() || shrinkSize == 0 || shrinkSize > size) {
        return Range();
    }
    const Range claimed(start, shrinkSize);
    start += shrinkSize;
    size -= shrinkSize;
    return claimed;
}

Range Range::ShrinkFromBack(size_t shrinkSize)
{
    if (!IsValid() || shrinkSize == 0 || shrinkSize > size) {
        return Range();
    }
    size -= shrinkSize;
    return Range(start + size, shrinkSize);
}

Range Range::Partition(size_t offset, size_t partitionSize) const
{
    if (!IsValid() || partitionSize == 0 || offset > size || partitionSize > size - offset) {
        return Range();
    }
    return Range(start + offset, partitionSize);
}

Range Range::FirstPart(size_t splitOffset) const
{
    return Partition(0, splitOffset);
}

Range Range::LastPart(size_t splitOffset) const
{
    if (!IsValid() || splitOffset >= size) {
        return Range();
    }
    return Partition(splitOffset, size - splitOffset);
}

bool Range::operator==(const Range& other) const
{
    return start == other.start && size == other.size;
}

bool Range::operator!=(const Range& other) const
{
    return !operator==(other);
}

void RangeRegistry::RegisterCallbacks(const Callbacks& newCallbacks)
{
    std::lock_guard<std::mutex> guard(lock);
    callbacks = newCallbacks;
}

bool RangeRegistry::RegisterRange(const Range& range)
{
    std::lock_guard<std::mutex> guard(lock);
    return InsertLocked(range, false);
}

bool RangeRegistry::Insert(const Range& range)
{
    std::lock_guard<std::mutex> guard(lock);
    return InsertLocked(range, true);
}

bool RangeRegistry::InsertLocked(const Range& range, bool prepareForHandBack)
{
    if (!range.IsValid()) {
        return false;
    }

    auto next = std::lower_bound(ranges.begin(), ranges.end(), range.Start(),
                                 [](const Range& current, uintptr_t start) { return current.Start() < start; });
    auto prev = next == ranges.begin() ? ranges.end() : std::prev(next);
    if ((prev != ranges.end() && prev->End() > range.Start()) ||
        (next != ranges.end() && range.End() > next->Start())) {
        return false;
    }

    if (prepareForHandBack && callbacks.prepareForHandBack != nullptr) {
        callbacks.prepareForHandBack(range, callbacks.context);
    }

    const bool joinsPrev = prev != ranges.end() && prev->End() == range.Start();
    const bool joinsNext = next != ranges.end() && range.End() == next->Start();
    if (joinsPrev) {
        GrowFromBackLocked(*prev, range.Size());
        if (joinsNext) {
            GrowFromBackLocked(*prev, next->Size());
            ranges.erase(next);
        }
    } else if (joinsNext) {
        GrowFromFrontLocked(*next, range.Size());
    } else {
        ranges.insert(next, range);
    }
    return true;
}

void RangeRegistry::GrowFromFrontLocked(Range& range, size_t growSize)
{
    const Range from = range;
    const Range to(from.Start() - growSize, from.Size() + growSize);
    if (callbacks.grow != nullptr) {
        callbacks.grow(from, to, callbacks.context);
    }
    (void)range.GrowFromFront(growSize);
}

void RangeRegistry::GrowFromBackLocked(Range& range, size_t growSize)
{
    const Range from = range;
    const Range to(from.Start(), from.Size() + growSize);
    if (callbacks.grow != nullptr) {
        callbacks.grow(from, to, callbacks.context);
    }
    (void)range.GrowFromBack(growSize);
}

Range RangeRegistry::ShrinkFromFrontLocked(Range& range, size_t shrinkSize)
{
    const Range from = range;
    const Range to = from.LastPart(shrinkSize);
    if (callbacks.shrink != nullptr) {
        callbacks.shrink(from, to, callbacks.context);
    }
    return range.ShrinkFromFront(shrinkSize);
}

Range RangeRegistry::ShrinkFromBackLocked(Range& range, size_t shrinkSize)
{
    const Range from = range;
    const Range to = from.FirstPart(from.Size() - shrinkSize);
    if (callbacks.shrink != nullptr) {
        callbacks.shrink(from, to, callbacks.context);
    }
    return range.ShrinkFromBack(shrinkSize);
}

void RangeRegistry::PrepareForHandOutLocked(const Range& range) const
{
    if (callbacks.prepareForHandOut != nullptr) {
        callbacks.prepareForHandOut(range, callbacks.context);
    }
}

Range RangeRegistry::ClaimLow(size_t claimSize)
{
    if (claimSize == 0) {
        return Range();
    }
    std::lock_guard<std::mutex> guard(lock);
    for (auto current = ranges.begin(); current != ranges.end(); ++current) {
        if (current->Size() < claimSize) {
            continue;
        }
        Range claimed;
        if (current->Size() == claimSize) {
            claimed = *current;
            ranges.erase(current);
        } else {
            claimed = ShrinkFromFrontLocked(*current, claimSize);
        }
        PrepareForHandOutLocked(claimed);
        return claimed;
    }
    return Range();
}

Range RangeRegistry::ClaimHigh(size_t claimSize)
{
    if (claimSize == 0) {
        return Range();
    }
    std::lock_guard<std::mutex> guard(lock);
    for (auto current = ranges.end(); current != ranges.begin();) {
        --current;
        if (current->Size() < claimSize) {
            continue;
        }
        Range claimed;
        if (current->Size() == claimSize) {
            claimed = *current;
            ranges.erase(current);
        } else {
            claimed = ShrinkFromBackLocked(*current, claimSize);
        }
        PrepareForHandOutLocked(claimed);
        return claimed;
    }
    return Range();
}

bool RangeRegistry::IsEmpty() const
{
    std::lock_guard<std::mutex> guard(lock);
    return ranges.empty();
}

bool RangeRegistry::IsContiguous() const
{
    std::lock_guard<std::mutex> guard(lock);
    return ranges.size() == 1;
}

bool RangeRegistry::Contains(const Range& range) const
{
    if (!range.IsValid()) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock);
    auto next = std::upper_bound(ranges.begin(), ranges.end(), range.Start(),
                                 [](uintptr_t start, const Range& current) { return start < current.Start(); });
    if (next == ranges.begin()) {
        return false;
    }
    return std::prev(next)->Contains(range);
}

size_t RangeRegistry::TotalSize() const
{
    std::lock_guard<std::mutex> guard(lock);
    size_t total = 0;
    for (const Range& range : ranges) {
        total += range.Size();
    }
    return total;
}

std::vector<Range> RangeRegistry::Snapshot() const
{
    std::lock_guard<std::mutex> guard(lock);
    return ranges;
}

} // namespace MapleRuntime
