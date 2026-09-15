// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zAddress.hpp"
#include "Base/Macros.h"

// Phase B: the read-barrier mask the compiler tests against (see TypeDef.h for why).
// "All colour bits set" keeps the predicate identical to the shift form it replaces.
// Phase C: bad = mid-evacuation, or carrying a colour other than the one being handed out.
// The initial conceptual state is RemappedYoung0 x RemappedOld0, encoded by Remapped00.
// c4unify: these three used to be hand-written literal expressions -- a second copy of
// WCollector::set_good_masks, whose own comment said it was written to "match live
// set_good_masks shape". They are now the same function evaluated at the initial epoch, and the
// old literals survive only as witnesses in the static_asserts below: if the shared formula ever
// drifts from what shipped, the build stops here rather than at the first flip.
//
// ⭐ The named constexpr constants are load-bearing, not style. These globals are
// constant-initialised today (they land in .data), and both the compiler-emitted barriers and
// BaseObject.cpp itself read them before main. Routing through a `constexpr unsigned long`
// makes a platform on which the expression is not a constant expression a compile error instead
// of a silent demotion to dynamic initialisation -- which would leave the masks reading 0 during
// static init, i.e. every reference load-good, i.e. no barrier at all.
namespace {
constexpr unsigned long kLoadGood0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.remapColour);
constexpr unsigned long kLoadBad0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.loadBad);
constexpr unsigned long kMarkBad0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.markBad);
constexpr unsigned long kStoreBad0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.storeBad);
constexpr unsigned long kStoreGood0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.storeGood);

// Witnesses: the literal expressions this file carried before c4unify, verbatim.
static_assert(kLoadGood0 == MapleRuntime::ZPointerRemapped00,
              "g_cjLoadGoodMask initial value changed");
static_assert(kLoadBad0 == (MapleRuntime::TAGGED_BITS_MASK |
                            (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00)),
              "g_cjLoadBadMask initial value changed");
// Mark-good includes load-good plus the current young and old mark epochs. The initial current
// epochs are *_0, so their *_1 bits are bad (OpenJDK zAddress.cpp:78-87,120-127).
static_assert(kMarkBad0 == (MapleRuntime::TAGGED_BITS_MASK |
                            (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00) |
                            MapleRuntime::MARKED_YOUNG_1 | MapleRuntime::MARKED_OLD_1),
              "g_cjMarkBadMask initial value changed");
// Store-good = mark-good | current Remembered (initial REMEMBERED_0). Store-bad rejects the
// other rem bit and all mark-bad bits (OpenJDK zAddress.cpp:83-87).
static_assert(kStoreBad0 == (MapleRuntime::TAGGED_BITS_MASK |
                             (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00) |
                             MapleRuntime::MARKED_YOUNG_1 | MapleRuntime::MARKED_OLD_1 |
                             MapleRuntime::REMEMBERED_1),
              "g_cjStoreBadMask initial value changed");
// Store-good = current remap | current MY | current MO | current Remembered
// (OpenJDK zAddress.cpp:83). Initial epoch: Remapped00 | MY_0 | MO_0 | REM_0.
static_assert(kStoreGood0 == (MapleRuntime::ZPointerRemapped00 | MapleRuntime::MARKED_YOUNG_0 |
                              MapleRuntime::MARKED_OLD_0 | MapleRuntime::REMEMBERED_0),
              "g_cjStoreGoodMask initial value changed");
// good/bad complementarity at the initial epoch (zAddress.cpp:87):
// StoreBad == StoreGood ^ StoreMetadataMask  (TAGGED_BITS_MASK is 0).
static_assert((kStoreGood0 ^ static_cast<unsigned long>(MapleRuntime::STORE_METADATA_MASK)) == kStoreBad0,
              "g_cjStoreGoodMask ^ STORE_METADATA_MASK != g_cjStoreBadMask at init");
} // namespace

extern "C" MRT_EXPORT unsigned long g_cjLoadGoodMask = kLoadGood0;

extern "C" unsigned long g_cjLoadBadMask = kLoadBad0;

extern "C" MRT_EXPORT unsigned long g_cjMarkBadMask = kMarkBad0;

extern "C" MRT_EXPORT unsigned long g_cjStoreBadMask = kStoreBad0;

extern "C" MRT_EXPORT unsigned long g_cjStoreGoodMask = kStoreGood0;
