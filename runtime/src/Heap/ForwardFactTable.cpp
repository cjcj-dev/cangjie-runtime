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

void ForwardFactTable::Record(BaseObject* from, BaseObject* to)
{
    if (from == nullptr || to == nullptr || from == to) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    // First writer wins (object lock makes double-copy impossible for same from;
    // keep emplace so a spurious second Record cannot flip to-address).
    auto result = table.emplace(from, to);
    if (result.second) {
        count.fetch_add(1, std::memory_order_relaxed);
    } else if (result.first->second != to) {
        // Partial/aborted path must not rewrite: keep first complete mapping.
        VLOG(REPORT, "[ForwardFactTable] reject rewrite from=%p old_to=%p new_to=%p", from, result.first->second, to);
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
    return it->second;
}

void ForwardFactTable::Clear()
{
    std::lock_guard<std::mutex> lg(mutex);
    table.clear();
    count.store(0, std::memory_order_relaxed);
}
} // namespace MapleRuntime
