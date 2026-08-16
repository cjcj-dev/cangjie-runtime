// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LOAD_GOOD_PROBE_H
#define MRT_LOAD_GOOD_PROBE_H

// loadgood: observation-only instrument for ZGC_LOAD_BARRIER_PARITY §四·①.
//
// The question: at the moment a reference is loaded, does the colour carried by the loaded
// word already say the value is stale, or does only a heap-state query say so?
//
// ZGC asks the first question (zBarrier.inline.hpp:319-341 barrier(): `if (fast_path(o))`,
// with fast_path = ZPointer::is_load_good, i.e. `(untype(ptr) & ZPointerLoadBadMask) == 0`
// plus non-null -- zAddress.inline.hpp:627-636). Barrier::ReadStaticRef asks the second
// (Heap::IsHeapAddress + Collector::IsGhostFromObject on the already-stripped target).
//
// This probe evaluates both predicates on the same loaded word and cross-tabulates them.
// It changes no return value and no control flow: every counter sits behind Enabled(),
// which is false unless MRT_GCV2_LOADGOOD=1 or the "loadgood" diagnostic token is set.
//
// Faces are separate bucket sets and must not be summed:
//   root -- Barrier::ReadStaticRef, a RootSlot. Our RootSlot mirrors OpenJDK ZUncoloredRoot
//           (RefField.h:380-382) and every writer reaches it through StorePlain(zaddress).
//   heap -- IdleBarrier::ReadReference, a HeapSlot. Heap slots are written coloured
//           (StoreColoured / GetAndTryTagRefField), so this face is the positive control:
//           it is the same instrument on a slot class that does carry colour.

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
namespace LoadGoodProbe {

constexpr uint8_t kFaceRoot = 0;
constexpr uint8_t kFaceHeap = 1;
constexpr uint8_t kFaceCount = 2;

// Set once during dynamic initialisation from MRT_GCV2_LOADGOOD / diag token "loadgood".
// Read directly so the disabled path is a single byte load and a not-taken branch, with
// no atomic, no getenv and no counter touched.
extern bool g_enabled;

inline bool Enabled() { return g_enabled; }

// A null slot read. Counted so the denominator covers every call, not only non-null ones.
void NoteNull(uint8_t face);

// One non-null load.
//   word     -- the raw machine word as it sat in the slot (colour bits intact)
//   ghost    -- what the production predicate on this path decided
//               (target != null && Heap::IsHeapAddress(target) && IsGhostFromObject(target))
//   loadGood -- Collector::is_load_good on the same word (our full predicate: positive
//               remap colour in both generations). Passed in rather than recomputed so the
//               probe records exactly what production would have decided.
void NoteRead(uint8_t face, uintptr_t word, bool ghost, bool loadGood);

// Raw before/after pairs, sampled (ring, capped). Two rings, asked by two questions:
//
//   bad   -- the loaded word tested load-bad, and what stripping it produced. Separates
//            "the colour already knew" (situation 1) from "the colour was fine and the
//            address is still wrong" (situation 2).
//   route -- a read whose production predicate said ghost, and what FindLatestVersion
//            returned for it. delta = resolved - stripped, in bytes, signed.
//            This is the only place the geometric route's answer is visible next to
//            its input; see the note on GetRoute/preLiveBytes in the report.
void NoteBadSample(uint8_t face, uintptr_t word, uintptr_t stripped);
void NoteRouteSample(uint8_t face, uintptr_t word, uintptr_t stripped, uintptr_t resolved);

// Prints every bucket for both faces. Idempotent in the sense that it may be called
// repeatedly; each call prints the running totals. Called at gc_end and from the crash
// path so a SIGSEGV or ABRT cannot erase what was already observed.
void Report(const char* point);

} // namespace LoadGoodProbe
} // namespace MapleRuntime

#endif
