// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MARK_FACE_SNAP_H
#define MRT_MARK_FACE_SNAP_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class RegionInfo;

// holdercapture: snapshot a region's *mark face* immediately before it is freed.
//
// Why this exists: youngclaim could not answer "was the crash holder marked, and by
// whom" because the join ran at crash time, and by then the holder's region was already
// free — its LiveInfo/markBitmap is gone, so IsMarkedObject() answers 0 for a reason
// that has nothing to do with what the collector saw. Reading the face later is not the
// same measurement as reading it before the free. This instrument moves the read to the
// last instant the face still exists (CollectRegion / ReleaseRegion entry, before
// InitFreeUnits), and stores it keyed by object address so the crash handler only has to
// look it up.
//
// Two faces are recorded, because the codebase has two (MARK_EPOCH_DISCIPLINE §10):
//   face A  ordinary region → LiveInfo::markBitmap bit for the object's offset
//   face B  large region    → metadata.isMarked single flag (IsMarkedObject(0))
// The row says which one answered, so a reader cannot silently take the wrong one.
//
// Gate: MRT_GCV2_HOLDERCAP=1, or the "holdercap" diagnostic token. Default off:
// product path early-returns before any counter, and no memory is allocated.
//
// Tunables (all optional):
//   MRT_GCV2_HOLDERCAP_OBJCAP=<pow2>   object-row capacity (default 1<<20)
//   MRT_GCV2_HOLDERCAP_MAXOBJ=<n>      per-region walk cap (default 16384)
//   MRT_GCV2_HOLDERCAP_WALK=0          region-level rows only, skip the object walk
namespace MarkFaceSnap {

bool Enabled();

// Called at the head of the free path, while the mark face is still readable.
// path uses RegionLifeDiag::FreePath codes.
void NoteRegionFree(RegionInfo* region, uint16_t path);

// Join an address (crash holder, CAS-null target, ...) against the snapshots.
// Prints exactly one line: exact object hit, interior hit, region-only hit, or miss.
//
// deepScan sweeps every row to resolve an interior address (a field, a RawArray+8)
// onto the object that contains it. That is a full pass over the row table, so it is
// for the crash handler only — a live-path caller would pay it on every hit.
void DumpJoinForAddr(uintptr_t addr, const char* tag, bool deepScan = false);

// Crash-time sweep over a set of candidate addresses (si_addr and the live
// registers). One pass over the row table tests all of them at once, so the cost
// does not multiply by the number of candidates. Prints a line per hit plus a
// census line, and never depends on any candidate being a plausible object —
// youngclaim's join produced 0 rows precisely because it required one.
void NoteCrashSweep(const uintptr_t* addrs, const char* const* names, size_t n);

// Activity proof + periodic persistence. Called at each GC end and atexit, so a
// timeout or SIGKILL cannot erase what was already observed.
void Report(const char* point);

} // namespace MarkFaceSnap
} // namespace MapleRuntime

#endif // MRT_MARK_FACE_SNAP_H
