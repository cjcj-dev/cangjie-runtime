// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_RELOCATION_DIAGNOSTIC_TABLE_H
#define MRT_RELOCATION_DIAGNOSTIC_TABLE_H

#include <cstddef>
#include <map>
#include <mutex>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class TypeInfo;

// r1missbucket diagnostic only. This table observes every completed
// CopyObject, including identity compaction deliberately rejected by
// ForwardFactTable. It is never consulted to choose or rewrite a reference.
class RelocationDiagnosticTable {
public:
    struct Entry {
        bool identity;
        BaseObject* from;
        BaseObject* to;
        size_t size;
        TypeInfo* typeInfo;
    };

    static RelocationDiagnosticTable& Instance() noexcept;

    void Record(BaseObject* from, BaseObject* to, size_t size, TypeInfo* typeInfo);
    bool Lookup(BaseObject* from, Entry& entry) const;
    bool LookupContaining(BaseObject* address, Entry& entry, size_t& offset) const;
    void Clear();
    size_t Size() const;

private:
    RelocationDiagnosticTable() = default;
    ~RelocationDiagnosticTable() = default;
    RelocationDiagnosticTable(const RelocationDiagnosticTable&) = delete;
    RelocationDiagnosticTable& operator=(const RelocationDiagnosticTable&) = delete;

    mutable std::mutex mutex;
    std::map<BaseObject*, Entry> table;
};
} // namespace MapleRuntime

#endif // MRT_RELOCATION_DIAGNOSTIC_TABLE_H
