// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RelocationDiagnosticTable.h"

namespace MapleRuntime {
RelocationDiagnosticTable& RelocationDiagnosticTable::Instance() noexcept
{
    static RelocationDiagnosticTable instance;
    return instance;
}

void RelocationDiagnosticTable::Record(BaseObject* from, BaseObject* to, size_t size, TypeInfo* typeInfo)
{
    if (from == nullptr || to == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lg(mutex);
    table.emplace(from, Entry{ from == to, from, to, size, typeInfo });
}

bool RelocationDiagnosticTable::LookupContaining(BaseObject* address, Entry& entry, size_t& offset) const
{
    std::lock_guard<std::mutex> lg(mutex);
    auto it = table.upper_bound(address);
    if (it == table.begin()) {
        return false;
    }
    --it;
    const uintptr_t start = reinterpret_cast<uintptr_t>(it->second.from);
    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    if (target < start || target - start >= it->second.size) {
        return false;
    }
    entry = it->second;
    offset = target - start;
    return true;
}

bool RelocationDiagnosticTable::Lookup(BaseObject* from, Entry& entry) const
{
    std::lock_guard<std::mutex> lg(mutex);
    auto it = table.find(from);
    if (it == table.end()) {
        return false;
    }
    entry = it->second;
    return true;
}

void RelocationDiagnosticTable::Clear()
{
    std::lock_guard<std::mutex> lg(mutex);
    table.clear();
}

size_t RelocationDiagnosticTable::Size() const
{
    std::lock_guard<std::mutex> lg(mutex);
    return table.size();
}
} // namespace MapleRuntime
