// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <atomic>
#include <csignal>
#include <sched.h>

#include "Heap/StickyLog.h"
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"

#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
namespace {
constexpr size_t NONFATAL_RETRY_LIMIT = 8;

class NonfatalCheckCounter {
public:
    explicit NonfatalCheckCounter(const char* checkSite) : site(checkSite) {}

    ~NonfatalCheckCounter()
    {
        LOG(RTLOG_ERROR, "GC nonfatal summary: site=%s passCount=%zu badCount=%zu guardCount=%zu", site,
            passCount.load(std::memory_order_relaxed), badCount.load(std::memory_order_relaxed),
            guardCount.load(std::memory_order_relaxed));
    }

    size_t RecordPass() { return passCount.fetch_add(1, std::memory_order_relaxed) + 1; }
    size_t RecordBad() { return badCount.fetch_add(1, std::memory_order_relaxed) + 1; }
    size_t RecordGuard() { return guardCount.fetch_add(1, std::memory_order_relaxed) + 1; }

    static bool ShouldLog(size_t count)
    {
        return count <= NONFATAL_RETRY_LIMIT || (count & (count - 1)) == 0;
    }

private:
    const char* site;
    std::atomic<size_t> passCount { 0 };
    std::atomic<size_t> badCount { 0 };
    std::atomic<size_t> guardCount { 0 };
};

struct UntagRefFieldRetryState {
    const void* field = nullptr;
    size_t consecutiveBadCount = 0;
};

struct UntagRefFieldBreadcrumb {
    const void* holder = nullptr;
    const void* field = nullptr;
    const void* target = nullptr;
    const void* caller = nullptr;
    size_t fieldOffset = 0;
    volatile sig_atomic_t active = 0;
};

NonfatalCheckCounter tryUntagRefFieldCounter("TryUntagRefField:isValidTarget");
NonfatalCheckCounter enumRefFieldRootCounter("EnumRefFieldRoot:latest->IsValidObject");
thread_local UntagRefFieldBreadcrumb untagRefFieldBreadcrumb;
thread_local UntagRefFieldRetryState untagRefFieldRetryState;
} // namespace

void PrintUntagRefFieldBreadcrumb() noexcept
{
    if (untagRefFieldBreadcrumb.active == 0) {
        return;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    FLOG(RTLOG_ERROR,
         "GC untag breadcrumb: holder=%p field=%p field_offset=%zu target=%p caller_pc=%p",
         untagRefFieldBreadcrumb.holder, untagRefFieldBreadcrumb.field, untagRefFieldBreadcrumb.fieldOffset,
         untagRefFieldBreadcrumb.target, untagRefFieldBreadcrumb.caller);
}

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

// this api updates current pointer as well as old pointer, caller should take care of this.
template<bool forward>
bool WCollector::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
                                       BaseObject*& toObj) const
{
    RefField<> oldRef(field);
    if (oldRef.IsTagged()) {
        fromObj = oldRef.GetTargetObject();
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
            untagRefFieldRetryState.field = nullptr;
            untagRefFieldRetryState.consecutiveBadCount = 0;
            return false;
        }
        target = oldRef.GetTargetObject();
        untagRefFieldBreadcrumb.active = 0;
        untagRefFieldBreadcrumb.holder = obj;
        untagRefFieldBreadcrumb.field = &field;
        untagRefFieldBreadcrumb.target = target;
        untagRefFieldBreadcrumb.caller = __builtin_return_address(0);
        untagRefFieldBreadcrumb.fieldOffset =
            obj == nullptr ? static_cast<size_t>(-1) : BaseObject::FieldOffset(obj, &field);
        std::atomic_signal_fence(std::memory_order_seq_cst);
        untagRefFieldBreadcrumb.active = 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const size_t passCount = tryUntagRefFieldCounter.RecordPass();
        const bool isValidTarget = target->IsValidObject();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        untagRefFieldBreadcrumb.active = 0;
        if (UNLIKELY(!isValidTarget)) {
            const size_t badCount = tryUntagRefFieldCounter.RecordBad();
            if (NonfatalCheckCounter::ShouldLog(badCount)) {
                LOG(RTLOG_ERROR,
                    "TryUntagRefField encounters invalid tagged target %p at field %p: holder=%p caller=%p "
                    "fieldOffset=%zu passCount=%zu badCount=%zu action=no-write",
                    target, &field, obj, untagRefFieldBreadcrumb.caller, untagRefFieldBreadcrumb.fieldOffset,
                    passCount, badCount);
            }
            if (untagRefFieldRetryState.field == &field) {
                ++untagRefFieldRetryState.consecutiveBadCount;
            } else {
                untagRefFieldRetryState.field = &field;
                untagRefFieldRetryState.consecutiveBadCount = 1;
            }
            if (untagRefFieldRetryState.consecutiveBadCount >= NONFATAL_RETRY_LIMIT) {
                const size_t guardCount = tryUntagRefFieldCounter.RecordGuard();
                LOG(RTLOG_ERROR,
                    "TryUntagRefField retry guard exhausted: target=%p field=%p holder=%p caller=%p "
                    "fieldOffset=%zu consecutiveBadCount=%zu guardCount=%zu action=return-null-no-write",
                    target, &field, obj, untagRefFieldBreadcrumb.caller, untagRefFieldBreadcrumb.fieldOffset,
                    untagRefFieldRetryState.consecutiveBadCount, guardCount);
                target = nullptr;
                untagRefFieldRetryState.field = nullptr;
                untagRefFieldRetryState.consecutiveBadCount = 0;
                return true;
            }
            return false;
        }
        untagRefFieldRetryState.field = nullptr;
        untagRefFieldRetryState.consecutiveBadCount = 0;
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
        BaseObject* target = oldField.GetTargetObject();
        CHECK_DETAIL(target->IsValidObject(), "Enum static root %p(%p) encounters invalid object", target, &field);
        rootSet.push_back(target);
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
    const size_t passCount = enumRefFieldRootCounter.RecordPass();
    const bool isValidLatest = latest->IsValidObject();
    if (UNLIKELY(!isValidLatest)) {
        const size_t badCount = enumRefFieldRootCounter.RecordBad();
        if (NonfatalCheckCounter::ShouldLog(badCount)) {
            LOG(RTLOG_ERROR,
                "Enum static root %p(%p) encounters invalid object: holder=%p caller=%p fieldOffset=%zu "
                "passCount=%zu badCount=%zu action=skip-no-write",
                latest, &field, nullptr, __builtin_return_address(0), static_cast<size_t>(-1), passCount, badCount);
        }
        return;
    }
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
        BaseObject* root = oldField.GetTargetObject();
        CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
        rootSet.push_back(root);
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
        CHECK_DETAIL(targetObj->IsValidObject(), "Invalid object %p is referenced by object %p: %s and offset %zd",
                     targetObj, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
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
    ForEachRefSlot(obj, visitor);
}

BaseObject* WCollector::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    const char* sourceKind = kind == RefSlotKind::WEAK_REFERENT ? "weak" : "strong";
    BaseObject* latest = nullptr;
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        CHECK_DETAIL(targetObj->IsValidObject(), "Invalid object %p is referenced by %s object %p: %s and offset %zd",
                     targetObj, sourceKind, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
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
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by %s object %p: %s and offset %zd",
                 latest, sourceKind, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
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
        BaseObject* latest = FindLatestVersion(object);
        return latest == nullptr ? object : latest;
    }
    return object;
}

void WCollector::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor)
{
    RootVisitor rawRootVisitor = [this, &visitor](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        visitor(ResolveMinorReference(field));
    };
    RefFieldVisitor fieldVisitor = [this, &visitor](RefField<>& field) { visitor(ResolveMinorReference(field)); };

    MutatorManager::Instance().VisitAllMutators(
        [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
    Heap::GetHeap().VisitStaticRoots(fieldVisitor);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitGCRoots(rawRootVisitor);
    collectorResources.GetFinalizerProcessor().VisitFinalizers(rawRootVisitor);
    Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);

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

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    CHECK_DETAIL(object->IsValidObject(), "minor root/reference %p is not a valid object", object);
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
    if (region->IsYoungRegion() && !region->IsMarkedObject(object)) {
        if (StickyLog::Instance().IsMinorValidatorEnabled()) {
            minorDiscoveredObjects.insert(object);
        }
        workStack.push_back(object);
    }
}

void WCollector::TraceYoungClosure(WorkStack& workStack)
{
    while (!workStack.empty()) {
        BaseObject* object = workStack.back();
        workStack.pop_back();
        if (MarkObject(object)) {
            continue;
        }
        object->ForEachRefField([this, &workStack](RefField<>& field) {
            PushYoungObject(ResolveMinorReference(field), workStack);
        });
    }
}

void WCollector::RescanRememberedSet(WorkStack& workStack)
{
    StickyLog::Instance().RescanLoggedLines([this, &workStack](MAddress lineStart, MAddress lineEnd) {
        if (StickyLog::Instance().IsMinorValidatorEnabled()) {
            minorRescannedLines.insert(lineStart);
        }
        RegionInfo* region = RegionInfo::GetRegionInfoAt(lineStart);
        if (!region->IsValidRegion() || region->IsGarbageRegion() || region->IsYoungRegion()) {
            return false;
        }
        bool retainLine = false;
        auto scanObject = [this, &workStack, lineStart, lineEnd, &retainLine](BaseObject* object) {
            MAddress objectStart = reinterpret_cast<MAddress>(object);
            MAddress objectEnd = objectStart + RegionSpace::GetAllocSize(*object);
            if (objectStart >= lineEnd || objectEnd <= lineStart) {
                return;
            }
            ForEachStrongRefSlot(object,
                [this, &workStack, &retainLine](RefSlotKind, BaseObject* target, RefField<>& field) {
                    if (StickyLog::Instance().IsMinorValidatorEnabled()) {
                        minorRescannedFields.insert(reinterpret_cast<MAddress>(&field));
                    }
                    if (Heap::IsHeapAddress(target) &&
                        RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target))->IsYoungRegion()) {
                        retainLine = true;
                    }
                    PushYoungObject(target, workStack);
                });
        };
        LiveInfo* retainedLiveInfo = region->GetRetainedLiveInfo();
        RegionInfo::RetainedLiveInfoState retainedState = region->GetRetainedLiveInfoState();
        if (retainedState == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED) {
            region->VisitAllObjects([&scanObject](BaseObject* object) { scanObject(object); });
        } else if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
            return false;
        } else {
            CHECK(retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_VALID);
            if (region->IsLargeRegion()) {
                scanObject(reinterpret_cast<BaseObject*>(region->GetRegionStart()));
            } else if (region->IsSmallRegion()) {
                CHECK(retainedLiveInfo != nullptr);
                uintptr_t position = region->GetRegionStart();
                size_t offset = 0;
                uintptr_t allocPtr = region->GetRegionAllocPtr();
                while (position < allocPtr) {
                    BaseObject* object = reinterpret_cast<BaseObject*>(position);
                    size_t allocSize = RegionSpace::GetAllocSize(*object);
                    position += allocSize;
                    if (retainedLiveInfo->IsSurvivedObject(offset)) {
                        scanObject(object);
                    }
                    offset += allocSize;
                }
            }
        }
        return retainLine;
    });
}

void WCollector::ValidateYoungMarking()
{
    struct ValidationEdge {
        BaseObject* object;
        BaseObject* source;
        MAddress sourceField;
    };
    std::unordered_set<BaseObject*> reachable;
    std::vector<ValidationEdge> pending;
    VisitMinorRoots([&pending](BaseObject* object) {
        if (Heap::IsHeapAddress(object)) {
            pending.push_back({ object, nullptr, 0 });
        }
    });

    size_t youngReachable = 0;
    while (!pending.empty()) {
        BaseObject* object = pending.back().object;
        BaseObject* source = pending.back().source;
        MAddress sourceField = pending.back().sourceField;
        pending.pop_back();
        if (!Heap::IsHeapAddress(object) || !reachable.insert(object).second) {
            continue;
        }
        CHECK_DETAIL(object->IsValidObject(), "sticky validator reached invalid object %p", object);
        RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region->IsYoungRegion()) {
            ++youngReachable;
            MAddress line = reinterpret_cast<MAddress>(object) & ~(StickyLog::LINE_SIZE - 1);
            RegionInfo* sourceRegion = source == nullptr ? nullptr :
                RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(source));
            MAddress sourceLine = source == nullptr ? 0 :
                reinterpret_cast<MAddress>(source) & ~(StickyLog::LINE_SIZE - 1);
            CHECK_DETAIL(region->IsMarkedObject(object),
                "sticky minor validator missed object=%p region=%p type=%u line=%#zx logged=%u liveBytes=%u "
                "objectClass=%s source=%p sourceRegion=%p sourceType=%u sourceYoung=%u sourceLine=%#zx "
                "sourceLogged=%u sourceLineRescanned=%u sourceFieldRescanned=%u targetDiscovered=%u targetCandidate=%u "
                "targetAge=%u sourceAge=%u "
                "sourceClass=%s sourceField=%#zx sourceOffset=%zu",
                object, region, region->GetRegionType(), line, StickyLog::Instance().IsLoggedLine(line),
                region->GetLiveByteCount(), object->GetTypeInfo()->GetName(), source, sourceRegion,
                sourceRegion == nullptr ? 0 : static_cast<unsigned>(sourceRegion->GetRegionType()),
                sourceRegion == nullptr ? 0 : static_cast<unsigned>(sourceRegion->IsYoungRegion()), sourceLine,
                source == nullptr ? 0 : static_cast<unsigned>(StickyLog::Instance().IsLoggedLine(sourceLine)),
                static_cast<unsigned>(minorRescannedLines.count(sourceLine) != 0),
                static_cast<unsigned>(minorRescannedFields.count(sourceField) != 0),
                static_cast<unsigned>(minorDiscoveredObjects.count(object) != 0),
                static_cast<unsigned>(minorCandidateRegions.count(region) != 0),
                static_cast<unsigned>(region->GetYoungAge()),
                sourceRegion == nullptr ? 0 : static_cast<unsigned>(sourceRegion->GetYoungAge()),
                source == nullptr ? "<root>" : source->GetTypeInfo()->GetName(), sourceField,
                source == nullptr ? 0 : sourceField - reinterpret_cast<MAddress>(source));
        }
        object->ForEachRefField([this, &pending, object](RefField<>& field) {
            BaseObject* target = ResolveMinorReference(field);
            if (Heap::IsHeapAddress(target)) {
                pending.push_back({ target, object, reinterpret_cast<MAddress>(&field) });
            }
        });
    }
    VLOG(REPORT, "[StickyMinor] validator reachable=%zu young=%zu failures=0", reachable.size(), youngReachable);
}

void WCollector::FlushAllocationRegions()
{
    theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
}

void WCollector::DoYoungGarbageCollection()
{
    uint64_t start = TimeUtil::NanoSeconds();
    ScopedStopTheWorld stw("sticky minor", true, GCPhase::GC_PHASE_ENUM);
    TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    FlushAllocationRegions();

    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats = manager.PrepareYoungGarbageCandidates(
        [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    minorRescannedLines.clear();
    minorRescannedFields.clear();
    minorDiscoveredObjects.clear();
    WorkStack workStack = NewWorkStack();
    WorkStack enumRoots = NewWorkStack();
    theAllocator.VisitAllocBuffers([&enumRoots](AllocBuffer& buffer) { buffer.MergeRoots(enumRoots); });
    while (!enumRoots.empty()) {
        BaseObject* object = enumRoots.back();
        enumRoots.pop_back();
        PushYoungObject(object, workStack);
    }
    VisitMinorRoots([this, &workStack](BaseObject* object) { PushYoungObject(object, workStack); });
    RescanRememberedSet(workStack);
    TraceYoungClosure(workStack);

    if (StickyLog::Instance().IsMinorValidatorEnabled()) {
        ValidateYoungMarking();
    }

    SatbBuffer& satbBuffer = SatbBuffer::Instance();
    satbBuffer.DiscardStickyLogBuffer();
    StickyLog& stickyLog = StickyLog::Instance();
    SatbBuffer::Node* promotionNode = nullptr;
    size_t promotedRegions = 0;
    size_t promotedObjects = 0;
    size_t promotedLoggedLines = 0;
    uint64_t promotionScanNs = 0;
    manager.CollectYoungGarbage(stats, [this, &satbBuffer, &stickyLog, &promotionNode, &promotedRegions,
                                           &promotedObjects, &promotedLoggedLines, &promotionScanNs](RegionInfo* region) {
        uint64_t scanStart = TimeUtil::NanoSeconds();
        ++promotedRegions;
        (void)region->VisitLiveObjectsUntilFalse([this, &satbBuffer, &stickyLog, &promotionNode,
                                                    &promotedObjects, &promotedLoggedLines](BaseObject* object) {
            ++promotedObjects;
            bool logged = false;
            ForEachStrongRefSlot(object,
                [&satbBuffer, &stickyLog, &promotionNode, &promotedLoggedLines, &logged, object]
                (RefSlotKind, BaseObject* target, RefField<>&) {
                    if (logged || !Heap::IsHeapAddress(target)) {
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (!targetRegion->IsYoungRegion()) {
                        return;
                    }
                    MAddress lineStart = 0;
                    if (stickyLog.TryLogLine(reinterpret_cast<MAddress>(object), lineStart)) {
                        satbBuffer.EnsureGoodStickyNode(promotionNode);
                        CHECK_DETAIL(promotionNode != nullptr && promotionNode->PushLine(lineStart),
                                     "failed to remember promoted source line");
                        ++promotedLoggedLines;
                    }
                    logged = true;
                });
            return true;
        });
        promotionScanNs += TimeUtil::NanoSeconds() - scanStart;
    });
    satbBuffer.FlushStickyLogQueue(promotionNode);
    VLOG(REPORT, "[StickyMinor] promotion scan regions=%zu objects=%zu loggedLines=%zu time=%zu us",
         promotedRegions, promotedObjects, promotedLoggedLines, promotionScanNs / NS_PER_US);
    if (StickyLog::Instance().IsForceSlowPathEnabled()) {
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_IDLE);
    } else {
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    }

    ++minorRunsSinceMajor;
    ++minorTotalRuns;
    GetGCStats().collectedBytes = stats.reclaimedBytes;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
        "[StickyMinor] run=%zu candidates=%zu candidateBytes=%zu reclaimedRegions=%zu reclaimedBytes=%zu pause=%zu us",
        minorTotalRuns, stats.candidateRegions, stats.candidateBytes, stats.reclaimedRegions, stats.reclaimedBytes,
        pauseUs);
}

void WCollector::DoGarbageCollection()
{
    StickyLog& stickyLog = StickyLog::Instance();
    if (gcReason == GC_REASON_YOUNG && stickyLog.IsMinorEnabled() &&
        minorRunsSinceMajor < stickyLog.GetMajorInterval()) {
        DoYoungGarbageCollection();
        return;
    }

    if (stickyLog.IsMinorEnabled()) {
        ScopedStopTheWorld stw("sticky major allocation rollover");
        FlushAllocationRegions();
    }
    TraceHeap();
    PostTrace();

    Preforward();

    ForwardFromSpace();

    if (StickyLog::Instance().IsEnabled()) {
        ScopedStopTheWorld stw("advance sticky log epoch");
        SatbBuffer::Instance().DiscardStickyLogBuffer();
        StickyLog::Instance().BeginEpoch();
    }
    if (StickyLog::Instance().IsForceSlowPathEnabled()) {
        TransitionToGCPhase(GCPhase::GC_PHASE_ENUM, true);
        Heap::GetHeap().InstallBarrier(GCPhase::GC_PHASE_IDLE);
    } else {
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    }
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);

    CollectSmallSpace();
    if (stickyLog.IsMinorEnabled()) {
        ScopedStopTheWorld stw("sticky major promotion");
        FlushAllocationRegions();
        reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager().PromoteAllRegions();
        minorRunsSinceMajor = 0;
    }
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
        for (;;) {
            // Object-level t5 publish, or region-level FORWARDED (set before
            // CollectRegion write-lock / type flip — after that TryLockRead
            // always fails because !IsFromRegion).
            if (obj->IsForwarded() ||
                region->GetRouteState() == RegionInfo::RouteState::FORWARDED) {
                return GetForwardPointer(obj, region);
            }
            if (region->TryLockReadFromRegion()) {
                BaseObject* toVersion = ForwardObjectImpl(obj, region);
                region->UnlockReadFromRegion();
                return toVersion;
            }
            // Write-lock (CollectRegion) or transient: never null-gated FindToVersion.
            sched_yield();
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
