// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REMEMBERED_SET_H
#define MRT_REMEMBERED_SET_H

#include <mutex>
#include <set>
#include <unordered_set>
#if defined(MRT_REMSET_ERASE_RANGE_CROSSCHECK)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

#include "Common/TypeDef.h"

namespace MapleRuntime {
class Barrier;
class WCollector;
class RegionManager;

class RememberedSet final {
public:
    class Records final {
    public:
        using const_iterator = std::set<MAddress>::const_iterator;

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
        return std::unordered_set<MAddress>(records.cbegin(), records.cend());
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
#if defined(MRT_REMSET_ERASE_RANGE_CROSSCHECK)
    friend class RememberedSetTest;
#endif

    void Record(MAddress fieldAddress)
    {
        std::lock_guard<std::mutex> guard(lock);
        records.insert(fieldAddress);
    }

    // HotSpot HeapRegionRemSet::clear() analogue: drop every field-slot whose address
    // falls inside a region that is about to be reclaimed/reused. Without this, the
    // next minor RescanRememberedSet reads freed payload as if it were still a holder.
    // The address-ordered index limits work to entries in the reclaimed range.
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
#if defined(MRT_REMSET_ERASE_RANGE_CROSSCHECK)
        const char* crossCheckEnv = std::getenv("MRT_GCV2_VERIFY_REMSET_ERASE_RANGE");
        bool crossCheck = crossCheckEnv != nullptr && std::strcmp(crossCheckEnv, "1") == 0;
        std::unordered_set<MAddress> legacyRecords;
        if (crossCheck) {
            legacyRecords.insert(records.cbegin(), records.cend());
        }
#endif
        if (outScanned != nullptr) {
            *outScanned = records.size();
        }
        auto first = records.lower_bound(start);
        auto last = records.lower_bound(end);
        size_t erased = static_cast<size_t>(std::distance(first, last));
        records.erase(first, last);
#if defined(MRT_REMSET_ERASE_RANGE_CROSSCHECK)
        if (crossCheck) {
            size_t legacyErased = 0;
            for (auto it = legacyRecords.begin(); it != legacyRecords.end();) {
                MAddress slot = *it;
                if (slot >= start && slot < end) {
                    it = legacyRecords.erase(it);
                    ++legacyErased;
                } else {
                    ++it;
                }
            }
            const char* injectEnv = std::getenv("MRT_GCV2_VERIFY_REMSET_ERASE_RANGE_INJECT_MISMATCH");
            bool injected = injectEnv != nullptr && std::strcmp(injectEnv, "1") == 0;
            if (injected) {
                legacyRecords.insert(start);
            }
            bool equivalent = erased == legacyErased && records.size() == legacyRecords.size();
            for (MAddress slot : records) {
                equivalent = equivalent && legacyRecords.count(slot) != 0;
            }
            if (!equivalent) {
                std::fprintf(stderr,
                    "ERASE_RANGE_CROSSCHECK_MISMATCH injected=%u start=%#zx end=%#zx new_erased=%zu "
                    "legacy_erased=%zu new_size=%zu legacy_size=%zu\n",
                    static_cast<unsigned>(injected), static_cast<size_t>(start), static_cast<size_t>(end), erased,
                    legacyErased, records.size(), legacyRecords.size());
                std::abort();
            }
            ++crossCheckCount;
        }
#endif
        return erased;
    }

    mutable std::mutex lock;
    std::set<MAddress> records;
#if defined(MRT_REMSET_ERASE_RANGE_CROSSCHECK)
    size_t crossCheckCount = 0;
#endif
};
} // namespace MapleRuntime
#endif // MRT_REMEMBERED_SET_H
