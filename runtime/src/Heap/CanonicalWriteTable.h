// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_CANONICAL_WRITE_TABLE_H
#define MRT_CANONICAL_WRITE_TABLE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/TypeDef.h"

namespace MapleRuntime {
class BaseObject;
class RegionInfo;

// Copy-time facts retained until the corresponding ghost routes retire. Only
// facts whose destination region is outside every source region of the cycle
// are published: an in-place compacted address cannot identify which object
// version its numeric value names.
class CanonicalWriteTable {
public:
    static CanonicalWriteTable& Instance() noexcept;

    void ConfigureFromEnvironment();
    bool IsEnabled() const { return mode != Mode::OFF; }
    bool IsValidationEnabled() const { return mode == Mode::VALIDATE || mode == Mode::POSITIVE_CONTROL; }
    bool IsPositiveControlEnabled() const { return mode == Mode::POSITIVE_CONTROL; }

    // Copy-time producer. Concurrent forwarding workers may call this method.
    void Record(BaseObject* from, BaseObject* to, size_t size);

    // Publish after forwarding has joined; retire after the old ghost routes
    // have been dispelled. The phase handshake excludes IDLE readers while the
    // backing containers are built or cleared.
    void Publish();
    void Retire();

    BaseObject* Canonicalize(BaseObject* reference);
    void ValidatePublished(BaseObject* reference);

    BaseObject* GetPositiveControlSource() const;
    void ArmPositiveControl();

private:
    enum class Mode : uint8_t {
        OFF,
        ON,
        VALIDATE,
        POSITIVE_CONTROL,
    };

    struct Entry {
        uintptr_t from;
        uintptr_t to;
        size_t size;
        RegionInfo* fromRegion;
        RegionInfo* toRegion;
    };

    CanonicalWriteTable() = default;
    ~CanonicalWriteTable() = default;
    CanonicalWriteTable(const CanonicalWriteTable&) = delete;
    CanonicalWriteTable& operator=(const CanonicalWriteTable&) = delete;

    BaseObject* Resolve(BaseObject* reference) const;

    Mode mode = Mode::OFF;
    mutable std::mutex mutex;
    std::unordered_map<uintptr_t, Entry> staging;
    std::unordered_set<RegionInfo*> stagingSourceRegions;
    std::unordered_map<uintptr_t, uintptr_t> activeExact;
    std::vector<Entry> activeRanges;
    std::atomic<bool> active{ false };
    bool publishedCycle = false;
    size_t compactSkipped = 0;
    size_t overlapSkipped = 0;
    std::atomic<bool> bypassNextCanonicalization{ false };
    std::atomic<size_t> lookupCount{ 0 };
    std::atomic<size_t> canonicalizedCount{ 0 };
    std::atomic<size_t> validationCount{ 0 };
};
} // namespace MapleRuntime

#endif // MRT_CANONICAL_WRITE_TABLE_H
