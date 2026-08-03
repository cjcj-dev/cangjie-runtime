// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GCINITWIN_PROBE_H
#define MRT_GCINITWIN_PROBE_H

#include <cstdint>
#include <cstddef>

namespace MapleRuntime {
namespace GcInitWin {

struct MinorTargetFate {
    const void* target;
    const void* region;
    uintptr_t regionStart;
    uintptr_t regionAlloc;
    uint8_t regionType;
    uint8_t heap;
    uint8_t validRegion;
    uint8_t young;
    uint8_t marked;
    uint8_t freeRegion;
    uint8_t garbageRegion;
    uint8_t inAllocRange;
    uint8_t state;
};

// Gated by MRT_GCINITWIN=1 (also auto-on when MRT_GCDISPEL=1 for this lane).
bool ProbeOn();

// Called around CJFileLoader::DoInitImage package GlobalInitFunc execution.
void NoteGlobalInitBegin(const char* fileBaseName);
void NoteGlobalInitEnd(const char* fileBaseName, bool ok);

// Observe every static-ref store (WriteStaticRef / MCC_WriteStaticRef / MCC_WriteRefField static path).
void NoteStaticRefWrite(const void* field, const void* ref, const char* site);

// Observe static-struct stores that may overwrite ref fields (primitiveTys Array rawptr).
void NoteStaticStructWrite(const void* dst, size_t dstLen, const void* src, const char* site);

// Bind static-root snapshots and target fate to one minor collection round.
void NoteMinorCycleStart(uint64_t round);
const void* InitialMinorTarget(const void* slot, uint64_t round);
void NoteMinorSlotSnapshot(const void* slot, uint64_t round, const char* moment,
                           const MinorTargetFate& currentFate, const MinorTargetFate& initialFate);

// At static-root enqueue / bad-TI hit: report lifecycle phase of watched slots.
void NoteStaticEnqueueLifecycle(const void* slot, const void* target, uint8_t tiClass, const char* point,
                                const char* kind, const MinorTargetFate& currentFate,
                                const MinorTargetFate& initialFate);

// Resolve symbol names for watched slots once maps are stable (first GC cycle ok).
void TryResolveWatchSlots();

void DumpSummary(const char* reason);

// Phase counter visible to other instrumentation.
uint32_t GlobalInitDepth();
uint64_t GlobalInitCompletedCount();
bool AnyGlobalInitActive();

} // namespace GcInitWin
} // namespace MapleRuntime

#endif
