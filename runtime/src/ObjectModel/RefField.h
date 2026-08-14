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

#include <execinfo.h>

#include "Base/Log.h"
#include "Common/ColourMask.h"
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

// Every heap/root healing write names its owning algorithm.  This is deliberately
// exhaustive: a catch-all value would recreate the attribution gap HealSlot closes.
enum class HealSite : uint16_t {
    BaseObjectCompareExchangeRefField,
    BarrierCompareAndSwapReference,
    BarrierCopyRefArrayRecolour,
    BarrierCopyStructArrayRecolour,
    BarrierWriteStructRecolour,
    EnumCompareAndSwapReference,
    EnumCopyStructArrayRecolour,
    EnumReadReference,
    EnumWriteStructRecolour,
    ForwardAtomicReadReference,
    ForwardCompareAndSwapReference,
    ForwardCopyStructArrayRecolour,
    ForwardReadReference,
    IdleAtomicReadReference,
    IdleCompareAndSwapReference,
    IdleCopyStructArrayRecolour,
    IdleReadReference,
    IdleWriteStructRecolour,
    MutatorPreForwardInterior,
    MutatorPreForwardRoot,
    MutatorPreForwardStackField,
    MutatorStripRootColour,
    PlainCensusInject,
    PlainCensusRestore,
    PostTraceAtomicReadReference,
    PostTraceCompareAndSwapReference,
    PostTraceCopyStructArrayRecolour,
    PostTraceReadReference,
    PostTraceWriteStructRecolour,
    PreforwardAtomicReadReference,
    PreforwardCompareAndSwapReference,
    PreforwardCopyStructArrayRecolour,
    PreforwardReadReference,
    TraceCompareAndSwapReference,
    TraceCopyStructArrayRecolour,
    TraceReadReference,
    TraceWriteStructRecolour,
    TracingCollectorResurrectFinalizer,
    TracingCollectorTraceRefField,
    WCollectorEnumRawInteriorRoot,
    WCollectorEnumRawRoot,
    WCollectorEnumRefFieldRoot,
    WCollectorFixOldTaggedDead,
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
    WCollectorMinorResolveDead,
    WCollectorMinorResolveLoadGoodForward,
    WCollectorMinorResolveOldForward,
    WCollectorMinorResolveOldIdentity,
    WCollectorNormalizeOldRoot,
    WCollectorNormalizeRawRoot,
    WCollectorPreserveRawInterior,
    WCollectorPreserveRootInterior,
    WCollectorRemsetResolveDead,
    WCollectorResolveDeadRoot,
    WCollectorResolveRootLoadGoodForward,
    WCollectorResolveRootOldForward,
    WCollectorTraceRefField,
    WCollectorTryUntagRefField,
    WCollectorTryUpdateRefField,
};

enum class HealNull : uint8_t { Disallow, Allow };

template<bool isAtomic>
class HeapSlot;

template<bool isAtomic>
inline bool HealSlot(HeapSlot<isAtomic>& slot, zpointer expected, zpointer desired, HealSite site,
                     HealNull allowNull = HealNull::Disallow,
                     std::memory_order succOrder = std::memory_order_relaxed,
                     std::memory_order failOrder = std::memory_order_relaxed);

// COLOUR_WRITEBACK_AUDIT §六 判据 1：堆内非 null 写必须带色。定义在 RefField.inline.h /
// BaseObject.cpp；默认关（MRT_GCV2_ASSERT_COLOURED_WRITES=1 打开）。
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

    void StoreColoured(zpointer value, std::memory_order order = std::memory_order_relaxed);

private:
    template<bool atomic>
    friend bool HealSlot(HeapSlot<atomic>&, zpointer, zpointer, HealSite, HealNull,
                         std::memory_order, std::memory_order);

    bool CompareExchange(zpointer expectedValue, zpointer newValue,
                         std::memory_order succOrder = std::memory_order_relaxed,
                         std::memory_order failOrder = std::memory_order_relaxed)
    {
        MAddress expectedRaw = raw(expectedValue);
        MAddress newRaw = raw(newValue);
        CHECK(std::numeric_limits<MAddress>::max() > newRaw);
        AssertColouredWriteIfEnabled(this, newRaw);
#if defined(CANGJIE_TSAN_SUPPORT)
        // tsan will get expectedValue's address for us, just pass the real value
        auto ret = Sanitizer::TsanAtomicCompareExchange(&fieldVal, expectedRaw, newRaw, succOrder, failOrder);
        return (ret == expectedRaw);
#else
        return __atomic_compare_exchange(&fieldVal, &expectedRaw, &newRaw, false, succOrder, failOrder);
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

    // 地址位（已剥 isTagged/colour）。返回裸 MAddress 供布局/偏移算术；
    // 若要当对象指针，经 uncolor_bits(GetFieldValue()) 或 GetTargetObject()。
    MAddress GetAddress() const
    {
#ifdef __arm__
        return address << ARM32_MARKED_FLAG_BITS;
#else
        return address;
#endif
    }

    bool IsTagged() const { return isTagged == 1; }
    uint16_t GetTagID() const { return tagID; }

    ~HeapSlot() = default;
    explicit HeapSlot(MAddress val) : fieldVal(val) {}
    // 凭什么: zpointer 就是槽里的带色位模式，与 MAddress 同宽。
    explicit HeapSlot(zpointer val) : fieldVal(static_cast<RefFieldValue>(raw(val))) {}
    HeapSlot(const HeapSlot& ref) : fieldVal(ref.fieldVal) {}
#ifdef __arm__
    HeapSlot(const BaseObject* obj, uint16_t tagged, uint16_t tagid) : isTagged(tagged), tagID(tagid)
    {
        address = reinterpret_cast<MAddress>(obj) >> ARM32_MARKED_FLAG_BITS;
    }
#else
    // Phase C: colour-carrying form. The three-argument form below keeps working and leaves the
    // colour clear, which reads as "never written" -- see Collector::IsLoadBad.
    HeapSlot(const BaseObject* obj, uint16_t tagged, uint16_t tagid, MAddress colour)
        : address(reinterpret_cast<MAddress>(obj)), isTagged(tagged), tagID(tagid),
          remapColour((colour >> REMAP_COLOUR_SHIFT) & ((MAddress(1) << REMAP_COLOUR_BITS) - 1)),
          markedYoung((colour >> MARKED_YOUNG_SHIFT) & ((MAddress(1) << MARKED_YOUNG_BITS) - 1)),
          markedOld((colour >> MARKED_OLD_SHIFT) & ((MAddress(1) << MARKED_OLD_BITS) - 1)),
          // Remembered occupies low bits of padding (bits 58-59); spare stays 0.
          // OpenJDK zAddress.cpp:83 StoreGood = MarkGood | Remembered.
          padding((colour >> REMEMBERED_SHIFT) & ((MAddress(1) << TAG_ID_PADDING_BITS) - 1))
    {
        CHECK(tagid < TAG_ID_COUNT);
    }

    HeapSlot(const BaseObject* obj, uint16_t tagged, uint16_t tagid)
        : address(reinterpret_cast<MAddress>(obj)), isTagged(tagged), tagID(tagid), remapColour(0),
          markedYoung(0), markedOld(0), padding(0)
    {
        // Was a silent bitfield truncate when tagID was 1 bit; diagnose out-of-range writes.
        CHECK(tagid < TAG_ID_COUNT);
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
            MAddress isTagged : 1;
            MAddress tagID : 1;
            MAddress address : 30;
        };
        RefFieldValue fieldVal;
    };
#else
    union {
        struct {
            MAddress address : 48;
            MAddress isTagged : 1;
            MAddress tagID : TAG_ID_BITS;
            // Phase C: one-hot RemappedYoung x RemappedOld state. address stays at bits 0..47;
            // a compiler fast path must strip bits 48..63 before exposing a load-good address.
            MAddress remapColour : REMAP_COLOUR_BITS;
            MAddress markedYoung : MARKED_YOUNG_BITS;
            MAddress markedOld : MARKED_OLD_BITS;
            MAddress padding : TAG_ID_PADDING_BITS;
        };
        RefFieldValue fieldVal;
    };
    static_assert(48 + 1 + TAG_ID_BITS + REMAP_COLOUR_BITS + MARKED_YOUNG_BITS + MARKED_OLD_BITS +
                          TAG_ID_PADDING_BITS ==
                      64,
                  "HeapSlot tag layout must fill 64 bits");
    static_assert(TAG_ID_COUNT > 1 && TAG_ID_COUNT <= (1u << TAG_ID_BITS), "TAG_ID_COUNT out of bit width");
#endif
};

// The sole HeapSlot compare-exchange write.  Match ZBarrier::self_heal: a
// non-null observed reference must not be healed to null unless its owner makes
// that destructive transition explicit.
template<bool isAtomic>
inline bool HealSlot(HeapSlot<isAtomic>& slot, zpointer expected, zpointer desired, HealSite site,
                     HealNull allowNull, std::memory_order succOrder, std::memory_order failOrder)
{
    (void)site;
    if (allowNull == HealNull::Disallow && !is_null(expected) && is_null(desired)) {
        return false;
    }
    return slot.CompareExchange(expected, desired, succOrder, failOrder);
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

// staticnull: default-off probe for RootSlot plain stores that install null over
// a previously non-null root (MRT_GCV2_STATICNULL=1). Captures slot/old/new +
// short backtrace so concurrent TRACE-window zeroing of static roots is siteable.
// Product path unchanged when env unset.
inline bool StaticNullProbeEnabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_STATICNULL");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

inline void NoteRootSlotNullStore(void* slot, MAddress oldRaw, const char* via)
{
    static std::atomic<size_t> g_staticNullN{ 0 };
    size_t n = g_staticNullN.fetch_add(1, std::memory_order_relaxed);
    if (n >= 128) {
        return;
    }
    void* frames[16];
    int nf = ::backtrace(frames, 16);
    char** syms = nf > 0 ? ::backtrace_symbols(frames, nf) : nullptr;
    std::fprintf(stderr,
                 "[GCV2][staticnull] path=root_null_store n=%zu via=%s slot=%p old=%#zx new=0\n",
                 n, via, slot, static_cast<size_t>(oldRaw));
    if (syms != nullptr) {
        int limit = nf < 12 ? nf : 12;
        for (int i = 0; i < limit; ++i) {
            std::fprintf(stderr, "[GCV2][staticnull]   #%d %s\n", i, syms[i]);
        }
        std::free(syms);
    }
    std::fflush(stderr);
}

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
        if (StaticNullProbeEnabled() && is_null(value)) {
            zaddress_unsafe prev;
            __atomic_load(&rootValue, &prev, std::memory_order_relaxed);
            MAddress oldRaw = raw(prev);
            if (oldRaw != 0) {
                NoteRootSlotNullStore(static_cast<void*>(this), oldRaw, "StorePlain");
            }
        }
        zaddress_unsafe unsafeValue = to_zaddress_unsafe(raw(value));
        __atomic_store(&rootValue, &unsafeValue, order);
    }

    zaddress_unsafe rootValue;

    friend void StorePlain(RootSlot&, zaddress, std::memory_order);
    friend bool HealRoot(RootSlot&, zaddress, HealSite, HealNull, std::memory_order);
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

// Named HeapSlot write for a derived *value* (base+offset interior). The storage is still a
// HeapSlot (object field / remset), but the payload is not an object root and must stay plain
// (03fc21ed / interiorsrc2). DerivedSlot itself cannot CAS into HeapSlot storage — stackmap
// DerivedSlots use RebaseDerived; these HeapSlot sites keep CAS and express provenance via
// (host, offset) in the call. Do not colour the installed value.
template<bool isAtomic = false>
inline bool CasInstallInteriorPlain(HeapSlot<isAtomic>& field, zpointer expected,
                                    BaseObject* host, size_t offset, HealSite site)
{
    MAddress plainVal = reinterpret_cast<MAddress>(host) + offset;
    return HealSlot(field, expected, to_zpointer(plainVal), site);
}

// When the host is unknown, still install a plain interior address (same 03fc21ed rule).
// Prefer the (host, offset) overload when TryRecoverInteriorBase succeeds.
template<bool isAtomic = false>
inline bool CasInstallInteriorPlain(HeapSlot<isAtomic>& field, zpointer expected,
                                    BaseObject* interior, HealSite site)
{
    MAddress plainVal = reinterpret_cast<MAddress>(interior);
    return HealSlot(field, expected, to_zpointer(plainVal), site);
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
