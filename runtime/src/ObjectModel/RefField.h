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
class RefField {
public:
    // size in bytes
    static constexpr size_t GetSize() { return sizeof(fieldVal); }

    BaseObject* GetTargetObject(std::memory_order order = std::memory_order_relaxed) const
    {
        // Always atomic: mutator plain path races with concurrent GC mark/CAS (R1/R3).
        // relaxed keeps cost near a plain load on x86_64/aarch64 while establishing HB.
#if defined(CANGJIE_TSAN_SUPPORT)
        MAddress value = static_cast<MAddress>(Sanitizer::TsanAtomicLoad(&fieldVal, order));
#else
        MAddress value = __atomic_load_n(&fieldVal, order);
#endif
        return reinterpret_cast<BaseObject*>(RefField<>(value).GetAddress());
    }

    MAddress GetFieldValue(std::memory_order order = std::memory_order_relaxed) const
    {
#if defined(CANGJIE_TSAN_SUPPORT)
        return static_cast<MAddress>(Sanitizer::TsanAtomicLoad(&fieldVal, order));
#else
        return __atomic_load_n(&fieldVal, order);
#endif
    }

    void SetTargetObject(const BaseObject* obj, std::memory_order order = std::memory_order_relaxed);
    void SetFieldValue(MAddress value, std::memory_order order = std::memory_order_relaxed);

    bool CompareExchange(MAddress expectedValue, MAddress newValue,
                         std::memory_order succOrder = std::memory_order_relaxed,
                         std::memory_order failOrder = std::memory_order_relaxed)
    {
        CHECK(std::numeric_limits<MAddress>::max() > newValue);
        AssertColouredWriteIfEnabled(this, newValue);
#if defined(CANGJIE_TSAN_SUPPORT)
        // tsan will get expectedValue's address for us, just pass the real value
        auto ret = Sanitizer::TsanAtomicCompareExchange(&fieldVal, expectedValue, newValue, succOrder, failOrder);
        return (ret == expectedValue);
#else
        return __atomic_compare_exchange(&fieldVal, &expectedValue, &newValue, false, succOrder, failOrder);
#endif
    }

    bool CompareExchange(const BaseObject* expectedObj, const BaseObject* newObj,
                         std::memory_order succOrder = std::memory_order_relaxed,
                         std::memory_order failOrder = std::memory_order_relaxed)
    {
        return CompareExchange(reinterpret_cast<MAddress>(expectedObj), reinterpret_cast<MAddress>(newObj), succOrder,
                               failOrder);
    }

    MAddress Exchange(MAddress newRef, std::memory_order order = std::memory_order_relaxed)
    {
        CHECK(fieldVal < std::numeric_limits<RefFieldValue>::max());
        AssertColouredWriteIfEnabled(this, newRef);
        MAddress ret = 0;
#if defined(CANGJIE_TSAN_SUPPORT)
        ret = Sanitizer::TsanAtomicExchange(&fieldVal, newRef, order);
#else
        __atomic_exchange(&fieldVal, &newRef, &ret, order);
#endif
        return static_cast<MAddress>(ret);
    }

    MAddress Exchange(const BaseObject* obj, std::memory_order order = std::memory_order_relaxed)
    {
        return Exchange(reinterpret_cast<MAddress>(obj), order);
    }

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

    ~RefField() = default;
    explicit RefField(MAddress val) : fieldVal(val) {}
    RefField(const RefField& ref) : fieldVal(ref.fieldVal) {}
    explicit RefField(const BaseObject* obj) : fieldVal(0)
    {
#ifdef __arm__
        address = reinterpret_cast<MAddress>(obj) >> ARM32_MARKED_FLAG_BITS;
#else
        address = reinterpret_cast<MAddress>(obj);
#endif
    }
#ifdef __arm__
    RefField(const BaseObject* obj, uint16_t tagged, uint16_t tagid) : isTagged(tagged), tagID(tagid)
    {
        address = reinterpret_cast<MAddress>(obj) >> ARM32_MARKED_FLAG_BITS;
    }
#else
    // Phase C: colour-carrying form. The three-argument form below keeps working and leaves the
    // colour clear, which reads as "never written" -- see Collector::IsLoadBad.
    RefField(const BaseObject* obj, uint16_t tagged, uint16_t tagid, MAddress colour)
        : address(reinterpret_cast<MAddress>(obj)), isTagged(tagged), tagID(tagid),
          remapColour((colour >> REMAP_COLOUR_SHIFT) & ((MAddress(1) << REMAP_COLOUR_BITS) - 1)),
          markedYoung((colour >> MARKED_YOUNG_SHIFT) & ((MAddress(1) << MARKED_YOUNG_BITS) - 1)),
          markedOld((colour >> MARKED_OLD_SHIFT) & ((MAddress(1) << MARKED_OLD_BITS) - 1)),
          padding(0)
    {
        CHECK(tagid < TAG_ID_COUNT);
    }

    RefField(const BaseObject* obj, uint16_t tagged, uint16_t tagid)
        : address(reinterpret_cast<MAddress>(obj)), isTagged(tagged), tagID(tagid), remapColour(0),
          markedYoung(0), markedOld(0), padding(0)
    {
        // Was a silent bitfield truncate when tagID was 1 bit; diagnose out-of-range writes.
        CHECK(tagid < TAG_ID_COUNT);
    }
#endif

    RefField(RefField&& ref) : fieldVal(ref.fieldVal) {}
    RefField() = delete;
    RefField& operator=(const RefField&) = delete;
    RefField& operator=(const RefField&&) = delete;

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
            // Phase C: one-hot remap colour (TypeDef.h). address stays at bits 0..47, so the
            // compiler -- which only ANDs against a mask -- is unaffected by the encoding.
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
                  "RefField tag layout must fill 64 bits");
    static_assert(TAG_ID_COUNT > 1 && TAG_ID_COUNT <= (1u << TAG_ID_BITS), "TAG_ID_COUNT out of bit width");
#endif
};

using RefFieldVisitor = std::function<void(RefField<>&)>;
} // namespace MapleRuntime
#endif // MRT_REF_FIELD_H
