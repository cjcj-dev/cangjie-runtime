// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <array>
#include "Common/Handle.h"
#include <atomic>
#include <functional>
#include <list>
#include "Common/OopStorage.h"
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include "Common/BaseObject.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Mutator/MutatorManager.h"
namespace MapleRuntime {
class Mutator;

class HandleMark {
    Mutator& mutator;
    size_t mark;
public:
    explicit HandleMark(Mutator& mutator);
    ~HandleMark();
    HandleMark(const HandleMark&) = delete;
    HandleMark& operator=(const HandleMark&) = delete;
};

template <typename Iterator>
class ParallelApply {
    Iterator iter_;
    volatile bool completed;
public:
    template <typename... Args>
    explicit ParallelApply(Args&&... args) : iter_(std::forward<Args>(args)...), completed(false) {}
    template <typename Closure>
    void apply(Closure* cl)
    {
        if (!completed) {
            iter_.Apply(*cl);
            if (!completed) {
                completed = true;
            }
        }
    }
    Iterator& iter() { return iter_; }
};

class OopStorageSetIteratorStrong {
public:
    OopStorageSetIteratorStrong(unsigned workers,
                                ZGenerationIdOptional generation);
    explicit OopStorageSetIteratorStrong(unsigned workers = 1)
        : OopStorageSetIteratorStrong(workers, ZGenerationIdOptional::none) {}
    void Apply(const NativeSlotVisitor& visitor);
private:
    std::array<OopStorage::ParState<true>, 1> states;
    ZGenerationIdOptional generation;
};
class OopStorageSetIteratorWeak {
public:
    OopStorageSetIteratorWeak(unsigned workers,
                              ZGenerationIdOptional generation);
    explicit OopStorageSetIteratorWeak(unsigned workers = 1)
        : OopStorageSetIteratorWeak(workers, ZGenerationIdOptional::none) {}
    void Apply(const NativeSlotVisitor& visitor);
    void report_num_dead();
private:
    std::array<OopStorage::ParState<true>, 3> states;
    ZGenerationIdOptional generation;
    size_t numDead = 0;
};
class StaticRootsAdapterIterator {
public:
    explicit StaticRootsAdapterIterator(ZGenerationIdOptional = ZGenerationIdOptional::none) {}
    void Apply(const NativeSlotVisitor& visitor);
private:
    std::atomic<bool> claimed{false};
};
class JavaThreadsIterator {
public:
    explicit JavaThreadsIterator(ZGenerationIdOptional generation = ZGenerationIdOptional::none);
    uint32_t claim();
    void Apply(const std::function<void(Mutator&)>& visitor);
private:
    std::vector<Mutator*> threads;
    volatile uint32_t claimed;
    ZGenerationIdOptional generation;
};
class RootsIteratorStrongColored {
public:
    RootsIteratorStrongColored(unsigned workers,
                               ZGenerationIdOptional generation)
        : strong(workers, generation), statics(generation) {}
    explicit RootsIteratorStrongColored(unsigned workers = 1)
        : RootsIteratorStrongColored(workers, ZGenerationIdOptional::none) {}
    void Apply(const NativeSlotVisitor& visitor);
private:
    ParallelApply<OopStorageSetIteratorStrong> strong;
    ParallelApply<StaticRootsAdapterIterator> statics;
};
class RootsIteratorWeakColored {
public:
    RootsIteratorWeakColored(unsigned workers,
                             ZGenerationIdOptional generation)
        : weak(workers, generation) {}
    explicit RootsIteratorWeakColored(unsigned workers = 1)
        : RootsIteratorWeakColored(workers, ZGenerationIdOptional::none) {}
    void Apply(const NativeSlotVisitor& visitor)
    {
        NativeSlotVisitor copy = visitor;
        weak.apply(&copy);
    }
    void report_num_dead() { weak.iter().report_num_dead(); }
private:
    ParallelApply<OopStorageSetIteratorWeak> weak;
};
class RootsIteratorAllColored {
public:
    RootsIteratorAllColored(unsigned workers,
                            ZGenerationIdOptional generation)
        : strong(workers, generation), weak(workers, generation),
          statics(generation) {}
    explicit RootsIteratorAllColored(unsigned workers = 1)
        : RootsIteratorAllColored(workers, ZGenerationIdOptional::none) {}
    void Apply(const NativeSlotVisitor& visitor);
private:
    ParallelApply<OopStorageSetIteratorStrong> strong;
    ParallelApply<OopStorageSetIteratorWeak> weak;
    ParallelApply<StaticRootsAdapterIterator> statics;
};

class RootsIteratorStrongUncolored {
public:
    explicit RootsIteratorStrongUncolored(ZGenerationIdOptional generation = ZGenerationIdOptional::none)
        : javaThreads(generation) {}
    void Apply(const std::function<void()>& visitor)
    {
        std::function<void()> copy = visitor;
        uncolored.apply(&copy);
    }
    void ApplyThreads(const std::function<void(Mutator&)>& visitor)
    {
        std::function<void(Mutator&)> copy = visitor;
        javaThreads.apply(&copy);
    }
private:
    struct Once {
        void Apply(const std::function<void()>& visitor)
        {
            if (!claimed.exchange(true, std::memory_order_relaxed)) {
                visitor();
            }
        }
        std::atomic<bool> claimed{false};
    };
    ParallelApply<Once> uncolored;
    ParallelApply<JavaThreadsIterator> javaThreads;
};
using RootsIteratorAllUncolored = RootsIteratorStrongUncolored;

class StaticRootTable {
public:
    struct StaticRootArray {
        NativeSlot* content[0];
    };

    StaticRootTable() { totalRootsCount = 0; }
    ~StaticRootTable() = default;
    void RegisterRoots(StaticRootArray* addr, U32 size);
    void UnregisterRoots(StaticRootArray* addr, U32 size);
    void VisitRoots(const NativeSlotVisitor& visitor);


private:
    std::mutex gcRootsLock;                         // lock gcRootsBuckets
    std::map<StaticRootArray*, U32> gcRootsBuckets; // record gc roots entry of CFile
    USize totalRootsCount;
};

class ExportObject : public BaseObject {
public:
    U32 GetId() { return id; }
private:
    U32 id = { 0 };
};

struct ExportObjectInfo {
    explicit ExportObjectInfo(bool state) : generation(1), occupied(true), activeState(state) {}
    NativeSlot* exportObj = nullptr;
    U32 generation = 0;
    bool occupied = false;
    bool activeState = true;
};
class ExportRootTable {
public:
    static constexpr U32 HANDLE_INDEX_BITS = 32;

    static U64 PackExportHandle(U32 index, U32 generation)
    {
        return (static_cast<U64>(generation) << HANDLE_INDEX_BITS) | static_cast<U64>(index);
    }
    static U32 ExportHandleIndex(U64 handle) { return static_cast<U32>(handle); }
    static U32 ExportHandleGeneration(U64 handle) { return static_cast<U32>(handle >> HANDLE_INDEX_BITS); }

    U64 RegisterExportRoot(BaseObject* exportObj)
    {
        std::lock_guard<std::mutex> lg(tableMutex);
        if (accessableId.empty()) {
            exportRoots.emplace_back(true);
            U32 index = static_cast<U32>(exportRoots.size() - 1);
            PublishRegisteredRoot(exportRoots[index], exportObj);
            return PackExportHandle(index, exportRoots[index].generation);
        }
        U64 index = accessableId.front();
        accessableId.pop_front();
        ExportObjectInfo& slot = exportRoots[index];
        U32 nextGen = slot.generation + 1;
        if (nextGen == 0) {
            nextGen = 1;
        }
        slot.generation = nextGen;
        PublishRegisteredRoot(slot, exportObj);
        slot.occupied = true;
        slot.activeState = true;
        return PackExportHandle(static_cast<U32>(index), nextGen);
    }
    BaseObject* GetExportRoot(U64 handle)
    {
        std::lock_guard<std::mutex> lg(tableMutex);
        U64 index = 0;
        if (!ResolveLiveIndex(handle, index)) {
            return nullptr;
        }
        return ZBarrier::ReadStaticRef(*exportRoots[index].exportObj);
    }
    void RemoveExportRoot(U64 handle)
    {
        std::lock_guard<std::mutex> lg(tableMutex);
        U64 index = 0;
        if (!ResolveLiveIndex(handle, index)) {
            return;
        }
        ZBarrier::WriteStaticRef(*exportRoots[index].exportObj, nullptr);
        weakStorage.Release(exportRoots[index].exportObj);
        exportRoots[index].exportObj = nullptr;
        exportRoots[index].occupied = false;
        exportRoots[index].activeState = true;
        accessableId.push_back(index);
    }
    OopStorage& RootStorage() { return weakStorage; }
    void VisitGCRoots(const NativeSlotVisitor& visitor);
    void SetActiveState(U64 handle, bool state)
    {
        std::lock_guard<std::mutex> lg(tableMutex);
        U64 index = 0;
        if (!ResolveLiveIndex(handle, index)) {
            return;
        }
        exportRoots[index].activeState = state;
    }
    bool CheckActiveState(U64 handle, BaseObject* obj)
    {
        std::lock_guard<std::mutex> lg(tableMutex);
        U64 index = 0;
        if (!ResolveLiveIndex(handle, index)) {
            return false;
        }
        auto info = exportRoots[index];
        // tableMutex protects handle ownership; slot access uses the native barrier.
        if (ZBarrier::ReadStaticRef(*info.exportObj) != obj) {
            return false;
        }
        return info.activeState;
    }
private:
    void PublishRegisteredRoot(ExportObjectInfo& slot, BaseObject* exportObj)
    {
        // ZGC native stores preserve the slot's previous value, not the
        // incoming reference (zBarrier.inline.hpp:709-715). The caller already
        // holds the incoming object; publish its handle before returning.
        slot.exportObj = weakStorage.Allocate();
        ZBarrier::WriteStaticRef(*slot.exportObj, exportObj);
    }

    bool ResolveLiveIndex(U64 handle, U64& index) const
    {
        U32 rawIndex = ExportHandleIndex(handle);
        U32 generation = ExportHandleGeneration(handle);
        if (rawIndex >= exportRoots.size()) {
            return false;
        }
        const ExportObjectInfo& slot = exportRoots[rawIndex];
        if (!slot.occupied) {
            return false;
        }
        // Packed MCC handles carry a non-zero generation. ExportObject::GetId is a
        // U32 slot index used by the OHOS cycle resolver; accept that raw index
        // only while the slot is occupied (object identity is checked by caller).
        if (generation != 0 && generation != slot.generation) {
            return false;
        }
        index = rawIndex;
        return true;
    }

    OopStorage weakStorage;
    std::mutex tableMutex;
    std::vector<ExportObjectInfo> exportRoots;
    std::list<U64> accessableId;
};

}
