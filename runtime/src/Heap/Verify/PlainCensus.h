// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
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
// K1 target: HeapSlot plain-write column == 0 (DerivedLegal column may be non-zero).
// Steady-state plainHeapRefSlots on heap fields should also be ~0 after minor fix drains.

// Writer sites feed three census columns (derivedtype / trustkill K1):
//   HeapSlot plain  = K1 residual (must go to 0)
//   Derived legal   = FixMinorInterior / named interior plain (legal by 03fc21ed)
//   unknown         = untagged choke hits
enum class PlainWriterSite : uint8_t {
    Unknown = 0,
    StoreColoured,
    CompareExchange,
    Exchange,
    TryUntag,
    FixMinorInterior, // derived-legal column (not K1)
    RootSlotWritebackPlain,
    GetAndTryTag,
    InjectPositive, // heap-plain K1 positive control
    Count // sentinel
};

enum class PlainWriteColumn : uint8_t {
    HeapSlotPlain = 0, // K1
    DerivedLegal = 1,  // FixMinorInterior interiors
    Unknown = 2,
    Count = 3
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
// Prints per-site counts plus three columns: heap_plain / derived_legal / unknown.
void DumpPlainWriteCounters(const char* point);

PlainWriteColumn ColumnOf(PlainWriterSite site);

} // namespace MapleRuntime

#endif // MRT_PLAIN_CENSUS_H
