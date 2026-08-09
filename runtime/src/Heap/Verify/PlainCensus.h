// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PLAIN_CENSUS_H
#define MRT_PLAIN_CENSUS_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// TRUST_STATE_KILL_PLAN Phase 1a: observe non-null HeapSlot values whose colour
// metadata is all-zero (plain heap refs). RootSlot/DerivedSlot are not counted.
//
// Gate (default off):  MRT_GCV2_PLAIN_CENSUS=1
// Cost control:        MRT_GCV2_PLAIN_CENSUS_EVERY=<N>      (1-based invoke skip)
//                      MRT_GCV2_PLAIN_CENSUS_TIMEOUT_MS=<N> (default 5000; report timeout)
// Sample cap:          MRT_GCV2_PLAIN_CENSUS_MAX_SAMPLES=<N> (default 8)
// force=true: run even when env is unset (hooks pass force at known GC points).
//
// Writer attribution (fail-open counters, default off):
//   MRT_GCV2_PLAIN_WRITE_COUNT=1  — count plain/bad-colour heap writes at choke
//   MRT_GCV2_ASSERT_COLOURED_WRITES=1 — existing CHECK path (unchanged)
//   MRT_GCV2_PLAIN_WRITE_INJECT=1 — one-shot positive control: install a plain
//                                   heap slot then re-census / fire count path
//
// K1 target: plainHeapRefSlots == 0 (this module only measures; no semantic change).

enum class PlainWriterSite : uint8_t {
    Unknown = 0,
    StoreColoured,
    CompareExchange,
    Exchange,
    TryUntag,
    FixMinorInterior,
    RootSlotWritebackPlain,
    GetAndTryTag,
    InjectPositive,
    Count // sentinel
};

const char* PlainWriterSiteName(PlainWriterSite site);

// RAII: tag the current thread's HeapSlot write as coming from `site`.
// Nested scopes restore the previous site on exit.
class ScopedPlainWriter {
public:
    explicit ScopedPlainWriter(PlainWriterSite site);
    ~ScopedPlainWriter();
    ScopedPlainWriter(const ScopedPlainWriter&) = delete;
    ScopedPlainWriter& operator=(const ScopedPlainWriter&) = delete;

private:
    PlainWriterSite prev_;
};

// Called from AssertColouredWriteIfEnabled when a non-null heap write lacks colour.
// Fail-open: only increments counters when MRT_GCV2_PLAIN_WRITE_COUNT=1.
void NotePlainHeapWrite(const void* slot, uintptr_t newVal);

// Full-heap census at a named GC point. Safe to call under STW or when mutators
// are suspended; walks Heap::ForEachObj + ForEachRefField (same shape as VerifyHeap).
void RunPlainCensus(const char* point, bool force = false);

// Positive-control inject: if MRT_GCV2_PLAIN_WRITE_INJECT=1 and not yet fired,
// installs one plain non-null value into a live heap slot (via CompareExchange of
// a snapshot), notes the writer, then restores the original value. Returns true
// if injection ran. Does nothing when env is off.
bool InjectPlainHeapWriteOnce();

// Dump writer counters (also emitted at end of each census when count env is on).
void DumpPlainWriteCounters(const char* point);

} // namespace MapleRuntime

#endif // MRT_PLAIN_CENSUS_H
