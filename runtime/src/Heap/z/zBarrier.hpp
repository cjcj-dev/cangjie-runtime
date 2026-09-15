// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_BARRIER_H
#define MRT_BARRIER_H

#include "Common/BaseObject.h"
#include "Common/ColourEncoding.h"
#include "Heap/z/zRememberedSet.hpp"
#include "ObjectModel/Field.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
class Collector;
enum class ReferenceStrength : uint8_t { Strong, Weak, Phantom };
struct ForwardingProvenance;

// One color-predicate barrier implementation for runtime and compiler accesses.
class Barrier {
public:
    Barrier(Collector& collector, RememberedSet& rememberedSet)
        : theCollector(collector), theRememberedSet(rememberedSet) {}
    Barrier(const Barrier&) = delete;
    Barrier& operator=(const Barrier&) = delete;
    ~Barrier() = default;

    // One barrier implementation; colour, reference strength and slot kind select the path.
    void WriteI8(BaseObject* obj, Field<int8_t>& field, int8_t val) const;
    void WriteI16(BaseObject* obj, Field<int16_t>& field, int16_t val) const;
    void WriteI32(BaseObject* obj, Field<int32_t>& field, int32_t val) const;
    void WriteI64(BaseObject* obj, Field<int64_t>& field, int64_t val) const;
    void WriteF32(BaseObject* obj, Field<float>& field, float val) const;
    void WriteF64(BaseObject* obj, Field<double>& field, double val) const;

    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const;
    BaseObject* ReadStaticRef(NativeSlot& field) const;
    void MarkYoungGoodBarrierOnOopField(NativeSlot& field) const;
    BaseObject* ReadPlainRoot(RootSlot& field) const;
    BaseObject* ReadPhantomRef(BaseObject* obj, RefField<false>& field) const;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    // Preloaded compiler store entry carries the overwritten word. Store-good
    // proves its old-value and remembered-set obligations were already discharged.
    void PostWriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref, zpointer prev) const;
    void WriteStaticRef(NativeSlot& field, BaseObject* ref) const;
    void WritePlainRoot(RootSlot& field, BaseObject* ref) const;
    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
    void WriteStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, GCTib gctib) const;
    void ReadStruct(MAddress dst, MAddress src, size_t size, GCTib gctib) const;
    void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const;

    void CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                      BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;
    void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                         BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;

    BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field, MemoryOrder order) const;

    void AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const;
    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const;
    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const;

    // helper for delegation
    template<typename T>
    inline void WriteField(BaseObject* obj, Field<T>& field, T val) const;

    void WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const;
    void ReadGeneric(const ObjectPtr dstPtr, ObjectPtr obj, void* fieldPtr, size_t size) const;

protected:

    void WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    void WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
    void CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                          BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;
    void CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                             BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;
    void AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                  MemoryOrder order) const;
    BaseObject* AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                        MemoryOrder order) const;
    bool CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                     BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const;
    void WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const;
    void ReadGenericImpl(const ObjectPtr dstPtr, ObjectPtr obj, void* fieldPtr, size_t size) const;

    Collector& theCollector;

protected:
    // Copy to mutator-local uncolored storage: load colored source slots through
    // the barrier, then store plain values. Native static destinations use the
    // separate typed static entry and remain colored.
    void CopyStructPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, size_t size) const;
    void CopyStaticStructPlainToNonHeap(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;
    void CopyStructArrayPlainToNonHeap(MAddress dstField, BaseObject* srcObj, MAddress srcField, size_t srcSize) const;
    void CopyRefArrayPlainToNonHeap(MAddress dst, BaseObject* srcObj, MAddress src, MIndex dstSize, MIndex srcSize) const;

    // Full-colour inverse boundary: snapshot the source for overlap safety,
    // copy primitive gaps, and publish each heap reference slot directly with
    // its current colour. No memcpy-written plain window is permitted.
    __attribute__((visibility("hidden"))) void CopyObjectStructColouredToHeap(
        BaseObject* layoutObj, MAddress layoutStart, MAddress dst, size_t dstLen,
        MAddress src, size_t srcLen) const;
    __attribute__((visibility("hidden"))) void CopyStaticStructColouredToHeap(
        MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const;
    __attribute__((visibility("hidden"))) void CopyStructArrayColouredToHeap(
        BaseObject* dstObj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
    __attribute__((visibility("hidden"))) void CopyRefArrayColouredToHeap(
        MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;

    // obj may be null for static/global fields (source treated as old).
    void RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref,
                            zpointer prev = zpointer::null) const;
private:
    using MarkFastPath = bool (*)(zpointer);
    using MarkColor = zpointer (*)(zaddress, zpointer);
    template<typename SlowPath>
    zaddress MarkBarrier(MarkFastPath fast, SlowPath slow, MarkColor color,
                           NativeSlot& field, zpointer observed) const;
    static bool IsMarkYoungGoodFastPath(zpointer value);
    static zpointer ColorMarkYoungGood(zaddress address, zpointer previous);
    zaddress MarkYoungSlowPath(zaddress address) const;
    void MarkIfYoung(zaddress address) const;
    void MarkYoung(zaddress address) const;
    template<bool atomic>
    void NativeStoreBarrier(RefField<atomic>& field, bool heal) const;
    template<bool atomic>
    BaseObject* LoadBarrier(BaseObject* obj, RefField<atomic>& field, zpointer observed,
                            ReferenceStrength strength) const;
    template<bool atomic>
    void StoreBarrier(BaseObject* obj, RefField<atomic>& field, bool heal,
                      ReferenceStrength strength = ReferenceStrength::Strong) const;

    RememberedSet& theRememberedSet;
};
} // namespace MapleRuntime
#endif // ~MRT_BARRIER_H
