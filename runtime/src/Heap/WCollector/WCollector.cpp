// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Concurrency/Concurrency.h"
#include "Heap/WCollector/TagEpochProbe.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {
bool WCollector::IsUnmovableFromObject(BaseObject* obj) const
{
    // filter const string object.
    if (!Heap::IsHeapAddress(obj)) {
        return false;
    }

    RegionInfo* regionInfo = nullptr;
    if (RegionInfo::InGhostFromRegion(obj)) {
        regionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<uintptr_t>(obj));
    } else {
        regionInfo = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    }
    return regionInfo->IsUnmovableFromRegion();
}

bool WCollector::MarkObject(BaseObject* obj) const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    size_t objectSize = obj->GetSize();
    bool marked = region->MarkObject(obj, objectSize);
    if (!marked) {
        region->AddLiveByteCount(objectSize);
        (void)region;
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %u", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    bool resurrected = region->ResurrectObject(obj, offset);
        if (!resurrected) {
            region->AddLiveByteCount(obj->GetSize());
            DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %u", region, region->GetRegionStart(),
                 obj, obj->GetTypeInfo(), obj->GetSize(), region->GetLiveByteCount());
        }
        return resurrected;
}

BaseObject* WCollector::FindToVersion(BaseObject* obj) const
{
    // Observation only: record if target is outside heap before GetUnitIdxAt aborts.
    if (!Heap::IsHeapAddress(obj)) {
        TagEpochProbe::OnPreFindToVersion(nullptr, obj, reinterpret_cast<MAddress>(obj), "FindToVersion");
    }
    RegionInfo* fromRegionInfo = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (fromRegionInfo == nullptr) {
        return nullptr;
    }
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    return space.GetRegionManager().RouteObject(obj);
}

// this api updates current pointer as well as old pointer, caller should take care of this.
template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj) const
{
    RefField<> oldRef(field);
    if (oldRef.IsTagged()) {
        fromObj = oldRef.GetTargetObject();
        TagEpochProbe::OnPreFindToVersion(&field, fromObj, oldRef.GetFieldValue(),
                                          forward ? "TryUpdateRefFieldImpl_fwd" : "TryUpdateRefFieldImpl");
        if (forward) {
            toObj = const_cast<WCollector*>(this)->TryForwardObject(fromObj);
        } else {
            toObj = FindToVersion(fromObj);
        }
        if (toObj == nullptr) {
            return false;
        }
        RefField<> tmpField(toObj);
        if (field.CompareExchange(oldRef.GetFieldValue(), tmpField.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(TRACE, "update obj %p<%p>(%zu)+%zu ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(),
                     obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field, oldRef.GetFieldValue(),
                     tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update ref@%p: 0x%zx -> %p", &field, oldRef.GetFieldValue(), toObj);
            }
            return true;
        } else {
            if (obj != nullptr) {
                DLOG(TRACE,
                     "update obj %p<%p>(%zu)+%zu but cas failed ref-field@%p: %#zx(%#zx) -> %#zx but cas failed ", obj,
                     obj->GetTypeInfo(), obj->GetSize(), BaseObject::FieldOffset(obj, &field), &field,
                     oldRef.GetFieldValue(), field.GetFieldValue(), tmpField.GetFieldValue());
            } else {
                DLOG(TRACE, "update but cas failed ref@%p: 0x%zx(%zx) -> %p", &field, oldRef.GetFieldValue(),
                     field.GetFieldValue(), toObj);
            }
            return true;
        }
    }

    return false;
}
bool WCollector::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef);
}

bool WCollector::TryForwardRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
{
    BaseObject* oldRef = nullptr;
    return TryUpdateRefFieldImpl<true>(obj, field, oldRef, newRef);
}
// this api untags current pointer as well as old pointer, caller should take care of this.
bool WCollector::TryUntagRefField(BaseObject* obj, RefField<>& field, BaseObject*& target) const
{
    for (;;) {
        RefField<> oldRef(field);
        if (!oldRef.IsTagged()) {
            return false;
        }
        target = oldRef.GetTargetObject();
        RefField<> newRef(target);
        if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(FIX, "untag obj %p<%p>(%zu) ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
                     &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            } else {
                DLOG(FIX, "untag ref@%p: %#zx -> %#zx", &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            }
            return true;
        }
    }

    return false;
}

// RefFieldRoot is root in tagged pointer format.
void WCollector::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
{
    RefField<> oldField(field);
    // if field is already tagged currently, it is also already enumerated.
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }

    BaseObject* latest = nullptr;
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Enum static root %p(%p) encounters invalid object", latest, &field);
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    } else {
        DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &field, oldField.GetFieldValue(), latest,
             latest->GetTypeInfo(), latest->GetSize());
    }
    rootSet.push_back(latest);
}

void WCollector::EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const
{
    RefField<>& refField = reinterpret_cast<RefField<>&>(ref);
    RefField<> oldField(refField);
    CHECK_DETAIL(!IsOldPointer(oldField), "EnumAndTagRawRoot failed: Invalid root: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        rootSet.push_back(oldField.GetTargetObject());
        return;
    }
    BaseObject* root = oldField.GetTargetObject();
    if (Heap::IsHeapAddress(root)) {
        CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
        RefField<> newField = GetAndTryTagRefField(root);
        if (oldField.GetFieldValue() == newField.GetFieldValue()) {
            DLOG(ENUM, "enum raw root @%p: %p(%zu)", &ref, root, root->GetSize());
        } else if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
            DLOG(ENUM, "enum static ref@%p: %#zx=>%#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(),
                 newField.GetFieldValue(), root, root->GetTypeInfo(), root->GetSize());
        } else {
            DLOG(ENUM, "enum static ref@%p: %#zx -> %p<%p>(%zu)", &refField, oldField.GetFieldValue(), root,
                 root->GetTypeInfo(), root->GetSize());
        }
        rootSet.push_back(root);
    }
}

// note each ref-field will not be traced twice, so each old pointer the tracer meets must come from previous gc.
void WCollector::TraceRefField(BaseObject* obj, RefField<>& field, WorkStack& workStack) const
{
    RefField<> oldField(field);
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        if (!IsMarkedObject(targetObj)) {
            workStack.push_back(targetObj);
        }
        return;
    }

    BaseObject* latest = nullptr;
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }

    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by object %p: %s and offset %zd", latest,
                 obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, oldField.GetFieldValue(),
             newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    }

    if (!IsMarkedObject(latest)) {
        workStack.push_back(latest);
    }
}

void WCollector::TraceObjectRefFields(BaseObject* obj, WorkStack& workStack)
{
    auto visitor = [this, obj, &workStack](RefField<>& field) { TraceRefField(obj, field, workStack); };
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (!typeInfo->HasRefField()) {
        return;
    }

    if (UNLIKELY(typeInfo->IsRawArray())) {
        MArray* array = reinterpret_cast<MArray*>(obj);
        MIndex arrayLength = array->GetLength();
        TypeInfo* componentTypeInfo = array->GetComponentTypeInfo();
        if (componentTypeInfo->IsStructType()) {
            GCTib gcTib = componentTypeInfo->GetGCTib();
            MAddress contentAddr = reinterpret_cast<Uptr>(array) + MArray::GetContentOffset();
            size_t elementSize = array->GetElementSize();
            for (MIndex i = 0; i < arrayLength; ++i) {
                gcTib.ForEachBitmapWord(contentAddr, visitor);
                contentAddr += elementSize;
            }
        } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
                   componentTypeInfo->IsInterface()) {
            RefField<>* arrayContent = reinterpret_cast<RefField<>*>(array->ConvertToCArray());
            for (MIndex i = 0; i < arrayLength; ++i) {
                visitor(arrayContent[i]);
            }
        } else {
            LOG(RTLOG_FATAL, "array object %p has wrong component type", array);
        }
        return;
    }

    MAddress contentAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    obj->GetGCTib().ForEachBitmapWord(contentAddr, visitor);
}

BaseObject* WCollector::GetAndTryTagObj(BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    BaseObject* latest = nullptr;
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        return targetObj;
    }
    if (IsOldPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        latest = FindLatestVersion(targetObj);
    } else {
        latest = field.GetTargetObject();
    }
    // target object could be null or non-heap for some static variable.
    if (!Heap::IsHeapAddress(latest)) {
        return nullptr;
    }
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by weak object %p: %s and offset %zd",
                 latest, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        DLOG(TRACE, "trace obj %p ref@%p: %p<%p>(%zu)", obj, &field, latest, latest->GetTypeInfo(), latest->GetSize());
    } else if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(TRACE, "trace obj %p ref@%p: %#zx => %#zx->%p<%p>(%zu)", obj, &field, oldField.GetFieldValue(),
            newField.GetFieldValue(), latest, latest->GetTypeInfo(), latest->GetSize());
    }
    return latest;
}

BaseObject* WCollector::ForwardUpdateRawRef(ObjectRef& root)
{
    auto& refField = reinterpret_cast<RefField<>&>(root);
    RefField<> oldField(refField);
    BaseObject* oldObj = oldField.GetTargetObject();
    DLOG(FIX, "visit raw-ref @%p: %p", &root, oldObj);
    CHECK_DETAIL(!IsOldPointer(oldField), "ForwardUpdateRawRef failed: Invalid object: %zx", oldField.GetFieldValue());
    if (IsCurrentPointer(oldField)) {
        if (IsGhostFromObject(oldObj)) {
            BaseObject* toVersion = TryForwardObject(oldObj);
            CHECK(toVersion != nullptr);
            RefField<> newField(toVersion);
            // CAS failure means some mutator or gc thread writes a new ref (must be a to-object), no need to retry.
            if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
                DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, toVersion);
                return toVersion;
            }
            CHECK(!IsCurrentPointer(refField));
        } else {
            RefField<> newField(oldObj);
            // CAS failure means some mutator or gc thread writes a new ref (must be a to-object), no need to retry.
            if (refField.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
                DLOG(FIX, "fix raw-ref @%p: %p -> %p", &root, oldObj, oldObj);
                return oldObj;
            }
        }
    }

    return oldObj;
}
void WCollector::PreforwardAllExportFromRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Heap::GetHeap().VisitAllExportRoots(visitor);
}
void WCollector::PreforwardFinalizerProcessorRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    collectorResources.GetFinalizerProcessor().VisitRawPointers(visitor);
}

void WCollector::PreforwardConcurrencyModelRoots()
{
    RootVisitor visitor = [this](ObjectRef& root) { ForwardUpdateRawRef(root); };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
}

void WCollector::PreforwardDiscoveredExternObjects()
{
    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
    CHECK(discoveredExternObjects.empty());
    auto it = cycleRefWorkStack.begin();
    std::unordered_map<BaseObject*, std::list<BaseObject*>> tmp;
    while (it != cycleRefWorkStack.end()) {
        BaseObject* exportObj = it->first;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        for (auto &externObj : it->second) {
            if (IsGhostFromObject(externObj) && !IsUnmovableFromObject(externObj)) {
                BaseObject* toObj = ForwardObject(externObj);
                externObj = toObj;
            }
        }
        if (latest != exportObj) {
            tmp[latest] = it->second;
            it = cycleRefWorkStack.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        cycleRefWorkStack.insert(tmp.begin(), tmp.end());
    }
}

void WCollector::PreforwardAllResurrectExportFromObjects()
{
    std::unordered_set<BaseObject*> tmp;
    std::lock_guard<std::mutex> lg(resurrectExportMtx);
    auto it = resurrectedExportObjectes.begin();
    while (it != resurrectedExportObjectes.end()) {
        BaseObject* exportObj = *it;
        BaseObject* latest = exportObj;
        if (IsGhostFromObject(exportObj) && !IsUnmovableFromObject(exportObj)) {
            latest = ForwardObject(exportObj);
        }
        if (latest != exportObj) {
            tmp.insert(latest);
            it = resurrectedExportObjectes.erase(it);
        } else {
            it++;
        }
    }
    if (!tmp.empty()) {
        resurrectedExportObjectes.insert(tmp.begin(), tmp.end());
    }
}
void WCollector::TraceHeap()
{
    WorkStack workStack = NewWorkStack();
    WorkStack foreignStack = NewWorkStack();
    // assemble garbage candidates for tracing.
    reinterpret_cast<RegionSpace&>(theAllocator).AssembleGarbageCandidates();

    {
        MRT_PHASE_TIMER("enum roots & update old pointers within");
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        DoEnumeration(workStack, foreignStack);
    }

    {
        MRT_PHASE_TIMER("trace live objects & update old pointers in ref-fields");
        markedObjectCount.store(0, std::memory_order_relaxed);
        TransitionToGCPhase(GCPhase::GC_PHASE_TRACE, true);
        reinterpret_cast<RegionSpace&>(theAllocator).PrepareTrace();
        DoTracing(workStack, foreignStack);

        ProcessFinalizers();
    }
}

void WCollector::PostTrace()
{
    MRT_PHASE_TIMER("PostTrace");
    TransitionToGCPhase(GC_PHASE_POST_TRACE, true);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    space.GetRegionManager().HandleTraceRegions();
    // clear weakRef List, set the referent as null
    WeakRefBuffer::Instance().ClearWeakRefBuffer();
    // clear satb buffer when gc finish tracing.
    SatbBuffer::Instance().ClearBuffer();
    // reclaim large objects immediately after tracing is done.
    PrepareCycleRef();
    CollectLargeGarbage();
    CollectPinnedGarbage();
    RefineFromSpace();
    fwdTable.PrepareForwardTable();
}

void WCollector::Preforward()
{
    ScopedEntryTrace trace("CJRT_GC_PREFORWARD");
    MRT_PHASE_TIMER("Preforward");
    {
        ScopedLightSync scopedLightSync("Preforward", true, GCPhase::GC_PHASE_PREFORWARD);
    }

    GCThreadPool* threadPool = GetThreadPool();
    MRT_ASSERT(threadPool != nullptr, "thread pool is null");
    // forward and fix cj future objects
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardConcurrencyModelRoots(); }));

    // forward and fix finalizer roots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardFinalizerProcessorRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllExportFromRoots(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));
    threadPool->Start();
    threadPool->WaitFinish();
}


extern "C" void CJ_MRT_RolveCycleRef();
extern "C" void ResolveCycleRefStub(CrossRefHandler, BaseObject*, BaseObject*, void**);

class CJFunc : public BaseObject {
public:
    CrossRefHandler GetHandler()
    {
        return handler;
    }
private:
    CrossRefHandler handler = nullptr;
};

class CJInteropContext : public BaseObject {
public:
    CJFunc* GetCJFunc()
    {
        return static_cast<CJFunc*>(Heap::GetBarrier().ReadReference(this,
            *reinterpret_cast<RefField<false>*>(&cjFunc)));
    }
private:
    CJFunc* cjFunc = nullptr;
};

class CJForeignProxy : public BaseObject {
public:
    CJInteropContext* GetCJInteropContext()
    {
        return static_cast<CJInteropContext*>(Heap::GetBarrier().ReadReference(this,
            *reinterpret_cast<RefField<false>*>(&interopContext)));
    }
private:
    CJInteropContext* interopContext = nullptr;
};

CrossRefHandler WCollector::GetCrossRefHandler(BaseObject *foreignProxy)
{
    return static_cast<CJForeignProxy*>(foreignProxy)->GetCJInteropContext()->GetCJFunc()->GetHandler();
}

void WCollector::ResolveCycleRef()
{
#if defined (__OHOS__)
    size_t i = 0;
    if (!cycleWorkStackMtx.try_lock()) {
        CJ_MRT_RolveCycleRef();
        return;
    }
    for (auto it = cycleRefWorkStack.begin(); it != cycleRefWorkStack.end(); i++) {
        ScopedObjectAccess soa;
        auto phase = GetGCPhase();
        static constexpr size_t taskNum = 100;
        if (phase == GC_PHASE_PREFORWARD || i >= taskNum) {
            cycleWorkStackMtx.unlock();
            CJ_MRT_RolveCycleRef();
            return;
        }
        BaseObject* exportObj = it->first;
        auto& heap = Heap::GetHeap();
        auto id = static_cast<ExportObject*>(exportObj)->GetId();
        if (!heap.CheckExportObjState(id, exportObj)) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        if (resurrectedExportObjectes.find(exportObj) != resurrectedExportObjectes.end() ||
            resurrectedExportObjectesForwardPhase.find(exportObj) != resurrectedExportObjectesForwardPhase.end()) {
            it = cycleRefWorkStack.erase(it);
            continue;
        }
        auto externObjs = it->second;
        void* returnUnit = nullptr;
        for (auto externObj : externObjs) {
            auto resolveHook = GetCrossRefHandler(externObj);
            ResolveCycleRefStub(resolveHook, exportObj, externObj, &returnUnit);
        }
        heap.SetExportObjActiveState(id, false);
        it++;
    }
    cycleWorkStackMtx.unlock();
    resurrectedExportObjectes.clear();
    resurrectedExportObjectesForwardPhase.clear();
#endif
}
void WCollector::PostResolveCycleTask()
{
#if defined (__OHOS__)
    if (cycleRefWorkStack.empty()) {
        return;
    }
    CJ_MRT_RolveCycleRef();
#endif
}

BaseObject* WCollector::ResolveMinorReference(RefField<>& field) const
{
    RefField<> value(field);
    BaseObject* object = value.GetTargetObject();
    if (IsOldPointer(value)) {
        TagEpochProbe::OnPreFindToVersion(&field, object, value.GetFieldValue(), "ResolveMinorReference");
        BaseObject* latest = FindLatestVersion(object);
        if (latest != nullptr) {
            field.SetTargetObject(latest);
            return latest;
        }
    }
    return object;
}

void WCollector::VisitMinorRootSlots(RootVisitor& rawRootVisitor, const RefFieldVisitor& fieldVisitor)
{
    MutatorManager::Instance().VisitAllMutators(
        [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
    Heap::GetHeap().VisitStaticRoots(fieldVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
}

void WCollector::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
}

void WCollector::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor)
{
    RootVisitor rawRootVisitor = [this, &visitor](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        visitor(ResolveMinorReference(field));
    };
    RefFieldVisitor fieldVisitor = [this, &visitor](RefField<>& field) { visitor(ResolveMinorReference(field)); };
    VisitMinorRootSlots(rawRootVisitor, fieldVisitor);
    VisitMinorValueRoots(visitor);
}

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    CHECK_DETAIL(object->IsValidObject(), "minor root/reference %p is not a valid object", object);
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (region->IsYoungRegion() && !region->IsMarkedObject(object)) {
        workStack.push_back(object);
    }
}

void WCollector::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan, MinorObjectSet& reachableObjects,
                                   MinorSlotSet& reachableSlots, MinorSlotSet& weakSlots)
{
    auto pushTarget = [this, fullYoungScan, &workStack](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(target)) {
                workStack.push_back(target);
            }
        } else {
            PushYoungObject(target, workStack);
        }
    };
    while (!workStack.empty()) {
        BaseObject* object = workStack.back();
        workStack.pop_back();
        if (!Heap::IsHeapAddress(object) || !reachableObjects.insert(object).second) {
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "minor closure reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) {
            (void)MarkObject(object);
        } else if (!fullYoungScan) {
            continue;
        }
        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            RefField<>* referentField = reinterpret_cast<RefField<>*>(
                reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            weakSlots.insert(reinterpret_cast<MAddress>(referentField));
            BaseObject* referent = ResolveMinorReference(*referentField);
            if (!Heap::IsHeapAddress(referent)) {
                continue;
            }
            RegionInfo* referentRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(referent));
            if (referentRegion->IsYoungRegion()) {
                WeakRefBuffer::Instance().Insert(object);
            }
            referent->ForEachRefField([&pushTarget](RefField<>& field) { pushTarget(field); });
            continue;
        }
        object->ForEachRefField([&reachableSlots, &pushTarget](RefField<>& field) {
            reachableSlots.insert(reinterpret_cast<MAddress>(&field));
            pushTarget(field);
        });
    }
}

void WCollector::RescanRememberedSet(WorkStack& workStack, const MinorSlotSet& rememberedSlots,
                                     const MinorSlotSet& reachableSlots, const MinorSlotSet& weakSlots,
                                     bool fullYoungScan)
{
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot) || weakSlots.count(slot) != 0 ||
            (fullYoungScan && reachableSlots.count(slot) == 0)) {
            continue;
        }
        RefField<>* field = reinterpret_cast<RefField<>*>(slot);
        PushYoungObject(ResolveMinorReference(*field), workStack);
    }
}

bool WCollector::FixMinorEvacuatedSlot(RefField<>& field) const
{
    RefField<> oldField(field);
    BaseObject* target = ResolveMinorReference(field);
    BaseObject* current = target;
    if (Heap::IsHeapAddress(target) && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    RefField<> newField(current);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        return false;
    }
    CHECK_DETAIL(field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue()),
                 "minor reference changed while the world was stopped field=%p from=%p to=%p",
                 &field, target, current);
    return true;
}

void WCollector::FixMinorRootSlots()
{
    RootVisitor rawRootVisitor = [this](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        (void)FixMinorEvacuatedSlot(field);
    };
    RefFieldVisitor fieldVisitor = [this](RefField<>& field) { (void)FixMinorEvacuatedSlot(field); };
    Heap::GetHeap().VisitStaticRoots(fieldVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
}

void WCollector::FixMinorObjectSlots(BaseObject* object)
{
    if (!object->HasRefField()) {
        return;
    }
    object->ForEachRefField([this](RefField<>& field) { (void)FixMinorEvacuatedSlot(field); });
}

void WCollector::EvacuateYoungRegions(const MinorObjectSet& reachableObjects, const MinorSlotSet& rememberedSlots)
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    auto currentObject = [this](BaseObject* object) {
        if (IsGhostFromObject(object) && !IsUnmovableFromObject(object)) {
            return ForwardObject(object);
        }
        return object;
    };

    auto fixForwardedReferences = [this, &reachableObjects, &rememberedSlots, &currentObject]() {
        FixMinorRootSlots();
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        for (BaseObject* object : reachableObjects) {
            FixMinorObjectSlots(currentObject(object));
        }
        for (MAddress slot : rememberedSlots) {
            if (Heap::IsHeapAddress(slot)) {
                (void)FixMinorEvacuatedSlot(*reinterpret_cast<RefField<>*>(slot));
            }
        }
    };

    TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
    FixMinorRootSlots();
    PreforwardDiscoveredExternObjects();
    PreforwardAllResurrectExportFromObjects();

    TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
    fwdTable.PrepareForwardTable();
    TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
    fixForwardedReferences();
    ValidateMinorReferences("before-return", &reachableObjects);

    ForwardFromSpace();

    for (RegionInfo* region : minorCandidateRegions) {
        if (region->IsYoungRegion()) {
            region->SetYoungRegionFlag(0);
            region->SetYoungAge(0);
        }
    }

    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t rebuiltRecords = 0;
    for (BaseObject* object : reachableObjects) {
        BaseObject* holder = currentObject(object);
        RegionInfo* holderRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(holder));
        if (holderRegion->IsYoungRegion() || !holder->HasRefField()) {
            continue;
        }
        holder->ForEachRefField([this, &rememberedSet, &rebuiltRecords](RefField<>& field) {
            BaseObject* target = ResolveMinorReference(field);
            if (!Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion->IsYoungRegion()) {
                rememberedSet.Record(reinterpret_cast<MAddress>(&field));
                ++rebuiltRecords;
            }
        });
    }
    VLOG(REPORT, "[GCV2Minor] remembered-set rebuilt=%zu", rebuiltRecords);

    fwdTable.PrepareForwardTable();
    ValidateMinorReferences("after-dispel", nullptr);
    manager.ReassembleFromSpace();
}

void WCollector::ValidateMinorReferences(const char* point, const MinorObjectSet* reachableObjects)
{
    const char* enabled = std::getenv("MRT_GCV2_STALE_REFERENCE_VALIDATOR");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    constexpr size_t categoryCount = 12;
    constexpr size_t sampleCount = 3;
    const std::array<const char*, categoryCount> categoryNames = {
        "stack", "register", "derived", "static", "heap", "weak", "finalizer", "export",
        "concurrency", "external_resurrection", "exception", "raw_object"
    };
    std::array<size_t, categoryCount> counts{};
    std::array<std::array<const void*, sampleCount>, categoryCount> slots{};
    std::array<std::array<BaseObject*, sampleCount>, categoryCount> holders{};
    std::array<std::array<BaseObject*, sampleCount>, categoryCount> targets{};
    std::array<std::array<uint8_t, sampleCount>, categoryCount> regionTypes{};
    std::array<std::array<uint8_t, sampleCount>, categoryCount> objectStates{};
    std::array<std::array<uint16_t, sampleCount>, categoryCount> tags{};
    WorkStack pending = NewWorkStack();
    MinorObjectSet visited;
    bool buildReachableClosure = reachableObjects == nullptr;

    auto record = [this, &counts, &slots, &holders, &targets, &regionTypes, &objectStates, &tags](
                      size_t category, const void* slot, BaseObject* holder, BaseObject* target, uint16_t tag) {
        if (!Heap::IsHeapAddress(target) || !IsGhostFromObject(target) || IsUnmovableFromObject(target)) {
            return false;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        bool regionReturned = region == nullptr || region->IsGarbageRegion() || region->IsFreeRegion();
        ObjectState::ObjectStateCode state = target->GetStateWord().GetStateCode();
        if (!regionReturned && state != ObjectState::FORWARDED) {
            return false;
        }
        size_t sample = counts[category]++;
        if (sample < sampleCount) {
            slots[category][sample] = slot;
            holders[category][sample] = holder;
            targets[category][sample] = target;
            regionTypes[category][sample] =
                region == nullptr ? std::numeric_limits<uint8_t>::max() : static_cast<uint8_t>(region->GetRegionType());
            objectStates[category][sample] = static_cast<uint8_t>(state);
            tags[category][sample] = tag;
        }
        return true;
    };
    auto inspectTarget = [&record, &pending, buildReachableClosure](
                             size_t category, const void* slot, BaseObject* holder, BaseObject* target, uint16_t tag) {
        if (record(category, slot, holder, target, tag)) {
            return;
        }
        if (!buildReachableClosure || !Heap::IsHeapAddress(target)) {
            return;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
        if (region != nullptr && !region->IsGarbageRegion() && !region->IsFreeRegion() && target->IsValidObject()) {
            pending.push_back(target);
        }
    };
    auto recordRawRoot = [&inspectTarget](size_t category) {
        return RootVisitor([category, &inspectTarget](ObjectRef& root) {
            RefField<> value = reinterpret_cast<RefField<>&>(root);
            uint16_t tag = value.IsTagged() ? value.GetTagID() : std::numeric_limits<uint16_t>::max();
            inspectTarget(category, &root, nullptr, value.GetTargetObject(), tag);
        });
    };
    auto recordField = [&inspectTarget](size_t category, BaseObject* holder, RefField<>& field) {
        RefField<> value(field);
        uint16_t tag = value.IsTagged() ? value.GetTagID() : std::numeric_limits<uint16_t>::max();
        inspectTarget(category, &field, holder, value.GetTargetObject(), tag);
    };

    RootVisitor stackVisitor = recordRawRoot(0);
    RootVisitor registerVisitor = recordRawRoot(1);
    DerivedPtrVisitor derivedVisitor = [&inspectTarget](BasePtrType basePtr, DerivedPtrType& derivedPtr) {
        inspectTarget(2, &derivedPtr, nullptr, reinterpret_cast<BaseObject*>(basePtr),
                      std::numeric_limits<uint16_t>::max());
    };
    RootVisitor exceptionVisitor = recordRawRoot(10);
    RootVisitor rawObjectVisitor = recordRawRoot(11);
    MutatorManager::Instance().VisitAllMutators(
        [&registerVisitor, &stackVisitor, &derivedVisitor, &exceptionVisitor, &rawObjectVisitor](Mutator& mutator) {
            mutator.VisitHeapReferences(
                registerVisitor, stackVisitor, derivedVisitor, exceptionVisitor, rawObjectVisitor);
        });

    Heap::GetHeap().VisitStaticRoots(
        [&recordField](RefField<>& field) { recordField(3, nullptr, field); });
    collectorResources.GetFinalizerProcessor().VisitRawPointers(recordRawRoot(6));
    Heap::GetHeap().VisitAllExportRoots(recordRawRoot(7));
    RootVisitor concurrencyVisitor = recordRawRoot(8);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&concurrencyVisitor);

    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        for (BaseObject* const& object : resurrectedExportObjectes) {
            inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
        }
        for (BaseObject* const& object : resurrectedExportObjectesForwardPhase) {
            inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
        }
    }
    {
        std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
        for (const auto& entry : cycleRefWorkStack) {
            inspectTarget(9, &entry.first, nullptr, entry.first, std::numeric_limits<uint16_t>::max());
            for (BaseObject* const& object : entry.second) {
                inspectTarget(9, &object, nullptr, object, std::numeric_limits<uint16_t>::max());
            }
        }
    }

    auto visitObject = [this, &recordField](BaseObject* object) {
        BaseObject* holder = object;
        if (IsGhostFromObject(holder) && !IsUnmovableFromObject(holder) &&
            holder->GetStateWord().GetStateCode() == ObjectState::FORWARDED) {
            holder = FindLatestVersion(holder);
        }
        if (holder == nullptr || IsGhostFromObject(holder) || !holder->IsValidObject() || !holder->HasRefField()) {
            return;
        }
        size_t category = holder->IsWeakRef() ? 5 : 4;
        holder->ForEachRefField(
            [category, holder, &recordField](RefField<>& field) { recordField(category, holder, field); });
    };
    if (reachableObjects != nullptr) {
        for (BaseObject* object : *reachableObjects) {
            visitObject(object);
        }
    } else {
        while (!pending.empty()) {
            BaseObject* object = pending.back();
            pending.pop_back();
            if (visited.insert(object).second) {
                visitObject(object);
            }
        }
    }

    size_t total = 0;
    for (size_t category = 0; category < categoryCount; ++category) {
        total += counts[category];
        VLOG(REPORT,
             "[GCV2Minor] STALE_SLOT_CATEGORY_%s point=%s count=%zu "
             "samples=[%p/%p/%p/type=%u/state=%u/tag=%u,%p/%p/%p/type=%u/state=%u/tag=%u,"
             "%p/%p/%p/type=%u/state=%u/tag=%u]",
             categoryNames[category], point, counts[category], slots[category][0], holders[category][0],
             targets[category][0], static_cast<unsigned>(regionTypes[category][0]),
             static_cast<unsigned>(objectStates[category][0]), static_cast<unsigned>(tags[category][0]),
             slots[category][1], holders[category][1], targets[category][1],
             static_cast<unsigned>(regionTypes[category][1]), static_cast<unsigned>(objectStates[category][1]),
             static_cast<unsigned>(tags[category][1]), slots[category][2], holders[category][2], targets[category][2],
             static_cast<unsigned>(regionTypes[category][2]), static_cast<unsigned>(objectStates[category][2]),
             static_cast<unsigned>(tags[category][2]));
    }
    VLOG(REPORT, "[GCV2Minor] VALIDATOR_GATED_BY_MRT_GCV2_STALE_REFERENCE_VALIDATOR point=%s total=%zu",
         point, total);
    if (std::strcmp(point, "round2-start") == 0) {
        VLOG(REPORT, "[GCV2Minor] STALE_SLOT_AT_ROUND2_START_%zu", total);
    }
}

void WCollector::ValidateYoungMarking(const MinorObjectSet& reachableObjects, const MinorObjectSet& allocationRoots)
{
    MinorObjectSet reachable;
    MinorObjectSet expectedYoung;
    WorkStack pending = NewWorkStack();
    VisitMinorRoots([&pending](BaseObject* object) {
        if (Heap::IsHeapAddress(object)) {
            pending.push_back(object);
        }
    });
    for (BaseObject* object : allocationRoots) {
        pending.push_back(object);
    }
    auto pushField = [this, &pending](RefField<>& field) {
        BaseObject* target = ResolveMinorReference(field);
        if (Heap::IsHeapAddress(target)) {
            pending.push_back(target);
        }
    };
    while (!pending.empty()) {
        BaseObject* object = pending.back();
        pending.pop_back();
        if (!reachable.insert(object).second) {
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "minor marking validator reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) {
            expectedYoung.insert(object);
        }
        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            RefField<>* referentField = reinterpret_cast<RefField<>*>(
                reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = ResolveMinorReference(*referentField);
            if (Heap::IsHeapAddress(referent)) {
                referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
            }
            continue;
        }
        object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
    }

    size_t actualYoung = 0;
    size_t unexpectedYoung = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        region->VisitAllObjects([&](BaseObject* object) {
            if (!region->IsMarkedObject(object)) {
                return;
            }
            ++actualYoung;
            if (expectedYoung.count(object) == 0 || reachableObjects.count(object) == 0) {
                ++unexpectedYoung;
            }
        });
    }
    size_t missingYoung = 0;
    for (BaseObject* object : expectedYoung) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (!region->IsMarkedObject(object) || reachableObjects.count(object) == 0) {
            ++missingYoung;
        }
    }
    VLOG(REPORT, "[GCV2Minor] mark-equivalence=%zu/%zu missing=%zu unexpected=%zu",
         actualYoung - unexpectedYoung, expectedYoung.size(), missingYoung, unexpectedYoung);
    CHECK_DETAIL(missingYoung == 0 && unexpectedYoung == 0 && actualYoung == expectedYoung.size(),
                 "minor marking differs from full marking: actual=%zu expected=%zu missing=%zu unexpected=%zu",
                 actualYoung, expectedYoung.size(), missingYoung, unexpectedYoung);
}

void WCollector::FlushAllocationRegions()
{
    theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
}

void WCollector::DoYoungGarbageCollection()
{
    uint64_t start = TimeUtil::NanoSeconds();
    ScopedStopTheWorld stw("young collection", true, GCPhase::GC_PHASE_ENUM);
    TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    FlushAllocationRegions();
    if (minorTotalRuns != 0) {
        ValidateMinorReferences("round2-start", nullptr);
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats = manager.PrepareYoungGarbageCandidates(
        [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    if (stats.candidateRegions == 0) {
        manager.ReassembleFromSpace();
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        ++minorTotalRuns;
        VLOG(REPORT, "[GCV2Minor] run=%zu candidates=0 candidateBytes=0 live=0 reclaimedBytes=0",
             minorTotalRuns);
        return;
    }

    MinorSlotSet rememberedSlots;
    {
        RememberedSet::Records records = Heap::GetHeap().GetRememberedSet().AcquireRecordsForMinor();
        rememberedSlots.insert(records.begin(), records.end());
    }

    const char* fallback = std::getenv("MRT_GCV2_FULL_YOUNG_SCAN");
    bool fullYoungScan = fallback == nullptr || std::strcmp(fallback, "0") != 0;
    WorkStack workStack = NewWorkStack();
    MinorObjectSet reachableObjects;
    MinorObjectSet allocationRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    WorkStack enumRoots = NewWorkStack();
    theAllocator.VisitAllocBuffers([&enumRoots](AllocBuffer& buffer) { buffer.MergeRoots(enumRoots); });
    while (!enumRoots.empty()) {
        BaseObject* object = enumRoots.back();
        enumRoots.pop_back();
        if (Heap::IsHeapAddress(object)) {
            allocationRoots.insert(object);
        }
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(object)) {
                workStack.push_back(object);
            }
        } else {
            PushYoungObject(object, workStack);
        }
    }
    VisitMinorRoots([this, fullYoungScan, &workStack](BaseObject* object) {
        if (fullYoungScan) {
            if (Heap::IsHeapAddress(object)) {
                workStack.push_back(object);
            }
        } else {
            PushYoungObject(object, workStack);
        }
    });
    TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableSlots, weakSlots);
    MinorSlotSet liveRememberedSlots;
    for (MAddress slot : rememberedSlots) {
        if (weakSlots.count(slot) == 0 && (!fullYoungScan || reachableSlots.count(slot) != 0)) {
            liveRememberedSlots.insert(slot);
        }
    }
    RescanRememberedSet(workStack, liveRememberedSlots, reachableSlots, weakSlots, fullYoungScan);
    TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableSlots, weakSlots);

    size_t liveObjects = 0;
    size_t liveBytes = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        liveBytes += region->GetLiveByteCount();
        region->VisitAllObjects([&](BaseObject* object) {
            if (region->IsMarkedObject(object)) {
                ++liveObjects;
            }
        });
    }
    if (fullYoungScan) {
        ValidateYoungMarking(reachableObjects, allocationRoots);
    }

    TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
    WeakRefBuffer::Instance().ClearWeakRefBuffer();
    SatbBuffer::Instance().ClearBuffer();

    size_t allocatedBefore = space.AllocatedBytes();
    EvacuateYoungRegions(reachableObjects, liveRememberedSlots);
    size_t allocatedAfter = space.AllocatedBytes();
    stats.reclaimedBytes = allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    GetGCStats().collectedBytes = stats.reclaimedBytes;

    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    ++minorTotalRuns;
    TagEpochProbe::OnMinorEnd();
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu live=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(fullYoungScan), stats.candidateRegions, stats.candidateBytes,
         liveObjects, liveBytes, liveRememberedSlots.size(), stats.reclaimedBytes, pauseUs);
}

void WCollector::DoGarbageCollection()
{
    if (gcReason == GC_REASON_YOUNG) {
        DoYoungGarbageCollection();
        return;
    }
    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();

    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    TagEpochProbe::OnMajorFlip();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);

    CollectSmallSpace();
    ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo();
}

void WCollector::MarkNewObject(BaseObject* obj)
{
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    if (UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_ENUM) || UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_TRACE) ||
        UNLIKELY(mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER)) {
        MarkObject(obj);
    }
}

void WCollector::ProcessFinalizers()
{
    std::function<bool(BaseObject*)> finalizable = [this](BaseObject* obj) { return !IsMarkedObject(obj); };
    FinalizerProcessor& fp = collectorResources.GetFinalizerProcessor();
    fp.EnqueueFinalizables(finalizable, snapshotFinalizerNum);
    fp.Notify();
}

BaseObject* WCollector::ForwardObject(BaseObject* obj)
{
    BaseObject* to = TryForwardObject(obj);
    return (to != nullptr) ? to : obj;
}

BaseObject* WCollector::TryForwardObject(BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr) {
        return nullptr;
    }

    if (fwdTable.RouteRegion(region)) {
        if (region->TryLockReadFromRegion()) {
            BaseObject* toVersion = ForwardObjectImpl(obj, region);
            region->UnlockReadFromRegion();
            return toVersion;
        } else {
            return FindToVersion(obj);
        }
    } else if (region->IsCompacted()) {
        return FindToVersion(obj);
    }
    return nullptr;
}

BaseObject* WCollector::ForwardObjectImpl(BaseObject* obj, RegionInfo* ghostFromRegion)
{
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
    do {
        StateWord oldWord = obj->GetStateWord();

        // 1. object has already been forwarded
        if (obj->IsForwarded()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion);
            DLOG(FORWARD, "skip forwarded obj %p -> %p<%p>(%zu)", obj, toObj, toObj->GetTypeInfo(), toObj->GetSize());
            return toObj;
        }

        // 2. object is being forwarded, spin until it is forwarded (or gets its own forwarded address)
        if (oldWord.IsLockedWord()) {
            sched_yield();
            continue;
        }

        // 3. hope we can forward this object
        if (obj->TryLockObject(oldWord)) {
            return ForwardObjectExclusive(obj);
        }
    } while (true);
    LOG(RTLOG_FATAL, "forwardObject exit in wrong path");
    return nullptr;
}

BaseObject* WCollector::ForwardObjectExclusive(BaseObject* obj)
{
    size_t size = RegionSpace::GetAllocSize(*obj);
    BaseObject* toObj = fwdTable.RouteObject(obj);
    CHECK_DETAIL(toObj != nullptr, "invalid object route");
    DLOG(FORWARD, "forward obj %p<%p>(%zu) to %p", obj, obj->GetTypeInfo(), size, toObj);
    CopyObject(*obj, *toObj, size);
    toObj->SetStateCode(ObjectState::NORMAL);
    std::atomic_thread_fence(std::memory_order_release);
    obj->UnlockObject(ObjectState::FORWARDED);
    return toObj;
}

void WCollector::CollectSmallSpace()
{
    GCStats& stats = GetGCStats();
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    {
        MRT_PHASE_TIMER("CollectFromSpaceGarbage");
        stats.collectedBytes += stats.smallGarbageSize;
        space.CollectFromSpaceGarbage();
    }

    size_t candidateBytes = stats.fromSpaceSize + stats.pinnedSpaceSize + stats.largeSpaceSize;
    stats.garbageRatio = (candidateBytes > 0) ? static_cast<float>(stats.collectedBytes) / candidateBytes : 0;

    stats.liveBytesAfterGC = space.AllocatedBytes();

    VLOG(REPORT,
         "collect %zu B: old small %zu - %zu B, old pinned %zu - %zu B, old large %zu - %zu B. garbage ratio %.2f%%",
         stats.collectedBytes, stats.fromSpaceSize, stats.smallGarbageSize, stats.pinnedSpaceSize,
         stats.pinnedGarbageSize, stats.largeSpaceSize, stats.largeGarbageSize,
         stats.garbageRatio * 100); // The base of the percentage is 100

    VLOG(REPORT, "start to release heap garbage memory");
#if defined(__EULER__)
    Heap::GetHeap().GetAllocator().TryReclaimGarbageMemory();
#endif
    collectorResources.GetFinalizerProcessor().NotifyToReclaimGarbage();
}

bool WCollector::ShouldIgnoreRequest(GCRequest& request) { return request.ShouldBeIgnored(); }
} // namespace MapleRuntime
