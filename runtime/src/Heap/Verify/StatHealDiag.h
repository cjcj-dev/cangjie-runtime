// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_STAT_HEAL_DIAG_H
#define MRT_STAT_HEAL_DIAG_H

#include <cstdint>

namespace MapleRuntime {

class RootSlot;

namespace StatHealDiag {

// Observation-only probe for static-root healing. Default off; enable with
// MRT_GCV2_STATHEAL=1 (or diagnostic token "statheal").
bool Enabled();

// Measurement-only A/B switch. It is consulted only while the statheal probe
// is enabled, so the default product path always heals.
bool SuppressHealForAB();

// One ReadStaticRef whose colour was bad and/or whose target entered routing.
// resolvedChanged distinguishes a real old->latest transition from a ghost
// lookup that still returns itself. Repeated real transitions for one slot in
// one completed-GC epoch are the direct cost of not writing the latest value.
void NoteStaticRead(const RootSlot& slot, uintptr_t observed, bool badColour, bool routed,
                    bool resolvedChanged, bool healAttempted, bool healSucceeded);

// StaticRootTable calls these around one exact, de-duplicated registry walk.
// The scan reconciles every registered RootSlot address with /proc/self/maps.
void BeginStaticRootScan();
void NoteStaticRootSlot(const RootSlot& slot);
void EndStaticRootScan();

void Report(const char* point);

} // namespace StatHealDiag
} // namespace MapleRuntime

#endif
