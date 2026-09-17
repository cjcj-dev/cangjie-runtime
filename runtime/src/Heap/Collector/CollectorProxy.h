// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_PROXY_H
#define MRT_COLLECTOR_PROXY_H

#include "Base/Macros.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "WCollector/WCollector.h"

namespace MapleRuntime {
// CollectorProxy is a special kind of collector, it is derived from Base class Collector, thus behaves like a real
// collector. However, it actually manages a set of collectors implemented yet, and delegate garbage-collecting to
// one of these collectors.
// CollectorProxy should inherit collector interfaces, but no datas
class CollectorProxy : public Collector {
#if defined(MRT_TESTABLE_INTERNALS)
    friend struct MarkPublicationFixture;
#endif
public:
    explicit CollectorProxy(Allocator& allocator, CollectorResources& resources) : wCollector(allocator, resources)
    {
        collectorType = CollectorType::PROXY_COLLECTOR;
    }

    ~CollectorProxy() override = default;

    void Init() override;
    void Fini() override;

    GCPhase GetGCPhase(GCCycleGeneration generation) const override
    {
        return currentCollector != nullptr ? currentCollector->GetGCPhase(generation) : GCPhase::GC_PHASE_UNDEF;
    }

    ZGeneration& GetZGeneration(GCCycleGeneration generation) override
    {
        return currentCollector != nullptr ? currentCollector->GetZGeneration(generation)
                                           : wCollector.GetZGeneration(generation);
    }

    const ZGeneration& GetZGeneration(GCCycleGeneration generation) const override
    {
        return currentCollector != nullptr ? currentCollector->GetZGeneration(generation)
                                           : wCollector.GetZGeneration(generation);
    }

    GCCycleSnapshot GetCycleSnapshot(GCCycleGeneration generation) const override
    {
        return currentCollector != nullptr ? currentCollector->GetCycleSnapshot(generation)
                                           : wCollector.GetCycleSnapshot(generation);
    }

    void MarkYoungObjectIfActive(BaseObject* object) const override
    {
        currentCollector->MarkYoungObjectIfActive(object);
    }
    void MarkYoungRootObject(BaseObject* object) const override
    {
        currentCollector->MarkYoungRootObject(object);
    }

    void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const override
    {
        currentCollector->MarkOldObjectIfActive(object, gcThread);
    }

    void PublishGenerationPhase(GCCycleGeneration generation, GCPhase phase) override
    {
        (currentCollector != nullptr ? *currentCollector : wCollector).PublishGenerationPhase(generation, phase);
    }

    void SetGCPhase(GCCycleGeneration generation, const GCPhase phase) override
    {
        currentCollector->SetGCPhase(generation, phase);
    }

    // dispatch garbage collection to the right collector
    MRT_EXPORT void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;

    bool ShouldIgnoreRequest(GCRequest& request) override { return currentCollector->ShouldIgnoreRequest(request); }

    CopyCollector& GetCurrentCollector() const { return *currentCollector; }

    FindToVersionResult FindToVersion(BaseObject* obj, Generation generation) const override
    {
        return currentCollector->FindToVersion(obj, generation);
    }
    BaseObject* ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
                                  Generation generation) const override
    {
        return currentCollector->ResolveStoreValue(ref, provenance, generation);
    }

    bool IsOldPointer(RefField<>& ref) const override { return currentCollector->IsOldPointer(ref); }
    bool IsCurrentPointer(RefField<>& ref) const override { return currentCollector->IsCurrentPointer(ref); }
    ZGenerationId remap_generation(RefField<>& ref) const override
    {
        return currentCollector->remap_generation(ref);
    }
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation) const override
    {
        return currentCollector->relocate_or_remap_object(obj, generation);
    }
    BaseObject* relocate_or_remap_object(BaseObject* obj, ZGenerationId generation,
                                         const ForwardingProvenance& provenance) const override
    {
        return currentCollector->relocate_or_remap_object(obj, generation, provenance);
    }
    bool IsFromObject(BaseObject* obj) const override { return currentCollector->IsFromObject(obj); }
    bool IsGhostFromObject(BaseObject* obj) const override { return currentCollector->IsGhostFromObject(obj); }
    bool IsUnmovableFromObject(BaseObject* obj) const override { return currentCollector->IsUnmovableFromObject(obj); }

    void AddRawPointerObject(BaseObject* obj) override { return currentCollector->AddRawPointerObject(obj); }
    void RemoveRawPointerObject(BaseObject* obj) override { return currentCollector->RemoveRawPointerObject(obj); }

    BaseObject* ForwardObject(BaseObject* obj, Generation generation) override
    {
        return currentCollector->ForwardObject(obj, generation);
    }

    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& toVersion) const override
    {
        return currentCollector->TryUpdateRefField(obj, field, toVersion);
    }

    bool TryUpdateRefFieldWithProvenance(BaseObject* obj, RefField<>& field, BaseObject*& toVersion,
                                         const ForwardingProvenance& provenance) const override
    {
        return currentCollector->TryUpdateRefFieldWithProvenance(obj, field, toVersion, provenance);
    }

    bool TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& toVersion) const override
    {
        return currentCollector->TryForwardRefField(obj, field, toVersion);
    }

    bool TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const override
    {
        return currentCollector->TryUntagRefField(obj, field, target);
    }

    RefField<> GetAndTryTagRefField(BaseObject* obj) const override
    {
        return currentCollector->GetAndTryTagRefField(obj);
    }


    RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* obj,
                                                  const ForwardingProvenance& provenance) const override
    {
        return currentCollector->GetAndTryTagRefFieldWithProvenance(obj, provenance);
    }

#if defined(MRT_TESTABLE_INTERNALS)
    friend struct RelocationReceiptTestAccess;
    friend struct MarkPort203TestAccess;
#endif

private:
    // supported collector set
    CopyCollector* currentCollector = nullptr;
    WCollector wCollector;
};
} // namespace MapleRuntime

#endif // MRT_COLLECTOR_PROXY_H
