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
// swaps it at a phase boundary; see WCollector::set_good_masks. The compiler emits a reference
// to this symbol by name (CJBarrierLowering.cpp:641), so it is extern "C": a mangled name would
// drift between compiler versions.
extern unsigned long g_cjLoadBadMask;

// ⭐ 构建溯源符号的**声明**；⭐ 定义在 `ColourMask.cpp`（⛔ 头里放定义会多重定义）
extern "C" const char g_cjRuntimeProvenance[];

// Mark barriers use the same dynamic-mask ABI as load barriers. The mark mask additionally rejects
// references carrying the previous young or old mark epoch (OpenJDK zAddress.hpp:209-217).
extern unsigned long g_cjMarkBadMask;

// Store barriers reject references that are mark-bad or missing the current Remembered epoch bit
// (OpenJDK zAddress.hpp:216-217, zAddress.cpp:83-87). Load/mark masks do not include Remembered.
extern unsigned long g_cjStoreBadMask;
}

namespace MapleRuntime {
// OpenJDK zGenerationId.hpp:29-32.
enum class ZGenerationId : uint8_t {
    young,
    old,
};

constexpr uint16_t TAG_ID_COUNT = static_cast<uint16_t>(MRT_TAG_ID_COUNT);
// Bits needed for values in [0, TAG_ID_COUNT). Taken from RefField padding on 64-bit.
constexpr unsigned TAG_ID_BITS =
    (TAG_ID_COUNT <= 2) ? 1u : (TAG_ID_COUNT <= 4) ? 2u : (TAG_ID_COUNT <= 8) ? 3u : 4u;

// A reference always carries a colour, so that "this value may be stale" is something the value
// itself says rather than something the reader has to already know. ZGC uses one physical bit for
// each RemappedYoung x RemappedOld state. A two-bit binary encoding cannot preserve the compiler's
// single-AND fast path: when 11 is current, a stale 10 or 01 differs by a missing bit, which AND
// cannot observe (OpenJDK zAddress.hpp:95-128,168-176).
//
// A generation relocate-start flip changes the accepted one-hot subset and republishes
// g_cjLoadBadMask; see WCollector::flip_young_relocate_start/flip_old_relocate_start.
constexpr unsigned REMAP_COLOUR_BITS = 4u;
// MarkedYoung[0,1] and MarkedOld[0,1] are independent one-hot epochs. Each family needs two
// physical bits so that a mark-start flip makes the previous epoch bad without a zero-bit trust
// state (OpenJDK zAddress.hpp:156-166, zAddress.cpp:120-146).
constexpr unsigned MARKED_YOUNG_BITS = 2u;
constexpr unsigned MARKED_OLD_BITS = 2u;
// address:48 + isTagged:1 + tagID:1 + remapColour:4 + markedYoung:2 + markedOld:2
// + remembered:2 + spare padding == 64 (spare = TAG_ID_PADDING_BITS - REMEMBERED_BITS)
constexpr unsigned TAG_ID_PADDING_BITS =
    15u - TAG_ID_BITS - REMAP_COLOUR_BITS - MARKED_YOUNG_BITS - MARKED_OLD_BITS;
constexpr unsigned REMAP_COLOUR_SHIFT = 48u + 1u + TAG_ID_BITS;
constexpr uintptr_t ZPointerRemapped00 = uintptr_t(1) << REMAP_COLOUR_SHIFT;
constexpr uintptr_t ZPointerRemapped01 = uintptr_t(1) << (REMAP_COLOUR_SHIFT + 1u);
constexpr uintptr_t ZPointerRemapped10 = uintptr_t(1) << (REMAP_COLOUR_SHIFT + 2u);
constexpr uintptr_t ZPointerRemapped11 = uintptr_t(1) << (REMAP_COLOUR_SHIFT + 3u);
constexpr uintptr_t REMAP_COLOUR_MASK =
    ZPointerRemapped00 | ZPointerRemapped01 | ZPointerRemapped10 | ZPointerRemapped11;
constexpr unsigned MARKED_YOUNG_SHIFT = REMAP_COLOUR_SHIFT + REMAP_COLOUR_BITS;
constexpr uintptr_t MARKED_YOUNG_0 = uintptr_t(1) << MARKED_YOUNG_SHIFT;
constexpr uintptr_t MARKED_YOUNG_1 = uintptr_t(1) << (MARKED_YOUNG_SHIFT + 1u);
constexpr uintptr_t MARKED_YOUNG_MASK = MARKED_YOUNG_0 | MARKED_YOUNG_1;
constexpr unsigned MARKED_OLD_SHIFT = MARKED_YOUNG_SHIFT + MARKED_YOUNG_BITS;
constexpr uintptr_t MARKED_OLD_0 = uintptr_t(1) << MARKED_OLD_SHIFT;
constexpr uintptr_t MARKED_OLD_1 = uintptr_t(1) << (MARKED_OLD_SHIFT + 1u);
constexpr uintptr_t MARKED_OLD_MASK = MARKED_OLD_0 | MARKED_OLD_1;
// Remembered[0,1] one-hot epoch (OpenJDK zAddress.hpp:148-154). Lives in former padding at
// bits 58-59 when TAG_ID_BITS=1 (ops/design/REMEMBERED_BIT_DESIGN.md).
constexpr unsigned REMEMBERED_BITS = 2u;
constexpr unsigned REMEMBERED_SHIFT = MARKED_OLD_SHIFT + MARKED_OLD_BITS;
constexpr uintptr_t REMEMBERED_0 = uintptr_t(1) << REMEMBERED_SHIFT;
constexpr uintptr_t REMEMBERED_1 = uintptr_t(1) << (REMEMBERED_SHIFT + 1u);
constexpr uintptr_t REMEMBERED_MASK = REMEMBERED_0 | REMEMBERED_1;
static_assert(REMEMBERED_BITS <= TAG_ID_PADDING_BITS,
              "Remembered family needs free RefField padding bits");

// Finalizable[0,1] one-hot epoch, flipped together with old mark start
// (OpenJDK zAddress.hpp:161-162, zAddress.cpp:143-147).
//
// ZGC colours a finalizable-marked reference LoadGood | MarkedYoung | Finalizable | Remembered
// (zAddress.inline.hpp:764-769) -- note: no MarkedOld bit. The Finalizable bits sit inside
// ZPointerMarkedMask (zAddress.hpp:157-165), which is part of ZPointerMarkMetadataMask
// (zAddress.hpp:192) but NOT of ZPointerMarkGoodMask (zAddress.cpp:81-83). So such a reference
// is permanently mark-bad and a strong mark/keep-alive barrier is forced down the slow path,
// where the object is upgraded to strongly reachable (zBarrier.inline.hpp:610-620).
//
// We do not carry that state in the pointer. Our equivalent lives in a side table:
// LiveInfo.h:204 resurrectBitmap, folded into liveness by LiveInfo.h:210 / Heap.cpp:76, and
// filled by TracingCollector.cpp:696-697 DoResurrection -- which runs inside the concurrent
// marking segment (TracingCollector.cpp:680-698), so "unreachable by schedule" is not available
// as an argument. The bits are reserved here, and only reserved: kFinalizableWired says so, no
// live mask includes them, and nothing publishes them. What this buys is that the padding budget
// is now checked by the compiler instead of by a comment, and that the state machine table
// (runtime/tests/colour_state_machine_probe.cpp) can name the cell we are missing.
constexpr unsigned FINALIZABLE_BITS = 2u;
constexpr unsigned FINALIZABLE_SHIFT = REMEMBERED_SHIFT + REMEMBERED_BITS;
constexpr uintptr_t FINALIZABLE_0 = uintptr_t(1) << FINALIZABLE_SHIFT;
constexpr uintptr_t FINALIZABLE_1 = uintptr_t(1) << (FINALIZABLE_SHIFT + 1u);
constexpr uintptr_t FINALIZABLE_MASK = FINALIZABLE_0 | FINALIZABLE_1;
// False until the family is actually published in a live mask (that is C4 knife 6, and it is a
// real behaviour change plus a two-half pin bump: g_cjMarkBadMask is an ABI atom shared with the
// compiler). Reading this constant is how code asks "is the fifth family real yet?".
constexpr bool kFinalizableWired = false;
// Fail closed rather than silently overlapping tagID: at MRT_TAG_ID_COUNT=16 (TAG_ID_BITS=4)
// the padding budget is 3 bits and Remembered+Finalizable want 4. That is a real mutual
// exclusion between "widen tagID" and "wire the fifth family"; it must be a build error, not a
// wrong bit layout discovered at run time.
static_assert(REMEMBERED_BITS + FINALIZABLE_BITS <= TAG_ID_PADDING_BITS,
              "Remembered+Finalizable exceed the RefField padding budget: widening tagID and "
              "wiring the Finalizable family are mutually exclusive at this width");
static_assert(FINALIZABLE_SHIFT + FINALIZABLE_BITS <= 64u, "Finalizable family runs off the word");

// Store metadata = remap + MY + MO + Remembered (OpenJDK zAddress.hpp:194). Finalizable is
// deliberately absent: see kFinalizableWired above.
constexpr uintptr_t STORE_METADATA_MASK =
    REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK | REMEMBERED_MASK;
// Tagged (mid-evacuation) needs the barrier whichever colour it carries.
constexpr uintptr_t TAGGED_BITS_MASK = ((uintptr_t(1) << (1u + TAG_ID_BITS)) - 1u) << 48u;

// The epoch state the collector hands out, and the three bad masks derived from it.
//
// These two POD structs and ComputeBadMasks are the whole of C4 knife 1 on the product side:
// the body of WCollector::set_good_masks, lifted verbatim so that it has exactly one writer.
// Before this there were two -- WCollector.h:131-140 and the three literal initialisers in
// BaseObject.cpp:240-258, the latter carrying a comment saying it was written to "match live
// set_good_masks shape". A second copy of a formula that must agree bit for bit is the defect;
// which of the two is wrong is a detail.
//
// Nothing here includes anything: this header sits at the bottom of the include graph (see the
// note at the top), so these must stay plain uintptr_t PODs.
struct EpochColours {
    uintptr_t remappedYoungMask;
    uintptr_t remappedOldMask;
    uintptr_t markedYoung;
    uintptr_t markedOld;
    uintptr_t remembered;
};

struct BadMasks {
    // The remap colour currently handed out; WCollector::currentRemapColour is a member, not a
    // global, and WCollector.h:428-429 storeColour reads it -- so a pure function has to return
    // it alongside the three published masks.
    uintptr_t remapColour;
    uintptr_t loadBad;
    uintptr_t markBad;
    uintptr_t storeBad;
};

// Token-for-token transcription of WCollector::set_good_masks (WCollector.h:132-139 @ 6adf9dd0),
// which itself mirrors ZGlobalsPointers::set_good_masks (OpenJDK zAddress.cpp:78-94).
//
// ⛔ Do NOT "clean this up" into the ZGC-looking Good ^ Metadata form. STORE_METADATA_MASK does
// not contain TAGGED_BITS_MASK, so loadBad = LoadGood ^ LoadMetadata silently drops the tagged
// term and every mid-evacuation reference becomes load-good. That rewrite is the single most
// plausible-looking behaviour change on this whole surface, which is why MRT_GCV2_MASKEQUIV
// exists and why the 32-cell static_assert in the probe compares against a literal copy.
constexpr BadMasks ComputeBadMasks(EpochColours e)
{
    // :133  currentRemapColour = ZPointerRemappedYoungMask & ZPointerRemappedOldMask;
    const uintptr_t remapColour = e.remappedYoungMask & e.remappedOldMask;
    // :134  g_cjLoadBadMask = TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ currentRemapColour);
    const uintptr_t loadBad = TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ remapColour);
    // :135  g_cjMarkBadMask = loadBad | (MARKED_YOUNG_MASK & ~currentMarkedYoung)
    //                                 | (MARKED_OLD_MASK & ~currentMarkedOld);
    const uintptr_t markBad =
        loadBad | (MARKED_YOUNG_MASK & ~e.markedYoung) | (MARKED_OLD_MASK & ~e.markedOld);
    // :139  g_cjStoreBadMask = markBad | (REMEMBERED_MASK & ~currentRemembered);
    const uintptr_t storeBad = markBad | (REMEMBERED_MASK & ~e.remembered);
    return BadMasks{ remapColour, loadBad, markBad, storeBad };
}

// The epoch WCollector starts in: the member initialisers at WCollector.h:116-122.
constexpr EpochColours kInitialEpochColours = { ZPointerRemapped10 | ZPointerRemapped00,
                                                ZPointerRemapped01 | ZPointerRemapped00,
                                                MARKED_YOUNG_0,
                                                MARKED_OLD_0,
                                                REMEMBERED_0 };

// Self-heal CAS bound for load barriers (ATOMIC_READ_PROTOCOL Q2). ZGC terminates
// self-heal via colour monotonicity; our Forward-phase writers can re-tag the same
// slot, so an unbounded heal loop is a livelock. After K failures the reader returns
// the resolved payload without writing the slot (wait-free escape).
constexpr int kSelfHealAttempts = 2;
// Colour-aware identity CAS (CompareAndSwapReferenceImpl family). A concurrent reader
// may self-heal the slot on every load so the raw expected bits keep moving while the
// decoded identity stays oldRef; without a bound that is the 47-minute natural_wave spin
// fixed on main by c3179214. Exhaustion returns false (callers already handle CAS fail).
constexpr int kCasAttempts = 8;
} // namespace MapleRuntime

#endif // MRT_COLOUR_MASK_H
