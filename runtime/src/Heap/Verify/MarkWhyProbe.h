// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_MARK_WHY_PROBE_H
#define MRT_HEAP_MARK_WHY_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class RegionInfo;
struct RegionBitmap;
class BaseObject;

// Read-only probe: after MarkBits, before CHECK(IsMarkedObject), dump identity of
// write/read bitmap, offsets, region metadata, GC phase, thread role.
// Gate (default off): MRT_GCV2_MARK_WHY=1
// Optional concurrent alloc counter: MRT_GCV2_MARK_WHY_ALLOC=1
// Sample every N successful marks for positive control: MRT_GCV2_MARK_WHY_SAMPLE=<N> (default 65536)
class MarkWhyProbe {
public:
    static bool Enabled();
    static bool AllocTrackEnabled();

    // Count real mark-bitmap allocations (CAS winner path) per region identity.
    static void NoteMarkBitmapAlloc(RegionInfo* region, RegionBitmap* allocated);

    // After MarkBits: compare writeBm vs GetMarkBitmap(), offsets, capacity, phase.
    // Returns false when the probe is off -- both call sites discard the result, so the disabled
    // build must not pay for a bitmap read. See the .cpp for why.
    static bool NoteAfterMarkBits(RegionInfo* region, const BaseObject* obj, size_t offsetWrite, size_t objSize,
                                  size_t regionSizeArg, RegionBitmap* writeBm, bool markBitsReturnedAlreadyMarked,
                                  const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_MARK_WHY_PROBE_H
