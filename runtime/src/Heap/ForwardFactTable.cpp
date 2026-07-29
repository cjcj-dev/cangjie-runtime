// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ForwardFactTable.h"

#include "Base/LogFile.h"

namespace MapleRuntime {
ForwardFactTable& ForwardFactTable::Instance() noexcept
{
    static ForwardFactTable instance;
    return instance;
}

void ForwardFactTable::Record(BaseObject* from, BaseObject* to, size_t size)
{
    if (from == nullptr || to == nullptr || from == to) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    // First writer wins (object lock makes double-copy impossible for same from;
    // keep emplace so a spurious second Record cannot flip to-address).
    auto result = table.emplace(from, Entry{ to, size });
    if (result.second) {
        count.fetch_add(1, std::memory_order_relaxed);
    } else if (result.first->second.to != to) {
        // Partial/aborted path must not rewrite: keep first complete mapping.
        VLOG(REPORT, "[ForwardFactTable] reject rewrite from=%p old_to=%p new_to=%p", from,
             result.first->second.to, to);
    }
}

BaseObject* ForwardFactTable::Lookup(BaseObject* from) const
{
    if (from == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lg(mutex);
    auto it = table.find(from);
    if (it == table.end()) {
        return nullptr;
    }
    return it->second.to;
}

bool ForwardFactTable::LookupContaining(BaseObject* address, BaseObject*& to, size_t& offset) const
{
    if (address == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lg(mutex);
    auto it = table.upper_bound(address);
    if (it == table.begin()) {
        return false;
    }
    --it;
    const uintptr_t source = reinterpret_cast<uintptr_t>(it->first);
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    if (target <= source || target - source >= it->second.size) {
        return false;
    }
    offset = target - source;
    to = reinterpret_cast<BaseObject*>(reinterpret_cast<uintptr_t>(it->second.to) + offset);
    return true;
}

void ForwardFactTable::Clear()
{
    std::lock_guard<std::mutex> lg(mutex);
    table.clear();
    count.store(0, std::memory_order_relaxed);
}
} // namespace MapleRuntime
