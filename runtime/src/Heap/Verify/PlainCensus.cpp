// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/PlainCensus.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Common/ColourMask.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {

constexpr size_t kSampleLimit = 8;
constexpr size_t kDefaultTimeoutMs = 5000;
constexpr size_t kWriterSiteCount = static_cast<size_t>(PlainWriterSite::Count);

// Colour metadata = remap + young mark + old mark.
// TRUST_STATE_KILL_PLAN: plain = non-null address bits with all colour metadata zero.
constexpr MAddress kColourMetaMask =
    static_cast<MAddress>(REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK);
constexpr MAddress kAddressBitsMask = (MAddress(1) << 48) - 1;

thread_local PlainWriterSite g_tlsWriterSite = PlainWriterSite::Unknown;

std::atomic<uint64_t> g_plainWriteTotal{ 0 };
std::atomic<uint64_t> g_plainWriteBySite[kWriterSiteCount] = {};
std::atomic<bool> g_injectFired{ false };

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

bool IsPlainHeapRefValue(MAddress rawVal)
{
    if ((rawVal & kAddressBitsMask) == 0) {
        return false; // null (or colour-only null-ish) — allowed
    }
    return (rawVal & kColourMetaMask) == 0;
}

struct CensusStats {
    size_t objectsScanned = 0;
    size_t objectsWithRefs = 0;
    size_t totalSlots = 0;
    size_t nullSlots = 0;
    size_t colouredSlots = 0;
    size_t plainSlots = 0;
    size_t taggedPlainSlots = 0;
    size_t samplesTaken = 0;
    std::array<void*, kSampleLimit> sampleSlots{};
    std::array<MAddress, kSampleLimit> sampleVals{};
    uint64_t costNs = 0;
    bool timedOut = false;
};

void PushSample(CensusStats& stats, void* slot, MAddress val)
{
    if (stats.samplesTaken >= kSampleLimit) {
        return;
    }
    stats.sampleSlots[stats.samplesTaken] = slot;
    stats.sampleVals[stats.samplesTaken] = val;
    ++stats.samplesTaken;
}

} // namespace

const char* PlainWriterSiteName(PlainWriterSite site)
{
    switch (site) {
        case PlainWriterSite::Unknown:
            return "unknown";
        case PlainWriterSite::StoreColoured:
            return "store_coloured";
        case PlainWriterSite::CompareExchange:
            return "compare_exchange";
        case PlainWriterSite::Exchange:
            return "exchange";
        case PlainWriterSite::TryUntag:
            return "try_untag";
        case PlainWriterSite::FixMinorInterior:
            return "fix_minor_interior";
        case PlainWriterSite::RootSlotWritebackPlain:
            return "root_slot_writeback_plain";
        case PlainWriterSite::GetAndTryTag:
            return "get_and_try_tag";
        case PlainWriterSite::InjectPositive:
            return "inject_positive";
        default:
            return "invalid";
    }
}

PlainWriteColumn ColumnOf(PlainWriterSite site)
{
    switch (site) {
        case PlainWriterSite::FixMinorInterior:
            return PlainWriteColumn::DerivedLegal;
        case PlainWriterSite::Unknown:
            return PlainWriteColumn::Unknown;
        case PlainWriterSite::StoreColoured:
        case PlainWriterSite::CompareExchange:
        case PlainWriterSite::Exchange:
        case PlainWriterSite::TryUntag:
        case PlainWriterSite::RootSlotWritebackPlain:
        case PlainWriterSite::GetAndTryTag:
        case PlainWriterSite::InjectPositive:
            return PlainWriteColumn::HeapSlotPlain;
        default:
            return PlainWriteColumn::Unknown;
    }
}

ScopedPlainWriter::ScopedPlainWriter(PlainWriterSite site) : prev_(g_tlsWriterSite)
{
    g_tlsWriterSite = site;
}

ScopedPlainWriter::~ScopedPlainWriter() { g_tlsWriterSite = prev_; }

void NotePlainHeapWrite(const void* slot, uintptr_t newVal)
{
    if (!EnvIsOne("MRT_GCV2_PLAIN_WRITE_COUNT")) {
        return;
    }
    if (!IsPlainHeapRefValue(static_cast<MAddress>(newVal))) {
        return;
    }
    size_t idx = static_cast<size_t>(g_tlsWriterSite);
    if (idx >= kWriterSiteCount) {
        idx = static_cast<size_t>(PlainWriterSite::Unknown);
    }
    g_plainWriteTotal.fetch_add(1, std::memory_order_relaxed);
    g_plainWriteBySite[idx].fetch_add(1, std::memory_order_relaxed);
    // Sample first few at REPORT so positive control is visible in logs.
    static std::atomic<size_t> samples{ 0 };
    size_t n = samples.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
        // stderr so observation works without MRT_REPORT file plumbing.
        std::fprintf(stderr,
                     "[GCV2][plain][write] site=%s slot=%p val=%#zx total=%llu\n",
                     PlainWriterSiteName(g_tlsWriterSite), slot, static_cast<size_t>(newVal),
                     static_cast<unsigned long long>(g_plainWriteTotal.load(std::memory_order_relaxed)));
    }
}

void DumpPlainWriteCounters(const char* point)
{
    if (!EnvIsOne("MRT_GCV2_PLAIN_WRITE_COUNT") && !EnvIsOne("MRT_GCV2_PLAIN_CENSUS")) {
        return;
    }
    uint64_t total = g_plainWriteTotal.load(std::memory_order_relaxed);
    uint64_t bySite[kWriterSiteCount] = {};
    uint64_t colHeap = 0;
    uint64_t colDerived = 0;
    uint64_t colUnknown = 0;
    for (size_t i = 0; i < kWriterSiteCount; ++i) {
        bySite[i] = g_plainWriteBySite[i].load(std::memory_order_relaxed);
        switch (ColumnOf(static_cast<PlainWriterSite>(i))) {
            case PlainWriteColumn::HeapSlotPlain:
                colHeap += bySite[i];
                break;
            case PlainWriteColumn::DerivedLegal:
                colDerived += bySite[i];
                break;
            case PlainWriteColumn::Unknown:
            default:
                colUnknown += bySite[i];
                break;
        }
    }
    std::fprintf(stderr,
                 "[GCV2][plain][write-counters] point=%s total=%llu "
                 "unknown=%llu store_coloured=%llu cas=%llu exchange=%llu "
                 "try_untag=%llu fix_minor_interior=%llu root_writeback_plain=%llu "
                 "get_and_try_tag=%llu inject=%llu env=MRT_GCV2_PLAIN_WRITE_COUNT\n",
                 point == nullptr ? "?" : point, static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::Unknown)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::StoreColoured)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::CompareExchange)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::Exchange)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::TryUntag)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::FixMinorInterior)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::RootSlotWritebackPlain)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::GetAndTryTag)]),
                 static_cast<unsigned long long>(bySite[static_cast<size_t>(PlainWriterSite::InjectPositive)]));
    // derivedtype K1 columns: HeapSlot plain must go to 0; DerivedLegal may be large.
    std::fprintf(stderr,
                 "[GCV2][plain][write-columns] point=%s heap_plain=%llu derived_legal=%llu "
                 "unknown=%llu (K1=heap_plain)\n",
                 point == nullptr ? "?" : point, static_cast<unsigned long long>(colHeap),
                 static_cast<unsigned long long>(colDerived),
                 static_cast<unsigned long long>(colUnknown));
}

// Injection state for positive control: leave plain until census restores.
struct InjectState {
    RefField<>* slot = nullptr;
    zpointer saved = to_zpointer(0);
    bool active = false;
};
InjectState g_injectState;

bool InjectPlainHeapWriteOnce()
{
    if (!EnvIsOne("MRT_GCV2_PLAIN_WRITE_INJECT")) {
        return false;
    }
    bool expected = false;
    if (!g_injectFired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    bool injected = false;
    // concreffix: the slot must currently CARRY COLOUR, otherwise "install plain" writes back the
    // value that is already there and the census sees no delta -- a positive control that verifies
    // nothing while still printing POSITIVE_CONTROL. Observed in the field: the first eligible slot
    // was already colourless, so old == plain and the injected/non-injected plainHeapRefSlots
    // distributions were identical. Skipping already-plain slots is what makes this a real control;
    // the skip count is reported so a MISSED verdict says which reason it was.
    size_t skippedAlreadyPlain = 0;
    Heap::GetHeap().ForEachObj(
        [&injected, &skippedAlreadyPlain](BaseObject* obj) {
            if (injected || obj == nullptr || !obj->HasRefField()) {
                return;
            }
            obj->ForEachRefField([&injected, &skippedAlreadyPlain, obj](RefField<>& field) {
                if (injected) {
                    return;
                }
                zpointer oldZ = field.GetFieldValue();
                MAddress oldRaw = raw(oldZ);
                MAddress addr = oldRaw & kAddressBitsMask;
                if (addr == 0) {
                    return;
                }
                // Same predicate the census counts with, so an injected slot is guaranteed to move
                // a slot out of `coloured` and into `plainHeapRefSlots`.
                if ((oldRaw & kColourMetaMask) == 0) {
                    ++skippedAlreadyPlain;
                    return;
                }
                // Install plain = address bits only (zero colour metadata + zero tag).
                MAddress plainRaw = addr;
                ScopedPlainWriter tag(PlainWriterSite::InjectPositive);
                // Bypass AssertColouredWrite CHECK by using CompareExchange only when
                // ASSERT is off; when ASSERT is on, still count via Note path if COUNT on.
                // Direct store through CompareExchange invokes Assert — if that aborts,
                // inject is only for COUNT/CENSUS arms (task: fail-open counters).
                if (EnvIsOne("MRT_GCV2_ASSERT_COLOURED_WRITES")) {
                    // Count-only positive control without mutating product heap.
                    size_t idx = static_cast<size_t>(PlainWriterSite::InjectPositive);
                    g_plainWriteTotal.fetch_add(1, std::memory_order_relaxed);
                    g_plainWriteBySite[idx].fetch_add(1, std::memory_order_relaxed);
                    std::fprintf(stderr,
                                 "[GCV2][plain][inject] POSITIVE_CONTROL_COUNT_ONLY "
                                 "(ASSERT on — no heap mutate) holder=%p slot=%p val=%#zx\n",
                                 obj, &field, static_cast<size_t>(plainRaw));
                    injected = true;
                    return;
                }
                if (HealSlot(field, oldZ, to_zpointer(plainRaw), HealSite::PlainCensusInject)) {
                    // When PLAIN_WRITE_COUNT=1, AssertColouredWrite already Note'd via TLS.
                    // When off, bump inject counter so DumpPlainWriteCounters still shows >0.
                    if (!EnvIsOne("MRT_GCV2_PLAIN_WRITE_COUNT")) {
                        size_t idx = static_cast<size_t>(PlainWriterSite::InjectPositive);
                        g_plainWriteTotal.fetch_add(1, std::memory_order_relaxed);
                        g_plainWriteBySite[idx].fetch_add(1, std::memory_order_relaxed);
                    }
                    g_injectState.slot = &field;
                    g_injectState.saved = oldZ;
                    g_injectState.active = true;
                    // delta!=0 is the evidence the write changed the slot; a control that prints
                    // old == plain proved nothing and must not read as a pass.
                    std::fprintf(stderr,
                                 "[GCV2][plain][inject] POSITIVE_CONTROL holder=%p slot=%p "
                                 "old=%#zx plain=%#zx delta=%#zx colour_cleared=%u "
                                 "skipped_already_plain=%zu leave_until_census_restore\n",
                                 obj, &field, static_cast<size_t>(oldRaw), static_cast<size_t>(plainRaw),
                                 static_cast<size_t>(oldRaw ^ plainRaw),
                                 static_cast<unsigned>((oldRaw & kColourMetaMask) != 0), skippedAlreadyPlain);
                    injected = true;
                }
            });
        },
        false);

    if (!injected) {
        // Say which reason: no eligible slot at all, or every candidate was already colourless.
        // The second case means the heap genuinely holds no coloured slot at this point, which is
        // itself the answer the census is being asked for -- not a broken apparatus.
        std::fprintf(stderr,
                     "[GCV2][plain][inject] POSITIVE_CONTROL_MISSED no coloured non-null heap slot "
                     "found skipped_already_plain=%zu\n",
                     skippedAlreadyPlain);
    }
    return injected;
}

void RestoreInjectedPlainIfAny()
{
    if (!g_injectState.active || g_injectState.slot == nullptr) {
        return;
    }
    MAddress cur = raw(g_injectState.slot->GetFieldValue());
    MAddress plainExpect = cur & kAddressBitsMask; // what we wrote
    (void)HealSlot(*g_injectState.slot, to_zpointer(plainExpect), g_injectState.saved,
                   HealSite::PlainCensusRestore);
    std::fprintf(stderr, "[GCV2][plain][inject] restored slot=%p to=%#zx\n", g_injectState.slot,
                 static_cast<size_t>(raw(g_injectState.saved)));
    g_injectState.active = false;
    g_injectState.slot = nullptr;
}

void RunPlainCensus(const char* point, bool force)
{
    if (!force && !EnvIsOne("MRT_GCV2_PLAIN_CENSUS")) {
        return;
    }

    static std::atomic<size_t> invokeCount{ 0 };
    size_t invoke = invokeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t every = EnvSizeT("MRT_GCV2_PLAIN_CENSUS_EVERY", 1);
    if (every == 0) {
        every = 1;
    }
    if ((invoke - 1) % every != 0) {
        return;
    }

    size_t timeoutMs = EnvSizeT("MRT_GCV2_PLAIN_CENSUS_TIMEOUT_MS", kDefaultTimeoutMs);
    size_t maxSamples = EnvSizeT("MRT_GCV2_PLAIN_CENSUS_MAX_SAMPLES", kSampleLimit);
    if (maxSamples > kSampleLimit) {
        maxSamples = kSampleLimit;
    }

    // Optional one-shot inject before measuring so the gate is not a constant-zero door.
    if (EnvIsOne("MRT_GCV2_PLAIN_WRITE_INJECT")) {
        (void)InjectPlainHeapWriteOnce();
    }

    CensusStats stats;
    uint64_t startNs = TimeUtil::NanoSeconds();
    uint64_t deadlineNs = startNs + static_cast<uint64_t>(timeoutMs) * 1000000ULL;

    Heap::GetHeap().ForEachObj(
        [&stats, deadlineNs, maxSamples](BaseObject* obj) {
            if (stats.timedOut) {
                return;
            }
            if (TimeUtil::NanoSeconds() > deadlineNs) {
                stats.timedOut = true;
                return;
            }
            if (obj == nullptr) {
                return;
            }
            ++stats.objectsScanned;
            if (!obj->HasRefField()) {
                return;
            }
            ++stats.objectsWithRefs;
            obj->ForEachRefField([&stats, maxSamples](RefField<>& field) {
                MAddress val = raw(field.GetFieldValue());
                ++stats.totalSlots;
                if ((val & kAddressBitsMask) == 0) {
                    ++stats.nullSlots;
                    return;
                }
                if ((val & kColourMetaMask) != 0) {
                    ++stats.colouredSlots;
                    return;
                }
                ++stats.plainSlots;
                if (stats.samplesTaken < maxSamples) {
                    PushSample(stats, &field, val);
                }
            });
        },
        false);

    stats.costNs = TimeUtil::NanoSeconds() - startNs;
    double plainRatio =
        stats.totalSlots == 0 ? 0.0 : (100.0 * static_cast<double>(stats.plainSlots) / static_cast<double>(stats.totalSlots));

    std::fprintf(stderr,
                 "[GCV2][plain][census] point=%s invoke=%zu env=MRT_GCV2_PLAIN_CENSUS "
                 "objects=%zu withRefs=%zu totalSlots=%zu null=%zu coloured=%zu "
                 "plainHeapRefSlots=%zu taggedPlain=%zu plainPct=%.4f "
                 "costNs=%llu timeoutMs=%zu timedOut=%u "
                 "samples=[%p:%#zx,%p:%#zx,%p:%#zx,%p:%#zx]\n",
                 point == nullptr ? "?" : point, invoke, stats.objectsScanned, stats.objectsWithRefs, stats.totalSlots,
                 stats.nullSlots, stats.colouredSlots, stats.plainSlots, stats.taggedPlainSlots, plainRatio,
                 static_cast<unsigned long long>(stats.costNs), timeoutMs, static_cast<unsigned>(stats.timedOut),
                 stats.sampleSlots[0], static_cast<size_t>(stats.sampleVals[0]), stats.sampleSlots[1],
                 static_cast<size_t>(stats.sampleVals[1]), stats.sampleSlots[2], static_cast<size_t>(stats.sampleVals[2]),
                 stats.sampleSlots[3], static_cast<size_t>(stats.sampleVals[3]));

    RestoreInjectedPlainIfAny();
    DumpPlainWriteCounters(point);
}

} // namespace MapleRuntime
