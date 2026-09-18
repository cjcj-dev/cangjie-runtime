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
#include "Heap/z/zAddress.hpp"
#include "Common/ColourEncoding.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Common/TypeDef.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

namespace MapleRuntime {
#ifdef __arm__
#define ARM32_MARKED_FLAG_BITS  2
#endif
class BaseObject;
class CopyCollector;

void AssertBarrierTransitionMonotonicity(zpointer oldPtr, zpointer newPtr);

template<bool isAtomic>
class HeapSlot;

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

    bool CompareExchange(zpointer expectedValue, zpointer newValue,
                         std::memory_order succOrder = std::memory_order_relaxed,
                         std::memory_order failOrder = std::memory_order_relaxed,
                         zpointer* observedOut = nullptr)
    {
        MAddress expectedRaw = raw(expectedValue);
        MAddress newRaw = raw(newValue);
        CHECK(std::numeric_limits<MAddress>::max() > newRaw);
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
        const zpointer pointer = static_cast<zpointer>(fieldVal);
        if (is_null_any(pointer)) {
            return raw(ZPointer::uncolor(pointer));
        }
        if (ZPointer::is_store_bad(pointer)) {
            return raw(ZPointer::uncolor_unsafe(pointer));
        }
        return raw(ZPointer::uncolor_store_good(pointer));
    }

    ~HeapSlot() = default;
    explicit HeapSlot(MAddress val) : fieldVal(val) {}
    // 凭什么: zpointer 就是槽里的带色位模式，与 MAddress 同宽。
    explicit HeapSlot(zpointer val) : fieldVal(static_cast<RefFieldValue>(raw(val))) {}
    HeapSlot(const HeapSlot& ref) : fieldVal(ref.fieldVal) {}
    HeapSlot(const BaseObject* obj, MAddress colour)
        : fieldVal(raw(ZAddress::color(from_object(obj), colour))) {}

    HeapSlot(HeapSlot&& ref) : fieldVal(ref.fieldVal) {}
    HeapSlot() = delete;
    HeapSlot& operator=(const HeapSlot&) = delete;
    HeapSlot& operator=(const HeapSlot&&) = delete;

private:
    // heapdesired: plain BaseObject* carrier is not a public heap-CAS desired.
    // Only CopyCollector (GetAndTryTagRefField / RootSlotWriteback plain-root arm /
    // null install) may mint it. Outside code that needs a plain value must say
    // so via zpointer/MAddress or the colour-carrying constructors above —
    // RefField<>(obj) as CompareExchange desired is a compile error.
    explicit HeapSlot(const BaseObject* obj)
        : fieldVal(raw(ZAddress::store_good(from_object(obj)))) {}
    friend     friend class CopyCollector;
    using RefFieldValue = MAddress;
    RefFieldValue fieldVal;
};

template<bool isAtomic = false>
inline void StoreColoured(HeapSlot<isAtomic>& slot, zaddress value, MAddress colour,
                          std::memory_order order = std::memory_order_relaxed)
{
    zpointer coloured = to_zpointer(raw(value) | colour);
    slot.StoreColoured(coloured, order);
}

// Compatibility spelling for code outside the runtime. It denotes HeapSlot only;
// roots and derived locations are different, non-convertible types below.
template<bool isAtomic = false>
using RefField = HeapSlot<isAtomic>;

// ZBarrierSet native access uses the same zpointer representation as heap fields.
// RootSlot below is reserved for uncolored roots owned by the shared root protocol.
using NativeSlot = HeapSlot<false>;
inline NativeSlot& NativeSlotAt(void* address) { return *reinterpret_cast<NativeSlot*>(address); }
inline NativeSlot& NativeSlotAt(MAddress address) { return *reinterpret_cast<NativeSlot*>(address); }
using NativeSlotVisitor = std::function<void(NativeSlot&)>;

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
    zaddress_unsafe rootValue;

    friend void StorePlain(RootSlot&, zaddress, std::memory_order);
    friend     friend class CopyCollector;
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
                                       BaseObject* host, size_t offset)
{
    MAddress address = reinterpret_cast<MAddress>(host) + offset;
    return field.CompareExchange(expected,
                    to_zpointer(raw(ZAddress::store_good(to_zaddress(address)))));
}

// When the host is unknown, preserve the interior payload but still publish a
// complete StoreGood word.
// Use the (host, offset) overload for a metadata-provided derived base.
template<bool isAtomic = false>
inline bool CasInstallInteriorColoured(HeapSlot<isAtomic>& field, zpointer expected,
                                       BaseObject* interior)
{
    MAddress address = reinterpret_cast<MAddress>(interior);
    return field.CompareExchange(expected,
                    to_zpointer(raw(ZAddress::store_good(to_zaddress(address)))));
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
