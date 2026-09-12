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

class TagReuseProbe {
public:
    static bool MarkBitsStickyEnabled();
    static bool NoteMarkBitsSticky(class RegionInfo* region, size_t offset, bool markBitsReturnedTrue,
                                   const char* site);
    static bool NoteMarkBitsSticky(class RegionInfo* region, size_t offset, bool markBitsReturnedTrue,
                                   const char* site, Generation generation);
};

} // namespace MapleRuntime

#endif
