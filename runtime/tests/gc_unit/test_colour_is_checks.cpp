// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Port of OpenJDK's ZAddressTest::is_checks
// (test/hotspot/gtest/gc/z/test_zAddress.cpp:177-435).
//
// The shape is the point.  ZGC does not check its colour predicates one interesting case at a time;
// it enumerates the whole cross-product of colours, computes what each predicate *should* say from
// the epoch state alone, and compares.  Then it advances the phases so the good masks move, and runs
// the entire cross-product again.  A predicate that is right for the colours in front of it today
// and wrong one flip later has nowhere to hide.
//
// Two rules kept from the original, both of which are why it finds things:
//
//   1. The expectation is derived from the epoch words, never from the masks and never by calling
//      the function under test.  ZGC writes `EXPECT_EQ(ZPointer::is_load_good(ptr),
//      same_old_remapping && same_young_remapping)` -- the right-hand side is assembled from the
//      pointer's own colour bits and the global epoch, so the two sides are independent
//      derivations of the same claim.  Rebuilding the mask arithmetic as the oracle would only
//      assert that a formula equals itself.
//
//   2. Every predicate is checked on every colour, including the ones where it should say no.
//      A predicate that returns false for everything passes any test suite that only feeds it
//      values it should accept.
//
// Our colour space is bit-identical to ZGC's (ColourMask.h, zAddress.hpp:60-176):
//   Remapped 48-51 (four one-hot) | MarkedYoung 52-53 | MarkedOld 54-55 | Remembered 56-57
// and the remap pair maps the same way: RemappedYoungMask and RemappedOldMask each hold two of the
// four one-hot values, and the colour currently handed out is their intersection.

#include <cstdint>

#include "Common/ColourMask.h"
#include "Common/ColourPredicates.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

// zAddress.hpp calls these RemappedYoung0/1 and RemappedOld0/1; a concrete colour is the pair.
// Absent is ZGC's ZColor::Uncolored, and it is not decoration: every predicate is a conjunction of
// "carries the current bit of this family", and a family whose bit is missing entirely is caught by
// that clause and by nothing else.  The bad-mask test cannot see it -- a missing bit is not a wrong
// bit.  Enumerating only the two epochs of each family therefore leaves every one of those clauses
// unprotected, which is how the first version of this file passed while is_mark_good had its
// marked-old clause deleted.
enum class YoungRemap { Absent, Y0, Y1 };
enum class OldRemap { Absent, O0, O1 };
enum class YoungMark { Absent, M0, M1 };
enum class OldMark { Absent, M0, M1 };
enum class Remember { Absent, R0, R1 };

// The epoch words WCollector holds, mirrored so the test can flip them the way the product does
// (WCollector.h flip_young_relocate_start / flip_old_relocate_start / flip_young_mark_start /
// flip_old_mark_start, each of which is one xor followed by set_good_masks).
struct Epoch {
    EpochColours e{ ZPointerRemapped10 | ZPointerRemapped00, ZPointerRemapped01 | ZPointerRemapped00,
                    MARKED_YOUNG_0, MARKED_OLD_0, REMEMBERED_0 };

    void FlipYoungRelocateStart() { e.remappedYoungMask ^= REMAP_COLOUR_MASK; }
    void FlipOldRelocateStart() { e.remappedOldMask ^= REMAP_COLOUR_MASK; }
    void FlipYoungMarkStart()
    {
        e.markedYoung ^= MARKED_YOUNG_MASK;
        e.remembered ^= REMEMBERED_MASK;
    }
    void FlipOldMarkStart() { e.markedOld ^= MARKED_OLD_MASK; }

    BadMasks Masks() const { return ComputeBadMasks(e); }
};

// (young, old) -> the single one-hot remap value that pair names.  Derived from the masks the
// product starts with: YoungMask holds {00,10} and OldMask holds {00,01} before any flip, and a
// flip xors in the complement, so Y0={00,10} Y1={01,11} O0={00,01} O1={10,11}.  The intersection
// of one young choice and one old choice is always exactly one bit -- that is the whole reason the
// space has four values and not two.
uintptr_t RemapBitFor(YoungRemap y, OldRemap o)
{
    if (y == YoungRemap::Absent || o == OldRemap::Absent) {
        return 0;
    }
    if (y == YoungRemap::Y0) {
        return o == OldRemap::O0 ? ZPointerRemapped00 : ZPointerRemapped10;
    }
    return o == OldRemap::O0 ? ZPointerRemapped01 : ZPointerRemapped11;
}

uintptr_t MarkYoungBitFor(YoungMark m)
{
    return m == YoungMark::Absent ? 0 : (m == YoungMark::M0 ? MARKED_YOUNG_0 : MARKED_YOUNG_1);
}
uintptr_t MarkOldBitFor(OldMark m)
{
    return m == OldMark::Absent ? 0 : (m == OldMark::M0 ? MARKED_OLD_0 : MARKED_OLD_1);
}
uintptr_t RememberBitFor(Remember r)
{
    return r == Remember::Absent ? 0 : (r == Remember::R0 ? REMEMBERED_0 : REMEMBERED_1);
}

// zAddress test's valid_value: any non-zero address payload. LogMinObjectAlignment there; here the
// only requirement has_address makes is that the low 48 bits are not all zero.
constexpr uintptr_t kValidAddress = uintptr_t(1) << 3;
constexpr uintptr_t kNullAddress = 0;

struct Colour {
    YoungRemap y;
    OldRemap o;
    YoungMark my;
    OldMark mo;
    Remember rm;
};

uintptr_t Paint(uintptr_t address, Colour c)
{
    return address | RemapBitFor(c.y, c.o) | MarkYoungBitFor(c.my) | MarkOldBitFor(c.mo) |
        RememberBitFor(c.rm);
}

// The oracle. Reads only the pointer's colour bits and the epoch words -- no mask arithmetic, no
// call into the predicates.
void CheckOneColour(const Epoch& epoch, uintptr_t address, Colour c)
{
    const BadMasks m = epoch.Masks();
    const uintptr_t value = Paint(address, c);

    const uintptr_t remapBit = RemapBitFor(c.y, c.o);
    const bool sameYoungRemap = (remapBit & epoch.e.remappedYoungMask) != 0;
    const bool sameOldRemap = (remapBit & epoch.e.remappedOldMask) != 0;
    const bool sameYoungMark = MarkYoungBitFor(c.my) == epoch.e.markedYoung;
    const bool sameOldMark = MarkOldBitFor(c.mo) == epoch.e.markedOld;
    const bool sameRemembered = RememberBitFor(c.rm) == epoch.e.remembered;
    const bool hasAddress = address != 0;

    const bool expectLoadGood = hasAddress && sameYoungRemap && sameOldRemap;
    const bool expectMarkGood = expectLoadGood && sameYoungMark && sameOldMark;
    const bool expectStoreGood = expectMarkGood && sameRemembered;

    GC_EXPECT_EQ(ColourPredicates::is_load_good(value, m.loadBad), expectLoadGood);
    GC_EXPECT_EQ(ColourPredicates::is_mark_good(value, m.loadBad, m.markBad), expectMarkGood);
    GC_EXPECT_EQ(ColourPredicates::is_store_good(value, m.loadBad, m.storeBad), expectStoreGood);

    // is_load_bad is a mask test, not the negation of is_load_good: a value carrying a remap colour
    // that is not the current one is bad, and the two agree here only because every value in this
    // cross-product carries exactly one remap bit.  The uncoloured case, where they disagree, is
    // pinned separately below.
    // Bad is "carries a colour of this family that is not the current one" -- a value carrying no
    // colour at all is not bad, it is unexamined.  That asymmetry is the whole point of the
    // Absent arm.
    const bool expectLoadBad = remapBit != 0 && !(sameYoungRemap && sameOldRemap);
    GC_EXPECT_EQ(ColourPredicates::is_load_bad(value, m.loadBad), expectLoadBad);

    // *_or_null differ from the plain form only at raw zero, and a coloured null is not raw zero.
    GC_EXPECT_EQ(ColourPredicates::is_load_good_or_null(value, m.loadBad), value == 0 || expectLoadGood);
    GC_EXPECT_EQ(ColourPredicates::is_mark_good_or_null(value, m.loadBad, m.markBad),
                 value == 0 || expectMarkGood);
    GC_EXPECT_EQ(ColourPredicates::is_store_good_or_null(value, m.loadBad, m.storeBad),
                 value == 0 || expectStoreGood);
}

void CheckAllColours(const Epoch& epoch)
{
    const YoungRemap ys[] = { YoungRemap::Absent, YoungRemap::Y0, YoungRemap::Y1 };
    const OldRemap os[] = { OldRemap::Absent, OldRemap::O0, OldRemap::O1 };
    const YoungMark mys[] = { YoungMark::Absent, YoungMark::M0, YoungMark::M1 };
    const OldMark mos[] = { OldMark::Absent, OldMark::M0, OldMark::M1 };
    const Remember rms[] = { Remember::Absent, Remember::R0, Remember::R1 };

    for (YoungRemap y : ys) {
        for (OldRemap o : os) {
            for (YoungMark my : mys) {
                for (OldMark mo : mos) {
                    for (Remember rm : rms) {
                        const Colour c{ y, o, my, mo, rm };
                        CheckOneColour(epoch, kValidAddress, c);
                        CheckOneColour(epoch, kNullAddress, c);
                    }
                }
            }
        }
    }
}

} // namespace

// 3 remap-young x 3 remap-old x 3 mark-young x 3 mark-old x 3 remembered x 2 addresses = 486 values
// (three = absent plus the two epochs), each checked against 7 predicates, at the starting epoch.
GC_TEST(ColourIsChecks, EveryColourAtTheStartingEpoch)
{
    Epoch epoch;
    CheckAllColours(epoch);
}

// The same cross-product after every flip, in the order the collector performs them.  ZGC's version
// walks a long irregular schedule of young flips between old flips (test_zAddress.cpp:374-431); the
// reason is that young and old advance independently, so a bug that only appears when one generation
// is several flips ahead of the other is invisible to a schedule that keeps them in step.
GC_TEST(ColourIsChecks, EveryColourAcrossAnIrregularFlipSchedule)
{
    Epoch epoch;
    int youngPhase = 0;
    int oldPhase = 0;

    auto advanceYoung = [&](int amount) {
        for (int i = 0; i < amount; ++i) {
            if (++youngPhase & 1) {
                epoch.FlipYoungMarkStart();
            } else {
                epoch.FlipYoungRelocateStart();
            }
            CheckAllColours(epoch);
        }
    };
    auto advanceOld = [&](int amount) {
        for (int i = 0; i < amount; ++i) {
            if (++oldPhase & 1) {
                epoch.FlipOldMarkStart();
            } else {
                epoch.FlipOldRelocateStart();
            }
            CheckAllColours(epoch);
        }
    };

    CheckAllColours(epoch);
    advanceOld(4);
    advanceYoung(4);
    for (int round = 0; round < 4; ++round) {
        advanceOld(1);
        advanceYoung(4);
    }
    for (int round = 0; round < 4; ++round) {
        advanceOld(1);
        advanceYoung(3);
    }
    for (int round = 0; round < 4; ++round) {
        advanceOld(1);
        advanceYoung(2);
    }
    for (int round = 0; round < 4; ++round) {
        advanceOld(1);
        advanceYoung(1);
    }
}

// The blind spot the collector's own header warns about (WCollector.h:128-136): a value carrying no
// remap colour at all satisfies neither is_load_good nor is_load_bad, so an if/else-if chain over
// the two lets it through unexamined.  That is correct while the good colour is zero and wrong the
// moment it is not, which is the state we are in.  Pinning it so the asymmetry is a stated property
// rather than something a reader has to rediscover from a crash.
GC_TEST(ColourIsChecks, UncolouredValueIsNeitherLoadGoodNorLoadBad)
{
    Epoch epoch;
    const BadMasks m = epoch.Masks();
    const uintptr_t plain = kValidAddress; // address, no colour bits at all

    GC_EXPECT_TRUE(!ColourPredicates::is_load_good(plain, m.loadBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_load_bad(plain, m.loadBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_mark_good(plain, m.loadBad, m.markBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_store_good(plain, m.loadBad, m.storeBad));
}

// Raw zero is the one value the _or_null forms exist for.
GC_TEST(ColourIsChecks, RawNullIsGoodOrNullAndNeverBad)
{
    Epoch epoch;
    const BadMasks m = epoch.Masks();

    GC_EXPECT_TRUE(!ColourPredicates::is_load_good(0, m.loadBad));
    GC_EXPECT_TRUE(ColourPredicates::is_load_good_or_null(0, m.loadBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_load_bad(0, m.loadBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_mark_good(0, m.loadBad, m.markBad));
    GC_EXPECT_TRUE(ColourPredicates::is_mark_good_or_null(0, m.loadBad, m.markBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_mark_bad(0, m.markBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_store_good(0, m.loadBad, m.storeBad));
    GC_EXPECT_TRUE(ColourPredicates::is_store_good_or_null(0, m.loadBad, m.storeBad));
    GC_EXPECT_TRUE(!ColourPredicates::is_store_bad(0, m.storeBad));
}

// The four remap values are a bijection with the (young epoch, old epoch) pair, and the colour
// currently handed out is the intersection of the two masks.  If that ever stopped holding, a stale
// colour could sit in the accepted set and every load-good test above would agree with a broken
// oracle, so it is checked directly rather than inferred.
GC_TEST(ColourIsChecks, RemapPairIsABijectionWithTheFourColours)
{
    const YoungRemap ys[] = { YoungRemap::Y0, YoungRemap::Y1 };
    const OldRemap os[] = { OldRemap::O0, OldRemap::O1 };
    uintptr_t seen = 0;
    unsigned count = 0;
    for (YoungRemap y : ys) {
        for (OldRemap o : os) {
            const uintptr_t bit = RemapBitFor(y, o);
            GC_EXPECT_EQ(bit & REMAP_COLOUR_MASK, bit); // inside the remap field
            GC_EXPECT_EQ(bit & (bit - 1), uintptr_t(0)); // exactly one bit
            GC_EXPECT_EQ(seen & bit, uintptr_t(0));      // not already used by another pair
            seen |= bit;
            ++count;
        }
    }
    GC_EXPECT_EQ(count, 4u);
    GC_EXPECT_EQ(seen, REMAP_COLOUR_MASK);

    // And the product's own arithmetic agrees: intersection of the two masks is the handed-out
    // colour, at the starting epoch and after each kind of flip.
    Epoch epoch;
    GC_EXPECT_EQ(epoch.Masks().remapColour, RemapBitFor(YoungRemap::Y0, OldRemap::O0));
    epoch.FlipYoungRelocateStart();
    GC_EXPECT_EQ(epoch.Masks().remapColour, RemapBitFor(YoungRemap::Y1, OldRemap::O0));
    epoch.FlipOldRelocateStart();
    GC_EXPECT_EQ(epoch.Masks().remapColour, RemapBitFor(YoungRemap::Y1, OldRemap::O1));
    epoch.FlipYoungRelocateStart();
    GC_EXPECT_EQ(epoch.Masks().remapColour, RemapBitFor(YoungRemap::Y0, OldRemap::O1));
}
