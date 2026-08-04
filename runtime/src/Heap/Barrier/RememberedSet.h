// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMEMBERED_SET_H
#define MRT_REMEMBERED_SET_H

#include <mutex>
#include <unordered_set>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class Barrier;
class WCollector;
class RegionManager;

class RememberedSet final {
public:
    class Records final {
    public:
        using const_iterator = std::unordered_set<MAddress>::const_iterator;

        Records(const Records&) = delete;
        Records& operator=(const Records&) = delete;
        Records(Records&& other) noexcept : owner(other.owner), lock(std::move(other.lock)) { other.owner = nullptr; }
        Records& operator=(Records&&) = delete;

        ~Records()
        {
            if (owner != nullptr) {
                owner->records.clear();
            }
        }

        const_iterator begin() const { return owner->records.cbegin(); }
        const_iterator end() const { return owner->records.cend(); }
        size_t size() const { return owner->records.size(); }

    private:
        friend class RememberedSet;
        explicit Records(RememberedSet& rememberedSet) : owner(&rememberedSet), lock(rememberedSet.lock) {}

        RememberedSet* owner;
        std::unique_lock<std::mutex> lock;
    };

    RememberedSet() = default;
    RememberedSet(const RememberedSet&) = delete;
    RememberedSet& operator=(const RememberedSet&) = delete;

    Records AcquireRecordsForMinor() { return Records(*this); }

    // Non-destructive snapshot for diagnostic verify (does not clear). HotSpot card-table verify analog.
    std::unordered_set<MAddress> Snapshot() const
    {
        std::lock_guard<std::mutex> guard(lock);
        return records;
    }

    bool Contains(MAddress fieldAddress) const
    {
        std::lock_guard<std::mutex> guard(lock);
        return records.count(fieldAddress) != 0;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> guard(lock);
        return records.size();
    }

private:
    friend class Barrier;
    friend class WCollector;
    friend class RegionManager;

    void Record(MAddress fieldAddress)
    {
        std::lock_guard<std::mutex> guard(lock);
        records.insert(fieldAddress);
    }

    // HotSpot HeapRegionRemSet::clear() analogue: drop every field-slot whose address
    // falls inside a region that is about to be reclaimed/reused. Without this, the
    // next minor RescanRememberedSet reads freed payload as if it were still a holder.
    // COST: global unordered_set has no range index ⇒ O(N) full scan under lock.
    // outScanned receives N at call time when non-null.
    size_t EraseRange(MAddress start, MAddress end, size_t* outScanned = nullptr)
    {
        if (start >= end) {
            if (outScanned != nullptr) {
                *outScanned = 0;
            }
            return 0;
        }
        std::lock_guard<std::mutex> guard(lock);
        if (outScanned != nullptr) {
            *outScanned = records.size();
        }
        size_t erased = 0;
        for (auto it = records.begin(); it != records.end();) {
            MAddress slot = *it;
            if (slot >= start && slot < end) {
                it = records.erase(it);
                ++erased;
            } else {
                ++it;
            }
        }
        return erased;
    }

    mutable std::mutex lock;
    std::unordered_set<MAddress> records;
};
} // namespace MapleRuntime
#endif // MRT_REMEMBERED_SET_H
