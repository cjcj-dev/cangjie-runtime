// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_ZCROSSVM_HPP
#define MRT_ZCROSSVM_HPP
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Common/TypeDef.h"
#include "Common/MarkWorkStack.h"
#include "Heap/z/zGenerationId.hpp"
namespace MapleRuntime {
class BaseObject;
using CrossRefHandler = void(*)(BaseObject*, BaseObject*);
struct ValueRoot {
    BaseObject* object;
    uintptr_t color;
    ValueRoot(BaseObject* value);
    operator BaseObject*() const { return object; }
};
struct ValueRootHash {
    size_t operator()(const ValueRoot& root) const { return std::hash<BaseObject*>{}(root.object); }
};
using ValueRootSet = std::unordered_set<ValueRoot, ValueRootHash>;
using ValueRootList = std::list<ValueRoot>;
using ValueRootMap = std::unordered_map<ValueRoot, ValueRootList, ValueRootHash>;
// Cangjie foreign-runtime ownership and managed cycle-resolution infrastructure.
// This state has no ZGC collector hierarchy counterpart.
class ZCrossVM {
    friend class RelocationReceiptTest;
    friend class ZGenerationRootTest;
public:
    void ResurrectExportObject(BaseObject* obj);
    void PrepareCycleRef();
    void MergeResurrectExportObjects(Generation generation);
    void ResolveCycleRef();
    void PostResolveCycleTask();
    void ProcessExportRoots(ValueRootList& exportOwners);
    void FindUselessExternObjects();
    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
    void VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor);
    void PreforwardDiscoveredExternObjects(Generation generation);
    void PreforwardAllResurrectExportFromObjects(Generation generation);
private:
    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);
    std::mutex externMtx;
    ValueRootMap discoveredExternObjects;
    // Resolver callbacks may enter managed code and therefore must not own the
    // root-carrier mutex.  Keep resolver serialization separate from the mutex
    // used by GC root and preforward consumers.
    std::mutex cycleResolverMtx;
    std::mutex cycleWorkStackMtx;
    ValueRootMap cycleRefWorkStack;
    // Number of callbacks already delivered for each stable export id. A
    // resolver can be reposted when PREFORWARD is published while a managed
    // callback is running, so progress must outlive one ResolveCycleRef call.
    // Protected by cycleWorkStackMtx together with the root carrier.
    std::unordered_map<U32, size_t> cycleRefProgress;
    std::mutex resurrectExportMtx;
    ValueRootSet resurrectedExportObjectes;
    ValueRootSet resurrectedExportObjectesForwardPhase;

    // Value-only root containers have no addressable RootSlot to heal. Keep
    // their RootObligation on the existing ResolveStoreValue authority and
    // rebuild key-bearing containers while their owner lock is held.
    BaseObject* ResolveCurrentValueRoot(const ValueRoot& root, const void* owner) const;
    void CurrentizeValueRootSet(ValueRootSet& roots) const;
    void CurrentizeValueRootMap(ValueRootMap& roots) const;
};
} // namespace MapleRuntime
#endif
