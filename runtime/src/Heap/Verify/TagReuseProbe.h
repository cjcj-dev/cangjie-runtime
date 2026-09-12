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

enum class Generation : uint8_t;

// Read-only probe: before ReleaseMemory(previous tag), scan regions whose liveInfo /
// liveInfo0 / retainedLiveInfo still point into the range about to be madvise'd.
// Gate (default off): MRT_GCV2_TAG_REUSE=1
class TagReuseProbe {
public:
    static bool TagReuseEnabled();

    // Called immediately before ForwardDataSpace::ReleaseMemory on previous tag.
    static void ScanBeforeRelease(uintptr_t rangeStart, size_t rangeSize, uint16_t previousTagId,
                                  uintptr_t liveInfoZoneStart, uintptr_t liveInfoZonePos,
                                  uintptr_t bitmapZoneStart, uintptr_t bitmapZonePos);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_TAG_REUSE_PROBE_H
