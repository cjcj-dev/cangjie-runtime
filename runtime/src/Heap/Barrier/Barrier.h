// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_BARRIER_H
#define MRT_BARRIER_H

#include "Common/BaseObject.h"
#include "Heap/Barrier/RememberedSet.h"
#include "ObjectModel/Field.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
class Collector;
// Barrier is the base class to define read/write barriers.
class Barrier {
public:
    Barrier(Collector& collector, RememberedSet& rememberedSet)
        : theCollector(collector), theRememberedSet(rememberedSet) {}
    virtual ~Barrier() {}

    // barriers for maple runtime. they can be override as needed.
    virtual void WriteI8(BaseObject* obj, Field<int8_t>& field, int8_t val) const;
    virtual void WriteI16(BaseObject* obj, Field<int16_t>& field, int16_t val) const;
    virtual void WriteI32(BaseObject* obj, Field<int32_t>& field, int32_t val) const;
    virtual void WriteI64(BaseObject* obj, Field<int64_t>& field, int64_t val) const;
    virtual void WriteF32(BaseObject* obj, Field<float>& field, float val) const;
    virtual void WriteF64(BaseObject* obj, Field<double>& field, double val) const;

    virtual BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const;
    virtual BaseObject* ReadStaticRef(ReadOnlyRootSlot& field) const;
    virtual BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const;
    virtual void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const;
    virtual void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    virtual void WriteStaticRef(RootSlot& field, BaseObject* ref) const;
    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
    virtual void WriteStaticStruct(MAddress dst, size_t dstLen, MAddress src, size_t srcLen, const GCTib gctib) const;

    void CopyRefArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                      BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;
    void CopyStructArray(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                         BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;

    virtual BaseObject* AtomicReadReference(BaseObject* obj, RefField<true>& field,
                                            MemoryOrder order) const;

    void AtomicWriteReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const;
    BaseObject* AtomicSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* ref, MemoryOrder order) const;
    bool CompareAndSwapReference(BaseObject* obj, RefField<true>& field, BaseObject* oldRef, BaseObject* newRef,
                                 MemoryOrder succOrder, MemoryOrder failOrder) const;

    // helper for delegation
    template<typename T>
    inline void WriteField(BaseObject* obj, Field<T>& field, T val) const;

    void WriteGeneric(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const;
    void ReadGeneric(const ObjectPtr dstPtr, ObjectPtr obj, void* fieldPtr, size_t size) const;

#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
    static void ResetGenerationalBarrierProbe();
    static uint64_t GetGenerationalBarrierFastPathHits();
    static uint64_t GetGenerationalBarrierRegionLookups();
#endif

protected:
    virtual void WriteReferenceImpl(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    virtual void WriteStructImpl(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
    virtual void CopyRefArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                  BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;
    virtual void CopyStructArrayImpl(BaseObject* dstObj, MAddress dstField, MIndex dstSize,
                                     BaseObject* srcObj, MAddress srcField, MIndex srcSize) const;
    virtual void AtomicWriteReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                          MemoryOrder order) const;
    virtual BaseObject* AtomicSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* ref,
                                                MemoryOrder order) const;
    virtual bool CompareAndSwapReferenceImpl(BaseObject* obj, RefField<true>& field, BaseObject* oldRef,
                                             BaseObject* newRef, MemoryOrder succOrder, MemoryOrder failOrder) const;
    virtual void WriteGenericImpl(const ObjectPtr obj, void* fieldPtr, const ObjectPtr src, size_t size) const;
    virtual void ReadGenericImpl(const ObjectPtr dstPtr, ObjectPtr obj, void* fieldPtr, size_t size) const;

    class LocalRefFieldContainer {
    public:
        // multi-thread unsafe.
        void Push(RefField<>* ref)
        {
            if (size >= CACHE_CAPACITY) {
                excessive.push_back(ref);
            } else {
                cache[size] = ref;
            }
            size++;
        }
        void VisitRefField(const RefFieldVisitor &visitor)
        {
            size_t cacheSize = size < CACHE_CAPACITY ? size : CACHE_CAPACITY;
            for (size_t i = 0; i != cacheSize; ++i) {
                visitor(*cache[i]);
            }
            for (auto* ref : excessive) {
                visitor(*ref);
            }
        }
    private:
        static constexpr size_t CACHE_CAPACITY = 10;
        RefField<>* cache[CACHE_CAPACITY]{ nullptr };
        size_t size{ 0 };
        std::vector<RefField<>*> excessive;
    };
    Collector& theCollector;

protected:
    // STACK_ROOTS_STAY_PLAIN: non-heap ReadStruct/ReadStaticStruct dst ref slots
    // must receive plain addresses (StorePlain), never coloured self-heal.
    // Heap-src fields still heal via ReadReference; only the stack/root copy is plain.
    void FixupNonHeapStructRefs(MAddress dst, BaseObject* srcObj, MAddress src, size_t size) const;
    void FixupNonHeapStaticStructRefs(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;

    // Shared post-copy fixup for every WriteStaticStruct phase override: resolve forwarding,
    // store plain. Typed on RootSlot so a coloured write cannot be spelled (see Barrier.cpp).
    void ResolveStaticStructRoots(MAddress dst, const GCTib gctib) const;

    // obj may be null for static/global fields (source treated as old).
    void RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const;
    // storecov: optional pre-store snapshot of store-good (addr,target) pairs; nullptr = always Record
    // (ReadGeneric / legacy callers). Defined in Barrier.cpp.
    struct StoreGoodPrevSnapshot;
    void RecordCrossGenEdgesInStruct(BaseObject* obj, MAddress start, size_t size,
                                     const StoreGoodPrevSnapshot* prevSnap = nullptr) const;
    void RecordCrossGenEdgesInRefArray(BaseObject* obj, MAddress start, size_t size,
                                       const StoreGoodPrevSnapshot* prevSnap = nullptr) const;
    void RecordStaticCrossGenEdges(MAddress start, const GCTib gctib) const;

private:
    RememberedSet& theRememberedSet;
};
} // namespace MapleRuntime
#endif // ~MRT_BARRIER_H
