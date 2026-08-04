// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_TAG_REUSE_PROBE_H
#define MRT_HEAP_TAG_REUSE_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Read-only probe: before ReleaseMemory(previous tag), scan regions whose liveInfo /
// liveInfo0 / retainedLiveInfo still point into the range about to be madvise'd.
// Gate (default off): MRT_GCV2_TAG_REUSE=1
// Optional MarkBits sticky check: MRT_GCV2_MARK_BITS_STICKY=1
class TagReuseProbe {
public:
    static bool TagReuseEnabled();
    static bool MarkBitsStickyEnabled();

    // Called immediately before ForwardDataSpace::ReleaseMemory on previous tag.
    static void ScanBeforeRelease(uintptr_t rangeStart, size_t rangeSize, uint16_t previousTagId,
                                  uintptr_t liveInfoZoneStart, uintptr_t liveInfoZonePos,
                                  uintptr_t bitmapZoneStart, uintptr_t bitmapZonePos);

    // After MarkBits, re-read IsMarkedObject; count sticky failures.
    // Returns true if the bit stuck (or probe off).
    static bool NoteMarkBitsSticky(class RegionInfo* region, size_t offset, bool markBitsReturnedTrue,
                                   const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_TAG_REUSE_PROBE_H
