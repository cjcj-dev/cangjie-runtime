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
enum class BarrierPhase : uint8_t {
    STW,
    IDLE,
    ENUM,
    TRACE,
    POST_TRACE,
    PREFORWARD,
    FORWARD,
};

// Barrier is the base class to define read/write barriers.
class Barrier {
public:
    Barrier(Collector& collector, RememberedSet& rememberedSet)
        : Barrier(collector, rememberedSet, BarrierPhase::STW) {}
    Barrier(const Barrier&) = delete;
    Barrier& operator=(const Barrier&) = delete;
    ~Barrier() = default;

    // Phase differences are selected explicitly from phase; Barrier has no vtable dispatch.
    void WriteI8(BaseObject* obj, Field<int8_t>& field, int8_t val) const;
    void WriteI16(BaseObject* obj, Field<int16_t>& field, int16_t val) const;
    void WriteI32(BaseObject* obj, Field<int32_t>& field, int32_t val) const;
    void WriteI64(BaseObject* obj, Field<int64_t>& field, int64_t val) const;
    void WriteF32(BaseObject* obj, Field<float>& field, float val) const;
    void WriteF64(BaseObject* obj, Field<double>& field, double val) const;

    BaseObject* ReadReference(BaseObject* obj, RefField<false>& field) const;
    BaseObject* ReadStaticRef(ReadOnlyRootSlot& field) const;
    BaseObject* ReadWeakRef(BaseObject* obj, RefField<false>& field) const;
    void ReadStruct(MAddress dst, BaseObject* obj, MAddress src, size_t size) const;
    void ReadStaticStruct(MAddress dst, MAddress src, size_t size, const GCTib gctib) const;

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const;
    void WriteStaticRef(RootSlot& field, BaseObject* ref) const;
    void WriteStruct(BaseObject* obj, MAddress dst, size_t dstLen, MAddress src, size_t srcLen) const;
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

#if defined(MRT_GENERATIONAL_BARRIER_PROBE)
    static void ResetGenerationalBarrierProbe();
    static uint64_t GetGenerationalBarrierFastPathHits();
    static uint64_t GetGenerationalBarrierRegionLookups();
#endif

protected:
    Barrier(Collector& collector, RememberedSet& rememberedSet, BarrierPhase phase)
        : theCollector(collector), theRememberedSet(rememberedSet), phase(phase) {}

    void WriteStaticRefPlain(RootSlot& field, BaseObject* ref) const;
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

    // Shared post-copy fixup for every WriteStaticStruct phase specialization: resolve forwarding,
    // store plain. Typed on RootSlot so a coloured write cannot be spelled (see Barrier.cpp).
    void ResolveStaticStructRoots(MAddress dst, const GCTib gctib) const;

    // MRT_GCV2_ZGC_SELFHEAL: the ported OpenJDK ZBarrier::self_heal loop
    // (RefField.h / zBarrier.inline.hpp:72-110), with the ZBarrierFastPath every
    // ReadReference already spells inline: target == nullptr || is_load_good(field).
    // One definition here rather than six lambdas so the exit predicate cannot drift
    // between phases. Only reached when ZgcSelfHealEnabled().
    void ZgcSelfHealLoadGood(RefField<false>& field, zpointer observed, zpointer healPtr,
                             HealSite site) const;
    void ZgcSelfHealLoadGood(RefField<true>& field, zpointer observed, zpointer healPtr,
                             HealSite site) const;

    // obj may be null for static/global fields (source treated as old).
    void RecordCrossGenEdge(BaseObject* obj, MAddress fieldAddress, BaseObject* ref) const;
    // storecov: optional pre-store snapshot of store-good (addr,target) pairs; nullptr = always Record
    // (ReadGeneric / legacy callers). Defined in Barrier.cpp.
    struct StoreGoodPrevSnapshot;
    void RecordCrossGenEdgesInStruct(BaseObject* obj, MAddress start, size_t size,
                                     const StoreGoodPrevSnapshot* prevSnap = nullptr) const;
    void RecordCrossGenEdgesInRefArray(BaseObject* obj, MAddress start, size_t size,
                                       const StoreGoodPrevSnapshot* prevSnap = nullptr) const;

private:
    template<typename Function>
    static decltype(auto) DispatchPhase(BarrierPhase phase, const Barrier& barrier, Function&& function);

    RememberedSet& theRememberedSet;
    const BarrierPhase phase;
};
} // namespace MapleRuntime
#endif // ~MRT_BARRIER_H
