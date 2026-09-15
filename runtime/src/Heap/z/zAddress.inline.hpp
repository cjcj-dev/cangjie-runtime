// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include "Heap/z/zAddress.hpp"
namespace MapleRuntime {
constexpr BadMasks ComputeBadMasks(EpochColours e)
{
    // :133  currentRemapColour = ZPointerRemappedYoungMask & ZPointerRemappedOldMask;
    const uintptr_t remapColour = e.remappedYoungMask & e.remappedOldMask;
    // loadBad = REMAP_COLOUR_MASK ^ currentRemapColour  (TAGGED_BITS_MASK is 0)
    const uintptr_t loadBad = TAGGED_BITS_MASK | (REMAP_COLOUR_MASK ^ remapColour);
    // :135  g_cjMarkBadMask = loadBad | (MARKED_YOUNG_MASK & ~currentMarkedYoung)
    //                                 | (MARKED_OLD_MASK & ~currentMarkedOld);
    const uintptr_t markBad =
        loadBad | (MARKED_YOUNG_MASK & ~e.markedYoung) | (MARKED_OLD_MASK & ~e.markedOld);
    // :139  g_cjStoreBadMask = markBad | (REMEMBERED_MASK & ~currentRemembered);
    const uintptr_t storeBad = markBad | (REMEMBERED_MASK & ~e.remembered);
    // :83   ZPointerStoreGoodMask = MarkGood | Remembered
    //       = current remap | current MarkedYoung | current MarkedOld | current Remembered.
    // :87   StoreBad = StoreGood ^ StoreMetadataMask  (TAGGED_BITS_MASK is 0).
    const uintptr_t storeGood = remapColour | e.markedYoung | e.markedOld | e.remembered;
    return BadMasks{ remapColour, loadBad, markBad, storeBad, storeGood };
}

// The epoch WCollector starts in: the member initialisers at WCollector.h:116-122.
constexpr EpochColours kInitialEpochColours = { ZPointerRemapped10 | ZPointerRemapped00,
                                                ZPointerRemapped01 | ZPointerRemapped00,
                                                MARKED_YOUNG_0,
                                                MARKED_OLD_0,
                                                REMEMBERED_0 };

// zAddress.cpp:87 at the initial epoch: StoreBad == StoreGood ^ StoreMetadataMask.
constexpr BadMasks kInitialBadMasks = ComputeBadMasks(kInitialEpochColours);
static_assert((kInitialBadMasks.storeGood ^ STORE_METADATA_MASK) == kInitialBadMasks.storeBad,
              "initial StoreGood ^ STORE_METADATA_MASK != StoreBad");

// ── raw bit views (for CAS expected/new, masks, logging) ──────────────────
// 凭什么: enum class stores the same bits; raw is identity, not a state change.
constexpr Uptr raw(zpointer p) { return static_cast<Uptr>(p); }
constexpr Uptr raw(zaddress a) { return static_cast<Uptr>(a); }
constexpr Uptr raw(zaddress_unsafe u) { return static_cast<Uptr>(u); }
constexpr Uptr raw(zoffset offset) { return static_cast<Uptr>(offset); }

// ── constructors from raw machine words ───────────────────────────────────
// to_zpointer: 凭什么: value was just read from a ref-field slot (or is about to
// be written into one). Only valid at the slot boundary.
constexpr zpointer to_zpointer(Uptr v) { return static_cast<zpointer>(v); }

// to_zaddress_unsafe: 凭什么: bits are already uncoloured, but the referent may
// be dead / unmapped (e.g. strip-only, or a non-heap word mistaken for a ref).
constexpr zaddress_unsafe to_zaddress_unsafe(Uptr v) { return static_cast<zaddress_unsafe>(v); }

// to_zaddress (raw): 凭什么: caller already holds a proven-good uncoloured address
// (null, or a value that went through make_load_good / safe). Prefer those.
constexpr zaddress to_zaddress(Uptr v) { return static_cast<zaddress>(v); }

// ── state transitions ─────────────────────────────────────────────────────
// safe: 凭什么: caller has *separately* proven the memory is live/mapped.
// Every call site MUST document the proof in a comment. If you cannot write the
// proof, the site is a defect — report it, do not call safe().
constexpr zaddress safe(zaddress_unsafe u) { return static_cast<zaddress>(raw(u)); }

// uncolor_bits: strip colour high bits → address bits only, still unsafe.
// 凭什么: bit layout (ColourMask.h); does NOT run a barrier or check liveness.
constexpr zaddress_unsafe uncolor_bits(zpointer p)
{
    // address occupies bits 0..47 on 64-bit (RefField.h); ARM32 is abandoned.
    return to_zaddress_unsafe(raw(p) & ((Uptr(1) << 48) - 1u));
}

// to_object: 凭什么: sole exit from the colour type system to a C++ object pointer.
// Input must already be zaddress (load-good or proven).
// ⭐ This is the ONLY production site allowed to write reinterpret_cast<BaseObject*>.
// All other paths must go through a named constructor below (or this).
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
extern const bool ZVerifyOops;
void VerifyAccessedOop(zaddress address);
#endif
inline BaseObject* to_object(zaddress a)
{
#if defined(MRT_DEBUG) && MRT_DEBUG == 1
    // zAddress.inline.hpp:505-522: verify the actual accessed oop.
    if (ZVerifyOops && a != zaddress::null) { VerifyAccessedOop(a); }
#endif
    return reinterpret_cast<BaseObject*>(raw(a));
}

// from_object: 凭什么: a live BaseObject* in hand is already an uncoloured, safe address.
inline zaddress from_object(const BaseObject* obj)
{
    return to_zaddress(reinterpret_cast<Uptr>(obj));
}

// ── non-RefField origins of BaseObject* (ctyperest exemption set) ─────────
// Bare reinterpret_cast<BaseObject*> outside this header is forbidden
// (tools/check_no_bare_baseobject_cast.sh). Every site picks one of these
// and documents *why* the bits are an uncoloured object base.

// from_region_addr: 凭什么: address computed from region metadata
// (region start / unit / free-slot / route to-addr / walk position).
// Allocator owns the layout; bits are never colour-tagged.
inline BaseObject* from_region_addr(Uptr addr)
{
    return to_object(to_zaddress(addr));
}

// from_alloc_addr: 凭什么: address just returned by the allocator
// (SetClassInfo / NewFinalizer / AllocPinned). Memory is committed and
// about to be / just was initialised as a BaseObject; never coloured.
inline BaseObject* from_alloc_addr(Uptr addr)
{
    return to_object(to_zaddress(addr));
}

// from_native_ref: 凭什么: MCC / Sync / scheduler entry holding a managed
// object as void* / C layout view (CJFuture/CJMutex/…). Protocol: the
// mutator published an uncoloured heap ref; not a RefField load.
// ⚠ Bits are NOT run through a read barrier here — caller must not pass
// a raw field load. Stack-map base pointers (BasePtrType = Uptr) use the
// Uptr overload.
// High 16 bits must be clear (plain address). Coloured bits here mean a
// missing barrier at the call site (traceuncolour: mutexPtr bare load).
inline BaseObject* from_native_ref(Uptr p)
{
    // Address occupies bits 0..47; colour/tag live in 48..63 (RefField.h).
    if (p != 0 && (p >> 48) != 0) {
        // Keep CHECK out of this header (no Log.h); hard fail matches prior OOB.
        __builtin_trap();
    }
    return to_object(to_zaddress(p));
}
inline BaseObject* from_native_ref(const void* p)
{
    return from_native_ref(reinterpret_cast<Uptr>(p));
}

// as_abi_ref_slot: 凭什么: stack buffer used as an ABI sret / arg-register
// placeholder in ArgValue::AddReference. ⛔ NOT a heap object; ⛔ must not
// be treated as a GC root. Exists only so the call stub sees a "ref" slot.
inline BaseObject* as_abi_ref_slot(void* p)
{
    return to_object(to_zaddress(reinterpret_cast<Uptr>(p)));
}

// null checks (enum class does not compare to 0 without cast)
constexpr bool is_null(zpointer p) { return p == zpointer::null; }
constexpr bool is_null(zaddress a) { return a == zaddress::null; }
constexpr bool is_null(zaddress_unsafe u) { return u == zaddress_unsafe::null; }


}
namespace MapleRuntime {
namespace ColourPredicates {

constexpr unsigned HEAP_ADDRESS_BITS = 48u;
constexpr uintptr_t HEAP_ADDRESS_MASK = (uintptr_t(1) << HEAP_ADDRESS_BITS) - 1u;

constexpr bool has_address(uintptr_t value)
{
    return (value & HEAP_ADDRESS_MASK) != 0;
}

// The current combined RemappedYoung x RemappedOld bit is the only remap bit
// excluded from load-bad. This derives it from the compiler ABI mask instead
// of publishing a second current-remap global.
constexpr uintptr_t current_remapped(uintptr_t loadBadMask)
{
    return REMAP_COLOUR_MASK & ~loadBadMask;
}

constexpr uintptr_t current_remapped_young_mask(uintptr_t loadBadMask)
{
    const uintptr_t current = current_remapped(loadBadMask);
    return (current == ZPointerRemapped00 || current == ZPointerRemapped10)
        ? (ZPointerRemapped00 | ZPointerRemapped10)
        : (current == ZPointerRemapped01 || current == ZPointerRemapped11)
            ? (ZPointerRemapped01 | ZPointerRemapped11)
            : uintptr_t(0);
}

constexpr uintptr_t current_remapped_old_mask(uintptr_t loadBadMask)
{
    const uintptr_t current = current_remapped(loadBadMask);
    return (current == ZPointerRemapped00 || current == ZPointerRemapped01)
        ? (ZPointerRemapped00 | ZPointerRemapped01)
        : (current == ZPointerRemapped10 || current == ZPointerRemapped11)
            ? (ZPointerRemapped10 | ZPointerRemapped11)
            : uintptr_t(0);
}

constexpr uintptr_t current_marked_young(uintptr_t markBadMask)
{
    return MARKED_YOUNG_MASK & ~markBadMask;
}

constexpr uintptr_t current_marked_old(uintptr_t markBadMask)
{
    return MARKED_OLD_MASK & ~markBadMask;
}

// Finalizable is reserved but not yet published (kFinalizableWired == false).
// ZGC flips it with MarkedOld. Deriving the reserved current bit from the live
// MarkedOld epoch lets the predicate and its tests exist without pretending
// that any product phase currently emits the bit.
constexpr uintptr_t current_finalizable(uintptr_t markBadMask)
{
    const uintptr_t markedOld = current_marked_old(markBadMask);
    return markedOld == MARKED_OLD_0 ? FINALIZABLE_0
        : markedOld == MARKED_OLD_1 ? FINALIZABLE_1 : uintptr_t(0);
}

constexpr uintptr_t current_remembered(uintptr_t storeBadMask)
{
    return REMEMBERED_MASK & ~storeBadMask;
}

// ZPointer::is_load_bad -- true when a non-current remap bit is present.
// Mid-evacuation is not a pointer bit (zAddress.hpp:60-128). A plain word is
// not mask-bad; HeapSlot encoding legality is enforced at publication and the
// legacy-generation load-heal path explicitly diverts it before this predicate.
// The answer can change when the remap epoch flips in GC_PHASE_PREFORWARD.
constexpr bool is_load_bad(uintptr_t value, uintptr_t loadBadMask)
{
    return (value & loadBadMask) != 0;
}

// ZPointer::is_remapped -- current combined remap epoch. Changes at relocate
// start (our GC_PHASE_PREFORWARD paths).
constexpr bool is_remapped(uintptr_t value, uintptr_t loadBadMask)
{
    const uintptr_t remapped = current_remapped(loadBadMask);
    return remapped != 0 && (value & remapped) != 0;
}

// ZPointer::is_load_good (zAddress.inline.hpp:631-633).
constexpr bool is_load_good(uintptr_t value, uintptr_t loadBadMask)
{
    return value != 0 && !is_load_bad(value, loadBadMask);
}

constexpr bool is_load_good_or_null(uintptr_t value, uintptr_t loadBadMask)
{
    (void)value;
    return !is_load_bad(value, loadBadMask);
}

// As in ZGC, the negative-mask predicate alone does not establish encoding
// legality: a plain non-null word is not mask-bad. HeapSlot publication is
// separately fail-closed through ClassifySlotWord.
//
// ZPointer::is_young_load_good/is_old_load_good -- true when the word's remap
// bit belongs to the current conceptual young/old half of the four-way colour.
// The halves change at the generation's relocate start in
// GC_PHASE_PREFORWARD. Both masks are derived from g_cjLoadBadMask; no phase
// or duplicate epoch word is consulted.
constexpr bool is_young_load_good(uintptr_t value, uintptr_t loadBadMask)
{
    return (value & current_remapped_young_mask(loadBadMask)) != 0;
}

constexpr bool is_old_load_good(uintptr_t value, uintptr_t loadBadMask)
{
    return (value & current_remapped_old_mask(loadBadMask)) != 0;
}

// ZPointer::is_mark_bad -- true for any load-bad bit or a stale
// MarkedYoung/MarkedOld bit. Mark epochs change around GC_PHASE_ENUM and remap
// epochs change during GC_PHASE_PREFORWARD.
constexpr bool is_mark_bad(uintptr_t value, uintptr_t markBadMask)
{
    return (value & markBadMask) != 0;
}

// ZPointer::is_mark_good (zAddress.inline.hpp:658-664).
constexpr bool is_mark_good(uintptr_t value, uintptr_t loadBadMask, uintptr_t markBadMask)
{
    (void)loadBadMask;
    return value != 0 && !is_mark_bad(value, markBadMask);
}

constexpr bool is_mark_good_or_null(uintptr_t value, uintptr_t loadBadMask, uintptr_t markBadMask)
{
    (void)value;
    (void)loadBadMask;
    return !is_mark_bad(value, markBadMask);
}

// Encoding completeness is checked at HeapSlot publication, not repeated in
// this phase predicate. ENUM/PREFORWARD changes are represented by the bad
// masks passed here.
//
// ZPointer::is_store_bad -- true for any mark-bad bit or a stale Remembered
// bit. Remembered changes with MarkedYoung around GC_PHASE_ENUM.
constexpr bool is_store_bad(uintptr_t value, uintptr_t storeBadMask)
{
    return (value & storeBadMask) != 0;
}

// ZPointer::is_store_good (zAddress.inline.hpp:683-685).
constexpr bool is_store_good(uintptr_t value, uintptr_t loadBadMask, uintptr_t storeBadMask)
{
    (void)loadBadMask;
    return value != 0 && !is_store_bad(value, storeBadMask);
}

constexpr bool is_store_good_or_null(uintptr_t value, uintptr_t loadBadMask, uintptr_t storeBadMask)
{
    (void)value;
    (void)loadBadMask;
    return !is_store_bad(value, storeBadMask);
}

// Encoding completeness is checked at HeapSlot publication, not repeated in
// this phase predicate. This function is the ZGC single-not-bad test.

// ZPointer::is_marked_finalizable -- tests the current reserved finalizable
// epoch. Synthetic words can exercise it, but kFinalizableWired documents that
// no GC_PHASE currently publishes such a word.
constexpr bool is_marked_finalizable(uintptr_t value, uintptr_t markBadMask)
{
    const uintptr_t finalizable = current_finalizable(markBadMask);
    return finalizable != 0 && (value & finalizable) != 0;
}

// ZPointer::is_marked_old -- true for the current MarkedOld epoch bit. Full
// collection flips it before GC_PHASE_ENUM; young collection leaves it alone.
constexpr bool is_marked_old(uintptr_t value, uintptr_t markBadMask)
{
    const uintptr_t markedOld = current_marked_old(markBadMask);
    return markedOld != 0 && (value & markedOld) != 0;
}

// ZPointer::is_marked_young -- true for the current MarkedYoung epoch bit. It
// flips around GC_PHASE_ENUM in both full and young collections.
constexpr bool is_marked_young(uintptr_t value, uintptr_t markBadMask)
{
    const uintptr_t markedYoung = current_marked_young(markBadMask);
    return markedYoung != 0 && (value & markedYoung) != 0;
}

// ZPointer::is_marked_any_old -- true for current MarkedOld or current
// Finalizable. Today only MarkedOld is publishable; Finalizable maps to no
// GC_PHASE while kFinalizableWired is false.
constexpr bool is_marked_any_old(uintptr_t value, uintptr_t markBadMask)
{
    return is_marked_old(value, markBadMask) || is_marked_finalizable(value, markBadMask);
}

// ZPointer::is_remembered_exact -- true when the current Remembered epoch bit
// is present. It changes with MarkedYoung around GC_PHASE_ENUM.
constexpr bool is_remembered_exact(uintptr_t value, uintptr_t storeBadMask)
{
    const uintptr_t remembered = current_remembered(storeBadMask);
    return remembered != 0 && (value & remembered) == remembered;
}

constexpr unsigned ZGC_PREDICATE_COUNT = 17u;

} // namespace ColourPredicates

// ZAddress::mark_young_good, zAddress.inline.hpp:793-805.
inline zpointer ColorAddressMarkYoungGood(zaddress address, zpointer previous)
{
    if (!ColourPredicates::has_address(raw(previous))) {
        return to_zpointer(::g_cjStoreGoodMask | REMEMBERED_MASK);
    }
    const uintptr_t oldMarked = raw(previous) & (MARKED_OLD_MASK | FINALIZABLE_MASK);
    return to_zpointer(raw(address) | (::g_cjLoadBadMask ^ REMAP_COLOUR_MASK) |
                      (MARKED_YOUNG_MASK & ~::g_cjMarkBadMask) | oldMarked | REMEMBERED_MASK);
}

} // namespace MapleRuntime
