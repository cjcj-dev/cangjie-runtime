// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REF_FIELD_H
#define MRT_REF_FIELD_H

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <type_traits>

#include "Base/Log.h"
#include "Common/ColourMask.h"
#include "Common/ColourEncoding.h"
#include "Common/ColourPredicates.h"
#include "Common/TypeDef.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
#ifdef __arm__
#define ARM32_MARKED_FLAG_BITS  2
#endif
class BaseObject;
class WCollector;

namespace HealPairDiag {
void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site);
}

// Instrumentation for the ZBarrier::self_heal port below. Declared rather than
// included for the same reason HealPairDiag is: this header sits under Heap/ in
// the include graph, not above it. Full contract in Heap/Verify/ZgcSelfHealDiag.h.
namespace ZgcSelfHealDiag {
void CheckTransitionMonotonicity(zpointer oldPtr, zpointer healPtr);
void NotePreconditions(bool ptrFastPath, bool healFastPath, zpointer healPtr);
void NoteEnter();
void NoteNullSkip();
void NoteHealed(unsigned iterations);
void NoteFastPathExit(unsigned iterations);
void NoteRetry(unsigned iterations);
}

// Every heap/root healing write names its owning algorithm.  This is deliberately
// exhaustive: a catch-all value would recreate the attribution gap HealSlot closes.
enum class HealSite : uint16_t {
    BaseObjectCompareExchangeRefField,
    BarrierReadStaticReference,
    BarrierCompareAndSwapReference,
    EnumCompareAndSwapReference,
    EnumCopyStructArrayRecolour,
    EnumReadReference,
    ForwardAtomicReadReference,
    ForwardCompareAndSwapReference,
    ForwardReadReference,
    IdleAtomicReadReference,
    IdleCompareAndSwapReference,
    IdleReadReference,
    MutatorPreForwardHeaderlessRecord,
    MutatorPreForwardInterior,
    MutatorPreForwardRoot,
    MutatorPreForwardStackField,
    MutatorStripRootColour,
    PostTraceAtomicReadReference,
    PostTraceCompareAndSwapReference,
    PostTraceCopyStructArrayRecolour,
    PostTraceReadReference,
    PreforwardAtomicReadReference,
    PreforwardCompareAndSwapReference,
    PreforwardReadReference,
    TraceCompareAndSwapReference,
    TraceCopyStructArrayRecolour,
    TraceReadReference,
    TracingCollectorResurrectFinalizer,
    TracingCollectorTraceRefField,
    WCollectorEnumRawInteriorRoot,
    WCollectorEnumRawRoot,
    WCollectorEnumRefFieldRoot,
    WCollectorFixOldTaggedLive,
    WCollectorFixOldTaggedNonHeap,
    WCollectorFixRootForwarded,
    WCollectorFixRootInteriorForward,
    WCollectorFixRootPostForwardInterior,
    WCollectorForwardRawGhost,
    WCollectorForwardRawInterior,
    WCollectorGetAndTryTagObj,
    WCollectorMinorFixForwarded,
    WCollectorMinorFixForwardNull,
    WCollectorMinorFixInteriorForward,
    WCollectorMinorFixInteriorPostForward,
    WCollectorMinorFixInteriorPreserve,
    WCollectorMinorResolveLoadGoodForward,
    WCollectorMinorResolveOldForward,
    WCollectorNormalizeRawRoot,
    WCollectorPreserveRawInterior,
    WCollectorPreserveRootInterior,
    WCollectorRemapYoungRoots,
    WCollectorResolveRootLoadGoodForward,
    WCollectorResolveRootOldForward,
    WCollectorTraceRefField,
    WCollectorTryUntagRefField,
    WCollectorTryUpdateRefField,
};

enum class HealNull : uint8_t { Disallow, Allow };

template<bool isAtomic>
class HeapSlot;

// observedOut: OpenJDK ZBarrier::self_heal (zBarrier.inline.hpp:92-107) needs the value the
// CAS actually found, so it can re-apply the same heal value to it. Optional; nullptr leaves
// every existing caller on the plain success/failure contract.
// ⚠ It is written only when the CAS is reached -- the non-null-to-null guard below returns
// before that, so a Disallow caller must not read it.
template<bool isAtomic>
inline bool HealSlot(HeapSlot<isAtomic>& slot, zpointer expected, zpointer desired, HealSite site,
                     HealNull allowNull = HealNull::Disallow,
                     std::memory_order succOrder = std::memory_order_relaxed,
                     std::memory_order failOrder = std::memory_order_relaxed,
                     zpointer* observedOut = nullptr);

// Full-colour write funnel: every HeapSlot write is checked unconditionally;
// RootSlot/DerivedSlot addresses remain outside this heap-only admission.
void AssertColouredWriteIfEnabled(const void* slot, MAddress newVal);

/* there are several similar terms about object address:
    1. address: the start position of any virtual memory block.
    2. object-ref: an address pointing to some object, RawRoot and RawRef are object-ref.
    3. ref-field: field in object or class pointing to some object.
        global/static reference is also treated (implemented) as ref-field.
        ref-field is implemented in tagged-pointer.
*/
template<bool isAtomic = false>
class HeapSlot {
public:
    // size in bytes
    static constexpr size_t GetSize() { return sizeof(fieldVal); }

    // 剥色地址位。返回 zaddress：调用方把「槽值地址位」当可解引用对象基址使用。
    // ⚠ 本函数不做读屏障；需要 load-good 的路径必须走 Collector::make_load_good。
    // 类型纪律见 ops/design/COLOUR_TYPE_DISCIPLINE.md。
    zaddress GetTargetObject(std::memory_order order = std::memory_order_relaxed) const
    {
        // Always atomic: mutator plain path races with concurrent GC mark/CAS (R1/R3).
        // relaxed keeps cost near a plain load on x86_64/aarch64 while establishing HB.
#if defined(CANGJIE_TSAN_SUPPORT)
        MAddress value = static_cast<MAddress>(Sanitizer::TsanAtomicLoad(&fieldVal, order));
#else
        MAddress value = __atomic_load_n(&fieldVal, order);
#endif
        return to_zaddress(HeapSlot<>(value).GetAddress());
    }

    // 带色原值。返回 zpointer：⛔ 不可解引用，只能进屏障 / CAS / 写回槽。
    zpointer GetFieldValue(std::memory_order order = std::memory_order_relaxed) const
    {
#if defined(CANGJIE_TSAN_SUPPORT)
        return to_zpointer(static_cast<MAddress>(Sanitizer::TsanAtomicLoad(&fieldVal, order)));
#else
        return to_zpointer(static_cast<MAddress>(__atomic_load_n(&fieldVal, order)));
#endif
    }

    // puborder: publishing a reference is a *release*, not a plain store.
    //
    // The default was relaxed, which lets another thread observe the reference before it observes
    // the header write that made the target a valid object.  The reader then hands out an object
    // whose header word is still zero, and the mutator faults on the first field it loads out of
    // it -- `mov 0x20(%rbx),%rax` with rbx = 0, si_addr = 0x20, which is 7 of 10 crashes here.
    //
    // The measurement that identified this: at the hand-out point, targets whose header is zero and
    // which have no to-version to resolve to sit in regions typed THREAD_LOCAL (6) and RECENT_FULL
    // (7) with garbage=0, free=0, ghost=0 -- live allocation regions, not reclaimed ones.  A zero
    // header in a live allocation region is an object that has not been initialised yet, not one
    // that was collected, which is why every reclaim-side hypothesis failed to explain it.
    //
    // OpenJDK does not need an explicit release here because safe publication is the Java memory
    // model's job and C2 emits the barrier; in C++ the ordering has to be written down.
    void StoreColoured(zpointer value, std::memory_order order = std::memory_order_release);

private:
    template<bool atomic>
    friend bool HealSlot(HeapSlot<atomic>&, zpointer, zpointer, HealSite, HealNull,
                         std::memory_order, std::memory_order, zpointer*);

    bool CompareExchange(zpointer expectedValue, zpointer newValue,
                         std::memory_order succOrder = std::memory_order_relaxed,
                         std::memory_order failOrder = std::memory_order_relaxed,
                         zpointer* observedOut = nullptr)
    {
        MAddress expectedRaw = raw(expectedValue);
        MAddress newRaw = raw(newValue);
        CHECK(std::numeric_limits<MAddress>::max() > newRaw);
        AssertColouredWriteIfEnabled(this, newRaw);
#if defined(CANGJIE_TSAN_SUPPORT)
        // tsan will get expectedValue's address for us, just pass the real value
        auto ret = Sanitizer::TsanAtomicCompareExchange(&fieldVal, expectedRaw, newRaw, succOrder, failOrder);
        if (observedOut != nullptr) {
            *observedOut = to_zpointer(static_cast<MAddress>(ret));
        }
        return (ret == expectedRaw);
#else
        bool ok = __atomic_compare_exchange(&fieldVal, &expectedRaw, &newRaw, false, succOrder, failOrder);
        // __atomic_compare_exchange overwrites expectedRaw with the observed word on failure and
        // leaves it alone on success, which is exactly ZGC's prev_ptr.
        if (observedOut != nullptr) {
            *observedOut = to_zpointer(expectedRaw);
        }
        return ok;
#endif
    }

public:
    zpointer Exchange(zpointer newRef, std::memory_order order = std::memory_order_relaxed)
    {
        MAddress newRaw = raw(newRef);
        CHECK(newRaw < std::numeric_limits<RefFieldValue>::max());
        AssertColouredWriteIfEnabled(this, newRaw);
        MAddress ret = 0;
#if defined(CANGJIE_TSAN_SUPPORT)
        ret = Sanitizer::TsanAtomicExchange(&fieldVal, newRaw, order);
#else
        __atomic_exchange(&fieldVal, &newRaw, &ret, order);
#endif
        return to_zpointer(static_cast<MAddress>(ret));
    }

    // 地址位（已剥 colour）。返回裸 MAddress 供布局/偏移算术；
    // 若要当对象指针，经 uncolor_bits(GetFieldValue()) 或 GetTargetObject()。
    MAddress GetAddress() const
    {
#ifdef __arm__
        return address << ARM32_MARKED_FLAG_BITS;
#else
        return address;
#endif
    }

    ~HeapSlot() = default;
    explicit HeapSlot(MAddress val) : fieldVal(val) {}
    // 凭什么: zpointer 就是槽里的带色位模式，与 MAddress 同宽。
    explicit HeapSlot(zpointer val) : fieldVal(static_cast<RefFieldValue>(raw(val))) {}
    HeapSlot(const HeapSlot& ref) : fieldVal(ref.fieldVal) {}
#ifdef __arm__
    HeapSlot(const BaseObject* obj, MAddress colour)
    {
        (void)colour;
        address = reinterpret_cast<MAddress>(obj) >> ARM32_MARKED_FLAG_BITS;
    }
#else
    HeapSlot(const BaseObject* obj, MAddress colour)
        : address(reinterpret_cast<MAddress>(obj)),
          remapColour((colour >> REMAP_COLOUR_SHIFT) & ((MAddress(1) << REMAP_COLOUR_BITS) - 1)),
          markedYoung((colour >> MARKED_YOUNG_SHIFT) & ((MAddress(1) << MARKED_YOUNG_BITS) - 1)),
          markedOld((colour >> MARKED_OLD_SHIFT) & ((MAddress(1) << MARKED_OLD_BITS) - 1)),
          padding((colour >> REMEMBERED_SHIFT) & ((MAddress(1) << TAG_ID_PADDING_BITS) - 1))
    {
    }
#endif

    HeapSlot(HeapSlot&& ref) : fieldVal(ref.fieldVal) {}
    HeapSlot() = delete;
    HeapSlot& operator=(const HeapSlot&) = delete;
    HeapSlot& operator=(const HeapSlot&&) = delete;

private:
    // heapdesired: plain BaseObject* carrier is not a public heap-CAS desired.
    // Only WCollector (GetAndTryTagRefField / RootSlotWriteback plain-root arm /
    // null install) may mint it. Outside code that needs a plain value must say
    // so via zpointer/MAddress or the colour-carrying constructors above —
    // RefField<>(obj) as CompareExchange desired is a compile error.
    explicit HeapSlot(const BaseObject* obj) : fieldVal(0)
    {
#ifdef __arm__
        address = reinterpret_cast<MAddress>(obj) >> ARM32_MARKED_FLAG_BITS;
#else
        address = reinterpret_cast<MAddress>(obj);
#endif
    }
    friend class WCollector;
#ifdef __arm__
    using RefFieldValue = U32;
#else
    using RefFieldValue = MAddress;
#endif

#ifdef __arm__
    union {
        struct {
            MAddress address : 30;
            MAddress reserved : 2;
        };
        RefFieldValue fieldVal;
    };
#else
    union {
        struct {
            MAddress address : 48;
            // ZGC layout (zAddress.hpp:60-128): remap starts at bit 48. No isTagged, no tagID.
            MAddress remapColour : REMAP_COLOUR_BITS;
            MAddress markedYoung : MARKED_YOUNG_BITS;
            MAddress markedOld : MARKED_OLD_BITS;
            MAddress padding : TAG_ID_PADDING_BITS;
        };
        RefFieldValue fieldVal;
    };
    static_assert(48 + REMAP_COLOUR_BITS + MARKED_YOUNG_BITS + MARKED_OLD_BITS + TAG_ID_PADDING_BITS == 64,
                  "HeapSlot colour layout must fill 64 bits");
    static_assert(TAGGED_BITS_MASK == 0, "pointer no longer carries isTagged/tagID");
    static_assert(TAG_ID_COUNT > 1 && TAG_ID_COUNT <= (1u << TAG_ID_BITS), "TAG_ID_COUNT out of bit width");
#endif
};

// The sole HeapSlot compare-exchange write.  Match ZBarrier::self_heal: a
// non-null observed reference must not be healed to null unless its owner makes
// that destructive transition explicit.
template<bool isAtomic>
inline bool HealSlot(HeapSlot<isAtomic>& slot, zpointer expected, zpointer desired, HealSite site,
                     HealNull allowNull, std::memory_order succOrder, std::memory_order failOrder,
                     zpointer* observedOut)
{
    (void)site;
    if (allowNull == HealNull::Disallow && !is_null(expected) && is_null(desired)) {
        return false;
    }
    bool ok = slot.CompareExchange(expected, desired, succOrder, failOrder, observedOut);
    if (ok && is_null(desired)) {
        HealPairDiag::NoteZeroWrite(&slot, raw(expected), raw(desired), static_cast<uint16_t>(site));
    }
    return ok;
}

// ZBarrier::self_heal is the product path (zBarrier.inline.hpp:72-110). The
// bounded kSelfHealAttempts loop is gone: ZGC's loop is unbounded and
// terminates on colour monotonicity.

// OpenJDK ZBarrier::self_heal, zBarrier.inline.hpp:72-110, transcribed.
//
// Two things it does that the bounded kSelfHealAttempts loop does not:
//   * :89       assert_transition_monotonicity before every CAS attempt;
//   * :103-107  on a lost CAS it re-applies the *same* heal value to the newly observed
//               word, so a slot another barrier left on weaker (remapped or finalizable)
//               metadata still gets upgraded. The bounded loop instead re-resolves from
//               scratch and, once kSelfHealAttempts is spent, returns the payload without
//               writing the slot at all -- which is how a slot can stay weak indefinitely.
//
// fastPath is the barrier's own ZBarrierFastPath, passed in rather than assumed: the tree
// carries two definitions of load-good (Collector.h:161-182) and the exit test has to be
// the one this caller would itself have accepted.
//
// The loop is unbounded, exactly as ZGC's is. Entry enforces ZGC's value
// qualification: the observed word is load-bad and the resolved heal word is
// load-good. Transition diagnostics then witness monotonic convergence.
template<bool isAtomic, typename FastPath>
inline bool ZgcSelfHeal(HeapSlot<isAtomic>& slot, zpointer ptr, zpointer healPtr, FastPath fastPath,
                        HealSite site, HealNull allowNull = HealNull::Disallow)
{
    // :73-79  Never heal with null since it interacts badly with reference processing.
    // ZGC's guard is `is_null_assert_load_good(heal_ptr) && !is_null_any(ptr)`; is_null_any
    // tests the address bits rather than the whole word, and ColourPredicates::has_address
    // (ColourPredicates.h:37-40) is that test.
    if (allowNull == HealNull::Disallow && is_null(healPtr) &&
        ColourPredicates::has_address(static_cast<uintptr_t>(raw(ptr)))) {
        ZgcSelfHealDiag::NoteNullSkip();
        return false;
    }

    ZgcSelfHealDiag::NoteEnter();
    // :82-87  assert_is_valid / assert(!fast_path(ptr)) / assert(fast_path(heal_ptr)) /
    //         assert(ZPointer::is_remapped(heal_ptr))
    const bool ptrFastPath = fastPath(ptr);
    const bool healFastPath = fastPath(healPtr);
    ZgcSelfHealDiag::NotePreconditions(ptrFastPath, healFastPath, healPtr);
    CHECK_DETAIL(!ptrFastPath, "ZBarrier::self_heal input must be load-bad");
    CHECK_DETAIL(healFastPath, "ZBarrier::self_heal value must be load-good");

    // :89
    for (unsigned iterations = 0;; ++iterations) {
        ZgcSelfHealDiag::CheckTransitionMonotonicity(ptr, healPtr);

        // :91-92  Heal.
        // HealNull::Allow: the :73-79 guard above is ZGC's and has already been applied once
        // at entry. Letting HealSlot re-apply its own version would short-circuit the CAS on a
        // later iteration and leave observedOut unwritten.
        zpointer prevPtr = zpointer::null;
        if (HealSlot(slot, ptr, healPtr, site, HealNull::Allow, std::memory_order_relaxed,
                     std::memory_order_relaxed, &prevPtr)) {
            // :93-96  Success
            ZgcSelfHealDiag::NoteHealed(iterations);
            return true;
        }

        if (fastPath(prevPtr)) {
            // :98-101  Must not self heal
            ZgcSelfHealDiag::NoteFastPathExit(iterations);
            return false;
        }

        // :103-107  The oop location was healed by another barrier, but still needs upgrading.
        // Re-apply healing to make sure the oop is not left with weaker (remapped or
        // finalizable) metadata bits than what this barrier tried to apply.
        ZgcSelfHealDiag::NoteRetry(iterations);
        ptr = prevPtr;
    }
}

template<bool isAtomic = false>
inline void StoreColoured(HeapSlot<isAtomic>& slot, zaddress value, MAddress colour,
                          std::memory_order order = std::memory_order_relaxed)
{
    zpointer coloured = is_null(value) ? zpointer::null : to_zpointer(raw(value) | colour);
    slot.StoreColoured(coloured, order);
}

// Compatibility spelling for code outside the runtime. It denotes HeapSlot only;
// roots and derived locations are different, non-convertible types below.
template<bool isAtomic = false>
using RefField = HeapSlot<isAtomic>;

// OpenJDK ZUncoloredRoot stores an unsafe, uncoloured address in the root and
// carries colour metadata outside the slot (zUncoloredRoot.hpp:32-54).
class RootSlot {
public:
    RootSlot() : rootValue(zaddress_unsafe::null) {}

    zaddress_unsafe LoadPlain(std::memory_order order = std::memory_order_relaxed) const
    {
        zaddress_unsafe value;
        __atomic_load(&rootValue, &value, order);
        return value;
    }

private:
    void StorePlain(zaddress value, std::memory_order order)
    {
        zaddress_unsafe unsafeValue = to_zaddress_unsafe(raw(value));
        __atomic_store(&rootValue, &unsafeValue, order);
    }

    bool CompareExchangePlain(zaddress_unsafe expected, zaddress desired,
                              std::memory_order succOrder, std::memory_order failOrder)
    {
        zaddress_unsafe unsafeDesired = to_zaddress_unsafe(raw(desired));
        return __atomic_compare_exchange(&rootValue, &expected, &unsafeDesired, false, succOrder, failOrder);
    }
    // PLAIN_ROOTS=0 escape hatch; private to WCollector so ordinary root writers stay plain-only.
    void StoreCollectorRollback(zpointer value, std::memory_order order) {
        zaddress_unsafe unsafeValue = to_zaddress_unsafe(raw(value));
        __atomic_store(&rootValue, &unsafeValue, order);
    }
    zaddress_unsafe rootValue;

    friend void StorePlain(RootSlot&, zaddress, std::memory_order);
    friend bool HealRoot(RootSlot&, zaddress, HealSite, HealNull, std::memory_order);
    friend bool HealRootIfObserved(RootSlot&, zaddress_unsafe, zaddress, HealSite,
                                   HealNull, std::memory_order, std::memory_order);
    friend class WCollector;
};

// Read-only root capability. This is intentionally const-qualified rather than a
// second storage representation: static/RELRO reads need no metadata migration,
// while RootSlot write APIs cannot accept this type.
using ReadOnlyRootSlot = const RootSlot;

inline void StorePlain(RootSlot& slot, zaddress value,
                       std::memory_order order = std::memory_order_relaxed)
{
    slot.StorePlain(value, order);
}

inline bool HealRoot(RootSlot& slot, zaddress good, HealSite site,
                     HealNull allowNull = HealNull::Disallow,
                     std::memory_order order = std::memory_order_relaxed)
{
    (void)site;
    zaddress_unsafe observed = slot.LoadPlain(order);
    if (allowNull == HealNull::Disallow && !is_null(observed) && is_null(good)) {
        return false;
    }
    slot.StorePlain(good, order);
    return true;
}

// ZUncoloredRoot::barrier writes the same load-good address that it hands to
// its closure (zUncoloredRoot.inline.hpp:35-60). ReadStaticRef runs in mutator
// context, so preserve a concurrent WriteStaticRef by replacing only the exact
// word this read observed.
inline bool HealRootIfObserved(RootSlot& slot, zaddress_unsafe observed, zaddress good, HealSite site,
                               HealNull allowNull = HealNull::Disallow,
                               std::memory_order succOrder = std::memory_order_relaxed,
                               std::memory_order failOrder = std::memory_order_relaxed)
{
    (void)site;
    if (allowNull == HealNull::Disallow && !is_null(observed) && is_null(good)) {
        return false;
    }
    return slot.CompareExchangePlain(observed, good, succOrder, failOrder);
}

// OpenJDK ProcessDerivedOop preserves the offset, processes the base, then
// restores base+offset (oopMap.cpp:404-424). No raw-address store is public.
class DerivedSlot {
public:
    zaddress_unsafe LoadDerived(std::memory_order order = std::memory_order_relaxed) const
    {
        zaddress_unsafe value;
        __atomic_load(&derivedValue, &value, order);
        return value;
    }

private:
    void StoreDerived(const RootSlot& base, size_t offset, std::memory_order order)
    {
        zaddress_unsafe rebased = to_zaddress_unsafe(raw(base.LoadPlain(order)) + offset);
        __atomic_store(&derivedValue, &rebased, order);
    }

    zaddress_unsafe derivedValue;

    friend void RebaseDerived(DerivedSlot&, const RootSlot&, size_t, std::memory_order);
};

inline void RebaseDerived(DerivedSlot& slot, const RootSlot& base, size_t offset,
                          std::memory_order order = std::memory_order_relaxed)
{
    slot.StoreDerived(base, offset, order);
}

// HeapSlot interior references carry a complete StoreGood colour. Only stackmap
// DerivedSlot remains plain via RebaseDerived.
template<bool isAtomic = false>
inline bool CasInstallInteriorColoured(HeapSlot<isAtomic>& field, zpointer expected,
                                       BaseObject* host, size_t offset, HealSite site)
{
    MAddress address = reinterpret_cast<MAddress>(host) + offset;
    return HealSlot(field, expected,
                    to_zpointer(MakeStoreGoodSlotWord(address, ::g_cjStoreGoodMask)), site);
}

// When the host is unknown, preserve the interior payload but still publish a
// complete StoreGood word.
// Prefer the (host, offset) overload when TryRecoverInteriorBase succeeds.
template<bool isAtomic = false>
inline bool CasInstallInteriorColoured(HeapSlot<isAtomic>& field, zpointer expected,
                                       BaseObject* interior, HealSite site)
{
    MAddress address = reinterpret_cast<MAddress>(interior);
    return HealSlot(field, expected,
                    to_zpointer(MakeStoreGoodSlotWord(address, ::g_cjStoreGoodMask)), site);
}

static_assert(sizeof(HeapSlot<>) == sizeof(MAddress), "HeapSlot must remain one machine word");
static_assert(sizeof(RootSlot) == sizeof(MAddress), "RootSlot must remain one machine word");
static_assert(sizeof(DerivedSlot) == sizeof(MAddress), "DerivedSlot must remain one machine word");

template<typename T>
struct IsSlotStorageType : std::false_type {};

template<bool isAtomic>
struct IsSlotStorageType<HeapSlot<isAtomic>> : std::true_type {};

template<>
struct IsSlotStorageType<RootSlot> : std::true_type {};

template<>
struct IsSlotStorageType<DerivedSlot> : std::true_type {};

// The compiler/stack-map/object-layout ABIs expose raw word addresses. Typed
// pointers are rejected at this boundary, but this does not make a category
// escape impossible: a caller that independently knows the ABI category can
// still spell static_cast<void*> explicitly, leaving a visible review token.
template<bool isAtomic = false>
inline HeapSlot<isAtomic>& HeapSlotAt(void* address)
{
    return *reinterpret_cast<HeapSlot<isAtomic>*>(address);
}

template<bool isAtomic = false, typename T,
         typename std::enable_if<
             IsSlotStorageType<typename std::remove_cv<T>::type>::value ||
             std::is_convertible<T*, const BaseObject*>::value,
             int>::type = 0>
HeapSlot<isAtomic>& HeapSlotAt(T*) = delete;

template<bool isAtomic = false>
inline HeapSlot<isAtomic>& HeapSlotAt(MAddress address)
{
    return HeapSlotAt<isAtomic>(reinterpret_cast<void*>(address));
}

inline RootSlot& RootSlotAt(void* address)
{
    return *reinterpret_cast<RootSlot*>(address);
}

template<typename T,
         typename std::enable_if<
             IsSlotStorageType<typename std::remove_cv<T>::type>::value ||
             std::is_convertible<T*, const BaseObject*>::value,
             int>::type = 0>
RootSlot& RootSlotAt(T*) = delete;

inline RootSlot& RootSlotAt(MAddress address)
{
    return RootSlotAt(reinterpret_cast<void*>(address));
}

inline DerivedSlot& DerivedSlotAt(void* address)
{
    return *reinterpret_cast<DerivedSlot*>(address);
}

template<typename T,
         typename std::enable_if<
             IsSlotStorageType<typename std::remove_cv<T>::type>::value ||
             std::is_convertible<T*, const BaseObject*>::value,
             int>::type = 0>
DerivedSlot& DerivedSlotAt(T*) = delete;

inline DerivedSlot& DerivedSlotAt(MAddress address)
{
    return DerivedSlotAt(reinterpret_cast<void*>(address));
}

using HeapSlotVisitor = std::function<void(HeapSlot<>&)>;
using RootSlotVisitor = std::function<void(RootSlot&)>;
using RefFieldVisitor = HeapSlotVisitor;
} // namespace MapleRuntime
#endif // MRT_REF_FIELD_H
