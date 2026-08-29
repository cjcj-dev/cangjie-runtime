#ifndef MRT_REMAP_YOUNG_ROOTS_H
#define MRT_REMAP_YOUNG_ROOTS_H

#include "Common/ColourMask.h"

#include <cstdint>

namespace MapleRuntime {
namespace RemapYoungRootsLogic {

// OpenJDK ZGenerationOld::remap_young_roots (zGeneration.cpp:1503-1523).
// Compile-time only — no MRT_GCV2_ env.
static constexpr bool kEnableRemapYoungRoots = true;

constexpr uintptr_t kYoungMask0 = ZPointerRemapped00 | ZPointerRemapped10;
constexpr uintptr_t kYoungMask1 = ZPointerRemapped01 | ZPointerRemapped11;
constexpr uintptr_t kOldMask0 = ZPointerRemapped00 | ZPointerRemapped01;
constexpr uintptr_t kOldMask1 = ZPointerRemapped10 | ZPointerRemapped11;

enum class Kind : uint8_t {
    Uncoloured = 0,
    LoadGood,
    YoungOnlyGood,
    OldOnlyGood,
    DoubleBad,
};

constexpr Kind Classify(uintptr_t value, uintptr_t youngMask, uintptr_t oldMask)
{
    if ((value & REMAP_COLOUR_MASK) == 0) {
        return Kind::Uncoloured;
    }
    const bool youngGood = (value & youngMask) != 0;
    const bool oldGood = (value & oldMask) != 0;
    if (youngGood && oldGood) {
        return Kind::LoadGood;
    }
    if (oldGood) {
        return Kind::OldOnlyGood;
    }
    if (youngGood) {
        return Kind::YoungOnlyGood;
    }
    return Kind::DoubleBad;
}

constexpr bool IsDoubleRemapBad(uintptr_t value, uintptr_t youngMask, uintptr_t oldMask)
{
    return Classify(value, youngMask, oldMask) == Kind::DoubleBad;
}

// ZBarrier::make_load_good returns directly for load-good values and enters
// relocate_or_remap only for load-bad values (zBarrier.inline.hpp:294-307).
// Remap coverage prevents a two-beat wrap; it must not be replaced by forcing
// a forwarding lookup for a value that already satisfies the fast path.
constexpr bool NeedsForwardingLookup(Kind kind)
{
    return kind == Kind::YoungOnlyGood || kind == Kind::OldOnlyGood || kind == Kind::DoubleBad;
}

// ZRemapYoungRootsTask reaches remembered fields by iterating live old pages;
// a bitmap record whose holder is not live is not a root and must not invoke a
// load barrier on the dead holder payload (zGeneration.cpp:1483-1523).
constexpr bool ShouldRemapRememberedSlot(bool holderIsLive) { return holderIsLive; }

constexpr uintptr_t CurrentRemapBit(uintptr_t youngMask, uintptr_t oldMask)
{
    return youngMask & oldMask;
}

constexpr uintptr_t FlipYoungMask(uintptr_t youngMask) { return youngMask ^ REMAP_COLOUR_MASK; }
constexpr uintptr_t FlipOldMask(uintptr_t oldMask) { return oldMask ^ REMAP_COLOUR_MASK; }

// Paint the current combined remap one-hot. Uncoloured words stay uncoloured
// (stack/runtime roots are RootSlots without colour).
constexpr uintptr_t RemapToCurrent(uintptr_t value, uintptr_t youngMask, uintptr_t oldMask, bool enable)
{
    if (!enable) {
        return value;
    }
    if ((value & REMAP_COLOUR_MASK) == 0) {
        return value;
    }
    return (value & ~REMAP_COLOUR_MASK) | CurrentRemapBit(youngMask, oldMask);
}

constexpr uintptr_t RemapToCurrent(uintptr_t value, uintptr_t youngMask, uintptr_t oldMask)
{
    return RemapToCurrent(value, youngMask, oldMask, kEnableRemapYoungRoots);
}

// After N young flips without a remap pass, a colour published at beat 0 is
// load-good again at beat 2 (xor wrap). Phase 8 remaps after each young-stale
// window so a root never carries two remap-bit errors.
constexpr bool ColourWrapsWithoutRemap(uintptr_t published, uintptr_t youngMask, uintptr_t oldMask)
{
    const uintptr_t y1 = FlipYoungMask(youngMask);
    const uintptr_t y2 = FlipYoungMask(y1);
    return Classify(published, y2, oldMask) == Kind::LoadGood &&
        Classify(published, y1, oldMask) != Kind::LoadGood;
}

constexpr bool RootsStayAtMostOneBeatStale(uintptr_t published, uintptr_t youngMask, uintptr_t oldMask,
                                           bool enable)
{
    uintptr_t slot = published;
    uintptr_t y = youngMask;
    const uintptr_t o = oldMask;
    slot = RemapToCurrent(slot, y, o, enable);
    y = FlipYoungMask(y);
    slot = RemapToCurrent(slot, y, o, enable);
    y = FlipYoungMask(y);
    return !IsDoubleRemapBad(slot, y, o) && Classify(slot, y, o) != Kind::LoadGood;
}

} // namespace RemapYoungRootsLogic
} // namespace MapleRuntime

#endif
