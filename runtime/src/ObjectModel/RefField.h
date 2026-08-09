// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_REF_FIELD_H
#define MRT_REF_FIELD_H

#include <atomic>
#include <cstdlib>
#include <functional>
#include <limits>

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

    zpointer Exchange(zpointer newRef, std::memory_order order = std::memory_order_relaxed)
    {
        CHECK(fieldVal < std::numeric_limits<RefFieldValue>::max());
        MAddress newRaw = raw(newRef);
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
    explicit HeapSlot(const BaseObject* obj) : fieldVal(0)
    {
#ifdef __arm__
        address = reinterpret_cast<MAddress>(obj) >> ARM32_MARKED_FLAG_BITS;
#else
        address = reinterpret_cast<MAddress>(obj);
#endif
    }
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
          padding(0)
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

    zaddress_unsafe rootValue;

    friend void StorePlain(RootSlot&, zaddress, std::memory_order);
    friend void HealRoot(RootSlot&, zaddress, std::memory_order);
};

inline void StorePlain(RootSlot& slot, zaddress value,
                       std::memory_order order = std::memory_order_relaxed)
{
    slot.StorePlain(value, order);
}

inline void HealRoot(RootSlot& slot, zaddress good,
                     std::memory_order order = std::memory_order_relaxed)
{
    slot.StorePlain(good, order);
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
                                    BaseObject* host, size_t offset)
{
    MAddress plainVal = reinterpret_cast<MAddress>(host) + offset;
    return field.CompareExchange(expected, to_zpointer(plainVal));
}

// When the host is unknown, still install a plain interior address (same 03fc21ed rule).
// Prefer the (host, offset) overload when TryRecoverInteriorBase succeeds.
template<bool isAtomic = false>
inline bool CasInstallInteriorPlain(HeapSlot<isAtomic>& field, zpointer expected,
                                    BaseObject* interior)
{
    MAddress plainVal = reinterpret_cast<MAddress>(interior);
    return field.CompareExchange(expected, to_zpointer(plainVal));
}

static_assert(sizeof(HeapSlot<>) == sizeof(MAddress), "HeapSlot must remain one machine word");
static_assert(sizeof(RootSlot) == sizeof(MAddress), "RootSlot must remain one machine word");
static_assert(sizeof(DerivedSlot) == sizeof(MAddress), "DerivedSlot must remain one machine word");

// The compiler/stack-map/object-layout ABIs expose raw word addresses. Keep the
// unavoidable representation escape in this single named layer; callers must
// already know the slot category from metadata or the ABI being decoded.
template<bool isAtomic = false>
inline HeapSlot<isAtomic>& HeapSlotAt(void* address)
{
    return *reinterpret_cast<HeapSlot<isAtomic>*>(address);
}

template<bool isAtomic = false>
inline HeapSlot<isAtomic>& HeapSlotAt(MAddress address)
{
    return HeapSlotAt<isAtomic>(reinterpret_cast<void*>(address));
}

inline RootSlot& RootSlotAt(void* address)
{
    return *reinterpret_cast<RootSlot*>(address);
}

inline RootSlot& RootSlotAt(MAddress address)
{
    return RootSlotAt(reinterpret_cast<void*>(address));
}

inline DerivedSlot& DerivedSlotAt(void* address)
{
    return *reinterpret_cast<DerivedSlot*>(address);
}

inline DerivedSlot& DerivedSlotAt(MAddress address)
{
    return DerivedSlotAt(reinterpret_cast<void*>(address));
}

using HeapSlotVisitor = std::function<void(HeapSlot<>&)>;
using RootSlotVisitor = std::function<void(RootSlot&)>;
using RefFieldVisitor = HeapSlotVisitor;
} // namespace MapleRuntime
#endif // MRT_REF_FIELD_H
