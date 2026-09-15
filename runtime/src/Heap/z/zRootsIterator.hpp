// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <list>
#include <map>
#include <mutex>
#include <vector>
#include "Common/BaseObject.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Mutator/MutatorManager.h"
namespace MapleRuntime {
class TracingCollector;

// ZRootsIteratorAllColored. Cangjie owns locked native-slot lists rather than
// OopStorage; each physical family is claimed once by the root workers.
class RootsIteratorAllColored {
public:
    explicit RootsIteratorAllColored(const TracingCollector& collector) : collector(collector) {}
    void Apply(const NativeSlotVisitor& visitor);
private:
    const TracingCollector& collector;
    std::atomic<bool> strongClaimed{false};
    std::atomic<bool> weakClaimed{false};
};

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
#ifdef MRT_TESTABLE_INTERNALS
    USize RootCountForTesting();
#endif

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
    NativeSlot exportObj{zpointer::null};
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
        return Heap::GetBarrier().ReadStaticRef(exportRoots[index].exportObj);
    }
    void RemoveExportRoot(U64 handle)
    {
        std::lock_guard<std::mutex> lg(tableMutex);
        U64 index = 0;
        if (!ResolveLiveIndex(handle, index)) {
            return;
        }
        Heap::GetBarrier().WriteStaticRef(exportRoots[index].exportObj, nullptr);
        exportRoots[index].occupied = false;
        exportRoots[index].activeState = true;
        accessableId.push_back(index);
    }
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
        // tableMutex excludes GC visitation, so this retained root is live here.
        if (Heap::GetBarrier().ReadStaticRef(info.exportObj) != obj) {
            return false;
        }
        return info.activeState;
    }
private:
    static void PublishRegisteredRoot(ExportObjectInfo& slot, BaseObject* exportObj)
    {
        // ZGC native stores preserve the slot's previous value, not the
        // incoming reference (zBarrier.inline.hpp:709-715). The caller already
        // holds the incoming object; publish its handle before returning.
        Heap::GetBarrier().WriteStaticRef(slot.exportObj, exportObj);
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

    std::mutex tableMutex;
    std::vector<ExportObjectInfo> exportRoots;
    std::list<U64> accessableId;
};

}
