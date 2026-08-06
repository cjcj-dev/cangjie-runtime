// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_COLOUR_MASK_H
#define MRT_COLOUR_MASK_H

// The bit layout of a reference, and the mask the read barrier tests against.
//
// This header includes nothing from the project on purpose. It sits at the bottom of the include
// graph, below RefField.h and TypeDef.h, both of which need the layout; reaching upward for so
// much as a typedef would put it in a cycle with them, which is how the first two attempts at
// declaring the mask failed. <cstdint> is the only dependency.

#include <cstdint>

// Tag-ID generation count for WCollector phase tags (RefField tagID field).
// Default 2 preserves upstream N=2 behaviour; rebuild with -DMRT_TAG_ID_COUNT=N to widen.
#ifndef MRT_TAG_ID_COUNT
#define MRT_TAG_ID_COUNT 2
#endif

extern "C" {
// Any bit set means the reference needs the barrier before use: it is mid-evacuation, or it
// carries a colour other than the one being handed out now. The collector owns the value and
// swaps it at a phase boundary; see WCollector::FlipRemapColour. The compiler emits a reference
// to this symbol by name (CJBarrierLowering.cpp:641), so it is extern "C": a mangled name would
// drift between compiler versions.
extern unsigned long g_cjLoadBadMask;
}

namespace MapleRuntime {
constexpr uint16_t TAG_ID_COUNT = static_cast<uint16_t>(MRT_TAG_ID_COUNT);
// Bits needed for values in [0, TAG_ID_COUNT). Taken from RefField padding on 64-bit.
constexpr unsigned TAG_ID_BITS =
    (TAG_ID_COUNT <= 2) ? 1u : (TAG_ID_COUNT <= 4) ? 2u : (TAG_ID_COUNT <= 8) ? 3u : 4u;

// A reference always carries a colour, so that "this value may be stale" is something the value
// itself says rather than something the reader has to already know. The colours are one-hot
// because the compiler's fast path is a single AND against a mask: with one-hot colours "is this
// the current colour" becomes "are any of the other colours' bits set", which an AND can answer.
// ZGC encodes Remapped the same way and for the same reason (jdk zAddress.hpp:169-170,
// zAddress.cpp:120-121).
//
// Flipping a phase is then one store to g_cjLoadBadMask, where today it is a full-heap
// stop-the-world walk that strips the old tag off every reference (InvalidateOldTaggedRefs).
constexpr unsigned REMAP_COLOUR_BITS = 2u;
// address:48 + isTagged:1 + tagID:TAG_ID_BITS + remapColour:2 + padding == 64
constexpr unsigned TAG_ID_PADDING_BITS = 15u - TAG_ID_BITS - REMAP_COLOUR_BITS;
constexpr unsigned REMAP_COLOUR_SHIFT = 48u + 1u + TAG_ID_BITS;
constexpr uintptr_t REMAP_COLOUR_A = uintptr_t(1) << REMAP_COLOUR_SHIFT;
constexpr uintptr_t REMAP_COLOUR_B = uintptr_t(1) << (REMAP_COLOUR_SHIFT + 1u);
constexpr uintptr_t REMAP_COLOUR_MASK = REMAP_COLOUR_A | REMAP_COLOUR_B;
// Tagged (mid-evacuation) needs the barrier whichever colour it carries.
constexpr uintptr_t TAGGED_BITS_MASK = ((uintptr_t(1) << (1u + TAG_ID_BITS)) - 1u) << 48u;
} // namespace MapleRuntime

#endif // MRT_COLOUR_MASK_H
