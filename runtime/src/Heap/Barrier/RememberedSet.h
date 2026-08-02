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

private:
    friend class Barrier;

    void Record(MAddress fieldAddress)
    {
        std::lock_guard<std::mutex> guard(lock);
        records.insert(fieldAddress);
    }

    std::mutex lock;
    std::unordered_set<MAddress> records;
};
} // namespace MapleRuntime
#endif // MRT_REMEMBERED_SET_H
