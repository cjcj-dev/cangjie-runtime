#ifndef MRT_Z_BARRIER_SET_INLINE_HPP
#define MRT_Z_BARRIER_SET_INLINE_HPP

#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zBarrierSet.hpp"
#include "ObjectModel/MArray.h"
#include <algorithm>

namespace MapleRuntime {
namespace AccessBarrierSupport {
template<DecoratorSet decorators>
inline DecoratorSet resolve_possibly_unknown_oop_ref_strength(BaseObject* base, ptrdiff_t offset)
{
    if constexpr ((decorators & ON_UNKNOWN_OOP_REF) == 0) { return decorators; }
    const bool weak = base != nullptr && Heap::IsHeapAddress(base) && base->IsWeakRef() && offset == TYPEINFO_PTR_SIZE;
    return (decorators & ~ON_UNKNOWN_OOP_REF) | (weak ? ON_WEAK_OOP_REF : ON_STRONG_OOP_REF);
}
}

template<DecoratorSet decorators>
inline bool is_store_barrier_no_keep_alive()
{
    if constexpr (decorators & ON_STRONG_OOP_REF) { return (decorators & AS_NO_KEEPALIVE) != 0; }
    return true;
}

template<DecoratorSet decorators>
inline bool is_store_barrier_no_keep_alive(BaseObject* base, ptrdiff_t offset)
{
    if constexpr ((decorators & ON_UNKNOWN_OOP_REF) == 0) { return is_store_barrier_no_keep_alive<decorators>(); }
    const DecoratorSet strength = AccessBarrierSupport::resolve_possibly_unknown_oop_ref_strength<decorators>(base, offset);
    return (strength & ON_STRONG_OOP_REF) == 0 || (decorators & AS_NO_KEEPALIVE) != 0;
}

template<DecoratorSet decorators, typename BarrierSetT>
template<DecoratorSet expected>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::verify_decorators_present()
{
    static_assert((decorators & expected) != 0, "required access decorator absent");
}

template<DecoratorSet decorators, typename BarrierSetT>
template<DecoratorSet expected>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::verify_decorators_absent()
{
    static_assert((decorators & expected) == 0, "unsupported access decorator present");
}

template<DecoratorSet decorators, typename BarrierSetT>
inline volatile zpointer* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::field_addr(BaseObject* base, ptrdiff_t offset)
{
    return reinterpret_cast<volatile zpointer*>(reinterpret_cast<uintptr_t>(base) + offset);
}

template<DecoratorSet decorators, typename BarrierSetT>
inline zaddress ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::load_barrier(volatile zpointer* p, zpointer observed)
{
    verify_decorators_absent<ON_UNKNOWN_OOP_REF>();
    if constexpr (decorators & AS_NO_KEEPALIVE) {
        if constexpr (decorators & ON_STRONG_OOP_REF) {
            return ZBarrier::load_barrier_on_oop_field_preloaded(p, observed);
        } else if constexpr (decorators & ON_WEAK_OOP_REF) {
            return ZBarrier::no_keep_alive_load_barrier_on_weak_oop_field_preloaded(p, observed);
        } else {
            verify_decorators_present<ON_PHANTOM_OOP_REF>();
            return ZBarrier::no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(p, observed);
        }
    } else {
        if constexpr (decorators & ON_STRONG_OOP_REF) {
            return ZBarrier::load_barrier_on_oop_field_preloaded(p, observed);
        } else if constexpr (decorators & ON_WEAK_OOP_REF) {
            return ZBarrier::load_barrier_on_weak_oop_field_preloaded(p, observed);
        } else {
            verify_decorators_present<ON_PHANTOM_OOP_REF>();
            return ZBarrier::load_barrier_on_phantom_oop_field_preloaded(p, observed);
        }
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline zaddress ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::load_barrier_on_unknown_oop_ref(BaseObject* base, ptrdiff_t offset, volatile zpointer* p, zpointer observed)
{
    verify_decorators_present<ON_UNKNOWN_OOP_REF>();
    const DecoratorSet strength = AccessBarrierSupport::resolve_possibly_unknown_oop_ref_strength<decorators>(base, offset);
    if (strength & ON_STRONG_OOP_REF) { return ZBarrier::load_barrier_on_oop_field_preloaded(p, observed); }
    if constexpr (decorators & AS_NO_KEEPALIVE) {
        if (strength & ON_WEAK_OOP_REF) { return ZBarrier::no_keep_alive_load_barrier_on_weak_oop_field_preloaded(p, observed); }
        return ZBarrier::no_keep_alive_load_barrier_on_phantom_oop_field_preloaded(p, observed);
    } else {
        if (strength & ON_WEAK_OOP_REF) { return ZBarrier::load_barrier_on_weak_oop_field_preloaded(p, observed); }
        return ZBarrier::load_barrier_on_phantom_oop_field_preloaded(p, observed);
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::store_barrier_heap_with_healing(volatile zpointer* p)
{
    if constexpr ((decorators & IS_DEST_UNINITIALIZED) == 0) {
        ZBarrier::store_barrier_on_heap_oop_field(p, true);
    } else {
        static_assert((decorators & IS_DEST_UNINITIALIZED) == 0, "healing requires initialized storage");
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::store_barrier_heap_without_healing(volatile zpointer* p)
{
    if constexpr ((decorators & IS_DEST_UNINITIALIZED) == 0) {
        ZBarrier::store_barrier_on_heap_oop_field(p, false);
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::store_barrier_native_with_healing(volatile zpointer* p)
{
    if constexpr ((decorators & IS_DEST_UNINITIALIZED) == 0) {
        ZBarrier::store_barrier_on_native_oop_field(p, true);
    } else {
        static_assert((decorators & IS_DEST_UNINITIALIZED) == 0, "healing requires initialized storage");
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::store_barrier_native_without_healing(volatile zpointer* p)
{
    if constexpr ((decorators & IS_DEST_UNINITIALIZED) == 0) {
        ZBarrier::store_barrier_on_native_oop_field(p, false);
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::no_keep_alive_store_barrier_heap(volatile zpointer* p)
{
    if constexpr ((decorators & IS_DEST_UNINITIALIZED) == 0) {
        ZBarrier::no_keep_alive_store_barrier_on_heap_oop_field(p);
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_load_in_heap(volatile zpointer* p)
{
    verify_decorators_absent<ON_UNKNOWN_OOP_REF>();
    const zpointer observed = Raw::load_in_heap(p);
    assert_is_valid(observed);
    return to_object(load_barrier(p, observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_store_in_heap(volatile zpointer* p, BaseObject* value)
{
    verify_decorators_absent<ON_UNKNOWN_OOP_REF>();
    if (is_store_barrier_no_keep_alive<decorators>()) {
        no_keep_alive_store_barrier_heap(p);
    } else {
        store_barrier_heap_without_healing(p);
    }
    Raw::store_in_heap(p, ZAddress::store_good(from_object(value)));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_cmpxchg_in_heap(volatile zpointer* p, BaseObject* compare, BaseObject* value)
{
    verify_decorators_present<ON_STRONG_OOP_REF>();
    verify_decorators_absent<AS_NO_KEEPALIVE>();
    store_barrier_heap_with_healing(p);
    const zpointer observed = Raw::atomic_cmpxchg_in_heap(p, ZAddress::store_good(from_object(compare)), ZAddress::store_good(from_object(value)));
    assert_is_valid(observed);
    return to_object(ZPointer::uncolor_store_good(observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_xchg_in_heap(volatile zpointer* p, BaseObject* value)
{
    verify_decorators_present<ON_STRONG_OOP_REF>();
    verify_decorators_absent<AS_NO_KEEPALIVE>();
    store_barrier_heap_with_healing(p);
    const zpointer observed = Raw::atomic_xchg_in_heap(p, ZAddress::store_good(from_object(value)));
    assert_is_valid(observed);
    return to_object(ZPointer::uncolor_store_good(observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_load_not_in_heap(volatile zpointer* p)
{
    verify_decorators_absent<ON_UNKNOWN_OOP_REF>();
    const zpointer observed = Raw::load(p);
    assert_is_valid(observed);
    return to_object(load_barrier(p, observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_store_not_in_heap(volatile zpointer* p, BaseObject* value)
{
    verify_decorators_absent<ON_UNKNOWN_OOP_REF>();
    if (!is_store_barrier_no_keep_alive<decorators>()) { store_barrier_native_without_healing(p); }
    Raw::store(p, ZAddress::store_good(from_object(value)));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_cmpxchg_not_in_heap(volatile zpointer* p, BaseObject* compare, BaseObject* value)
{
    verify_decorators_present<ON_STRONG_OOP_REF>();
    verify_decorators_absent<AS_NO_KEEPALIVE>();
    store_barrier_native_with_healing(p);
    const zpointer observed = Raw::atomic_cmpxchg(p, ZAddress::store_good(from_object(compare)), ZAddress::store_good(from_object(value)));
    assert_is_valid(observed);
    return to_object(ZPointer::uncolor_store_good(observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_xchg_not_in_heap(volatile zpointer* p, BaseObject* value)
{
    verify_decorators_present<ON_STRONG_OOP_REF>();
    verify_decorators_absent<AS_NO_KEEPALIVE>();
    store_barrier_native_with_healing(p);
    const zpointer observed = Raw::atomic_xchg(p, ZAddress::store_good(from_object(value)));
    assert_is_valid(observed);
    return to_object(ZPointer::uncolor_store_good(observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_load_in_heap_at(BaseObject* base, ptrdiff_t offset)
{
    volatile zpointer* const p = field_addr(base, offset);
    const zpointer observed = Raw::load_in_heap(p);
    assert_is_valid(observed);
    if constexpr (decorators & ON_UNKNOWN_OOP_REF) {
        return to_object(load_barrier_on_unknown_oop_ref(base, offset, p, observed));
    } else {
        return to_object(load_barrier(p, observed));
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_store_in_heap_at(BaseObject* base, ptrdiff_t offset, BaseObject* value)
{
    volatile zpointer* const p = field_addr(base, offset);
    if (is_store_barrier_no_keep_alive<decorators>(base, offset)) {
        no_keep_alive_store_barrier_heap(p);
    } else {
        store_barrier_heap_without_healing(p);
    }
    Raw::store_in_heap(p, ZAddress::store_good(from_object(value)));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_cmpxchg_in_heap_at(BaseObject* base, ptrdiff_t offset, BaseObject* compare, BaseObject* value)
{
    verify_decorators_present<ON_STRONG_OOP_REF | ON_UNKNOWN_OOP_REF>();
    verify_decorators_absent<AS_NO_KEEPALIVE>();
    volatile zpointer* const p = field_addr(base, offset);
    store_barrier_heap_with_healing(p);
    const zpointer observed = Raw::atomic_cmpxchg_in_heap(p, ZAddress::store_good(from_object(compare)), ZAddress::store_good(from_object(value)));
    assert_is_valid(observed);
    return to_object(ZPointer::uncolor_store_good(observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline BaseObject* ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_xchg_in_heap_at(BaseObject* base, ptrdiff_t offset, BaseObject* value)
{
    verify_decorators_present<ON_STRONG_OOP_REF | ON_UNKNOWN_OOP_REF>();
    verify_decorators_absent<AS_NO_KEEPALIVE>();
    volatile zpointer* const p = field_addr(base, offset);
    store_barrier_heap_with_healing(p);
    const zpointer observed = Raw::atomic_xchg_in_heap(p, ZAddress::store_good(from_object(value)));
    assert_is_valid(observed);
    return to_object(ZPointer::uncolor_store_good(observed));
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_arraycopy_in_heap(zpointer* src, zpointer* dst, size_t length)
{
    oop_arraycopy_in_heap_no_check_cast(dst, src, length);
}

template<DecoratorSet decorators, typename BarrierSetT>
inline zaddress ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_copy_one_barriers(volatile zpointer* dst, volatile zpointer* src)
{
    store_barrier_heap_without_healing(dst);
    return ZBarrier::load_barrier_on_oop_field(src);
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_copy_one(volatile zpointer* dst, volatile zpointer* src)
{
    const zaddress obj = oop_copy_one_barriers(dst, src);
    *const_cast<zpointer*>(dst) = ZAddress::store_good(obj);
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_clear_one(volatile zpointer* dst)
{
    store_barrier_heap_without_healing(dst);
    *const_cast<zpointer*>(dst) = color_null();
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_arraycopy_in_heap_no_check_cast(
    zpointer* dst, zpointer* src, size_t length)
{
    // ZGC zBarrierSet.inline.hpp:395-424. These entry points permit overlap.
    if (src > dst) {
        for (const zpointer* const end = src + length; src < end; src++, dst++) {
            oop_copy_one(dst, src);
        }
        return;
    }
    if (src < dst) {
        const zpointer* const end = src;
        src += length - 1;
        dst += length - 1;
        for (; src >= end; src--, dst--) {
            oop_copy_one(dst, src);
        }
        return;
    }
    // src and dst are the same; nothing to do.
}
template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::struct_copy_one(MArray* layout, MAddress dst, MAddress src)
{
    const size_t stride = layout->GetElementSize();
    size_t cursor = 0;
    layout->GetComponentTypeInfo()->GetGCTib().ForEachBitmapWord(dst, [&](RefField<>& field) {
        const size_t offset = reinterpret_cast<MAddress>(&field) - dst;
        if (cursor < offset) {
            std::memmove(reinterpret_cast<void*>(dst + cursor), reinterpret_cast<void*>(src + cursor), offset - cursor);
        }
        oop_copy_one(reinterpret_cast<zpointer*>(dst + offset), reinterpret_cast<zpointer*>(src + offset));
        cursor = offset + sizeof(zpointer);
    });
    if (cursor < stride) {
        std::memmove(reinterpret_cast<void*>(dst + cursor), reinterpret_cast<void*>(src + cursor), stride - cursor);
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::struct_arraycopy_in_heap_no_check_cast(
    MArray* layout, MAddress dst, MAddress src, size_t length)
{
    // Same direction selection as oop_arraycopy, with the inline-value stride.
    const size_t stride = layout->GetElementSize();
    if (src > dst) {
        for (const MAddress end = src + length * stride; src < end; src += stride, dst += stride) {
            struct_copy_one(layout, dst, src);
        }
        return;
    }
    if (src < dst) {
        const MAddress end = src;
        src += (length - 1) * stride;
        dst += (length - 1) * stride;
        for (; src >= end; src -= stride, dst -= stride) {
            struct_copy_one(layout, dst, src);
        }
        return;
    }
}

// ZGC zBarrierSet.inline.hpp:473-519: primitive gap, oop_copy_one, trailer.
// Native and Uncolored are Cangjie headerless/global/stack value adaptations.
template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::value_copy_in_heap(
    const ValuePayload& src, const ValuePayload& dst)
{
    CHECK(src.size <= dst.size);
    const auto& offsets = src.offsets.empty() ? dst.offsets : src.offsets;
    size_t copied = 0;
    for (size_t offset : offsets) {
        if (offset >= src.size) { break; }
        CHECK(copied <= offset && offset + sizeof(zpointer) <= src.size);
        std::memmove(reinterpret_cast<void*>(dst.address + copied),
                     reinterpret_cast<void*>(src.address + copied), offset - copied);
        auto* const srcSlot = reinterpret_cast<volatile zpointer*>(src.address + offset);
        auto* const dstSlot = reinterpret_cast<volatile zpointer*>(dst.address + offset);
        if (src.kind == ValuePayload::Kind::Heap && dst.kind == ValuePayload::Kind::Heap) {
            oop_copy_one(dstSlot, srcSlot);
        } else {
            if (dst.kind == ValuePayload::Kind::Heap) { store_barrier_heap_without_healing(dstSlot); }
            else if (dst.kind == ValuePayload::Kind::Native) { store_barrier_native_without_healing(dstSlot); }
            const zaddress value = src.kind == ValuePayload::Kind::Uncolored ?
                safe(RootSlotAt(src.address + offset).LoadPlain()) :
                ZBarrier::load_barrier_on_oop_field(srcSlot);
            if (dst.kind == ValuePayload::Kind::Uncolored) { StorePlain(RootSlotAt(dst.address + offset), value); }
            else { Raw::store(dstSlot, ZAddress::store_good(value)); }
        }
        copied = offset + sizeof(zpointer);
    }
    std::memmove(reinterpret_cast<void*>(dst.address + copied),
                 reinterpret_cast<void*>(src.address + copied), src.size - copied);
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_arraycopy_in_heap(
    BaseObject* srcObj, MAddress src, size_t srcSize, BaseObject* dstObj, MAddress dst, size_t dstSize)
{
    const size_t length = std::min(srcSize, dstSize) / sizeof(zpointer);
    if (Heap::IsHeapAddress(src) && Heap::IsHeapAddress(dst)) {
        oop_arraycopy_in_heap(reinterpret_cast<zpointer*>(src), reinterpret_cast<zpointer*>(dst), length);
        return;
    }
    if (src == dst || length == 0) { return; }
    for (size_t i = 0; i < length; ++i) {
        const size_t offset = (src > dst ? i : length - i - 1) * sizeof(zpointer);
        ValuePayload source(src + offset, sizeof(zpointer));
        source.offsets.push_back(0);
        value_copy_in_heap(source, ValuePayload(dst + offset, sizeof(zpointer)));
    }
}

template<DecoratorSet decorators, typename BarrierSetT>
inline void ZBarrierSet::AccessBarrier<decorators, BarrierSetT>::value_arraycopy_in_heap(
    BaseObject* srcObj, MAddress src, size_t srcSize, BaseObject* dstObj, MAddress dst, size_t dstSize)
{
    CHECK(srcSize <= dstSize);
    auto* layout = static_cast<MArray*>(Heap::IsHeapAddress(dst) ? dstObj : srcObj);
    if (layout == nullptr) {
        std::memmove(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), srcSize);
        return;
    }
    const size_t stride = layout->GetElementSize();
    CHECK(stride != 0 && srcSize % stride == 0);
    if (Heap::IsHeapAddress(src) && Heap::IsHeapAddress(dst)) {
        struct_arraycopy_in_heap_no_check_cast(layout, dst, src, srcSize / stride);
        return;
    }
    if (src == dst || srcSize == 0) { return; }
    const size_t length = srcSize / stride;
    for (size_t i = 0; i < length; ++i) {
        const size_t offset = (src > dst ? i : length - i - 1) * stride;
        ValuePayload source(src + offset, stride, layout->GetComponentTypeInfo()->GetGCTib(),
            Heap::IsHeapAddress(src) ? ValuePayload::Kind::Heap : ValuePayload::Kind::Uncolored);
        value_copy_in_heap(source, ValuePayload(dst + offset, stride));
    }
}

} // namespace MapleRuntime
#endif
