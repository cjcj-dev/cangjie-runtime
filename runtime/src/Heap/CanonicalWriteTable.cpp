// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CanonicalWriteTable.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Allocator/RegionInfo.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Panic.h"
#include "Common/BaseObject.h"

namespace MapleRuntime {
CanonicalWriteTable& CanonicalWriteTable::Instance() noexcept
{
    static CanonicalWriteTable instance;
    return instance;
}

void CanonicalWriteTable::ConfigureFromEnvironment()
{
    const char* value = std::getenv("MRT_CANONICAL_WRITE");
    if (value == nullptr || strcmp(value, "0") == 0) {
        mode = Mode::OFF;
    } else if (strcmp(value, "1") == 0) {
        mode = Mode::ON;
    } else if (strcmp(value, "validate") == 0) {
        mode = Mode::VALIDATE;
    } else if (strcmp(value, "positive") == 0) {
        mode = Mode::POSITIVE_CONTROL;
    } else {
        mode = Mode::OFF;
        LOG(RTLOG_ERROR,
            "Unsupported MRT_CANONICAL_WRITE=%s; expected 0, 1, validate, or positive; using 0", value);
    }
    LOG(RTLOG_INFO, "canonical write normalization: %s", mode == Mode::OFF ? "off" :
        mode == Mode::ON ? "on" : mode == Mode::VALIDATE ? "validate" : "positive-control");
}

void CanonicalWriteTable::Record(BaseObject* from, BaseObject* to, size_t size)
{
    if (!IsEnabled() || from == nullptr || to == nullptr || size == 0) {
        return;
    }
    RegionInfo* fromRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(from));
    RegionInfo* toRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(to));
    std::lock_guard<std::mutex> lock(mutex);
    stagingSourceRegions.insert(fromRegion);
    if (fromRegion == toRegion) {
        ++compactSkipped;
        return;
    }

    const uintptr_t fromAddress = reinterpret_cast<uintptr_t>(from);
    Entry entry{ fromAddress, reinterpret_cast<uintptr_t>(to), size, fromRegion, toRegion };
    auto result = staging.emplace(fromAddress, entry);
    if (!result.second && result.first->second.to != entry.to) {
        VLOG(REPORT, "[CanonicalWriteTable] reject rewrite from=%p old_to=%p new_to=%p", from,
             reinterpret_cast<void*>(result.first->second.to), to);
    }
}

void CanonicalWriteTable::Publish()
{
    if (!IsEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    active.store(false, std::memory_order_relaxed);
    activeExact.clear();
    activeRanges.clear();
    overlapSkipped = 0;
    activeExact.reserve(staging.size());
    activeRanges.reserve(staging.size());
    for (const auto& item : staging) {
        const Entry& entry = item.second;
        if (stagingSourceRegions.find(entry.toRegion) != stagingSourceRegions.end()) {
            ++overlapSkipped;
            continue;
        }
        activeExact.emplace(entry.from, entry.to);
        activeRanges.push_back(entry);
    }
    std::sort(activeRanges.begin(), activeRanges.end(),
        [](const Entry& lhs, const Entry& rhs) { return lhs.from < rhs.from; });
    staging.clear();
    stagingSourceRegions.clear();
    lookupCount.store(0, std::memory_order_relaxed);
    canonicalizedCount.store(0, std::memory_order_relaxed);
    validationCount.store(0, std::memory_order_relaxed);
    bypassNextCanonicalization.store(false, std::memory_order_relaxed);
    publishedCycle = true;
    active.store(!activeRanges.empty(), std::memory_order_release);
    VLOG(REPORT,
         "[CanonicalWriteTable] publish facts=%zu compact_skipped=%zu overlap_skipped=%zu",
         activeRanges.size(), compactSkipped, overlapSkipped);
}

void CanonicalWriteTable::Retire()
{
    if (!IsEnabled()) {
        return;
    }
    // PrepareFromSpace has already retired the ghost routes and its phase
    // handshake has excluded IDLE readers before this store and clear.
    active.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex);
    if (publishedCycle) {
        VLOG(REPORT,
             "[CanonicalWriteTable] retire facts=%zu lookups=%zu canonicalized=%zu validations=%zu "
             "compact_skipped=%zu overlap_skipped=%zu",
             activeRanges.size(), lookupCount.load(std::memory_order_relaxed),
             canonicalizedCount.load(std::memory_order_relaxed), validationCount.load(std::memory_order_relaxed),
             compactSkipped, overlapSkipped);
    }
    activeExact.clear();
    activeRanges.clear();
    staging.clear();
    stagingSourceRegions.clear();
    publishedCycle = false;
    compactSkipped = 0;
    overlapSkipped = 0;
}

BaseObject* CanonicalWriteTable::Resolve(BaseObject* reference) const
{
    if (reference == nullptr || !active.load(std::memory_order_acquire)) {
        return nullptr;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(reference);
    auto exact = activeExact.find(address);
    if (exact != activeExact.end()) {
        return reinterpret_cast<BaseObject*>(exact->second);
    }
    auto range = std::upper_bound(activeRanges.begin(), activeRanges.end(), address,
        [](uintptr_t target, const Entry& entry) { return target < entry.from; });
    if (range == activeRanges.begin()) {
        return nullptr;
    }
    --range;
    if (address < range->from || address - range->from >= range->size) {
        return nullptr;
    }
    return reinterpret_cast<BaseObject*>(range->to + (address - range->from));
}

BaseObject* CanonicalWriteTable::Canonicalize(BaseObject* reference)
{
    if (reference == nullptr || !active.load(std::memory_order_acquire)) {
        return reference;
    }
    lookupCount.fetch_add(1, std::memory_order_relaxed);
    BaseObject* resolved = Resolve(reference);
    if (resolved == nullptr || resolved == reference) {
        return reference;
    }
    if (IsPositiveControlEnabled() && bypassNextCanonicalization.exchange(false, std::memory_order_relaxed)) {
        return reference;
    }
    canonicalizedCount.fetch_add(1, std::memory_order_relaxed);
    return resolved;
}

void CanonicalWriteTable::ValidatePublished(BaseObject* reference)
{
    if (!IsValidationEnabled()) {
        return;
    }
    validationCount.fetch_add(1, std::memory_order_relaxed);
    BaseObject* resolved = Resolve(reference);
    CHECK_DETAIL(resolved == nullptr || resolved == reference,
                 "canonical write validator observed an unresolved old publication: from=%p to=%p",
                 reference, resolved);
}

BaseObject* CanonicalWriteTable::GetPositiveControlSource() const
{
    if (!IsPositiveControlEnabled() || !active.load(std::memory_order_acquire) || activeRanges.empty()) {
        return nullptr;
    }
    return reinterpret_cast<BaseObject*>(activeRanges.front().from);
}

void CanonicalWriteTable::ArmPositiveControl()
{
    CHECK(IsPositiveControlEnabled());
    bypassNextCanonicalization.store(true, std::memory_order_relaxed);
}
} // namespace MapleRuntime
