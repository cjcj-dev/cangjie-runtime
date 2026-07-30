// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <atomic>
#include <csignal>
#include <unistd.h>

#include "Base/SysCall.h"
#include "Heap/FixEdgeSet.h"
#include "Heap/ForwardFactTable.h"
#include "Heap/RelocationDiagnosticTable.h"
#include "Heap/StickyLog.h"
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#include "securec.h"

#include "Concurrency/Concurrency.h"
#include "Mutator/MutatorManager.h"

namespace MapleRuntime {
namespace {
struct UntagRefFieldBreadcrumb {
    const void* holder = nullptr;
    const void* field = nullptr;
    const void* target = nullptr;
    const void* caller = nullptr;
    size_t fieldOffset = 0;
    volatile sig_atomic_t active = 0;
};

thread_local UntagRefFieldBreadcrumb untagRefFieldBreadcrumb;

constexpr bool IsInvalidCopyDestinationState(bool missing, bool free, bool garbage, bool from, bool invalidRole)
{
    return missing || free || garbage || from || invalidRole;
}
static_assert(IsInvalidCopyDestinationState(false, false, true, false, false),
              "garbage copy destinations must remain fail-closed");
} // namespace

void PrintUntagRefFieldBreadcrumb() noexcept
{
    if (untagRefFieldBreadcrumb.active == 0) {
        return;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
    // AS-safe: stack format + write(2); no FormatLog / logMutex (REPORT-gchang11 §5 D).
    char buf[320];
    int n = sprintf_s(buf, sizeof(buf),
                      "%d E GC untag breadcrumb: holder=%p field=%p field_offset=%zu target=%p caller_pc=%p\n",
                      static_cast<int>(GetTid()), untagRefFieldBreadcrumb.holder, untagRefFieldBreadcrumb.field,
                      untagRefFieldBreadcrumb.fieldOffset, untagRefFieldBreadcrumb.target,
                      untagRefFieldBreadcrumb.caller);
    if (n > 0) {
        (void)write(STDERR_FILENO, buf, static_cast<size_t>(n));
    }
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
        DLOG(TRACE, "mark obj %p<%p>(%zu) in region %p(%u)@%#zx, live %zu", obj, obj->GetTypeInfo(), objectSize,
             region, region->GetRegionType(), region->GetRegionStart(), region->GetLiveByteCount());
    }
    return marked;
}

bool WCollector::ResurrectObject(BaseObject* obj, size_t offset, RegionInfo* region)
{
    bool resurrected = region->ResurrectObject(obj, offset);
        if (!resurrected) {
            region->AddLiveByteCount(obj->GetSize());
            DLOG(TRACE, "resurrect region %p@%#zx obj %p<%p>(%zu), live bytes %zu", region, region->GetRegionStart(),
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
            CHECK_DETAIL(!forward,
                         "TryForwardRefField: route carrier unavailable for tagged from-object %p; "
                         "terminate instead of retrying the unchanged field",
                         fromObj);
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
        const bool isValidTarget = target->IsValidObject();
        if (LIKELY(isValidTarget)) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            untagRefFieldBreadcrumb.active = 0;
        }
        CHECK_DETAIL(isValidTarget, "TryUntagRefField encounters invalid tagged target %p at field %p", target,
                     &field);
        RefField<> newRef(target);
        if (field.CompareExchange(oldRef.GetFieldValue(), newRef.GetFieldValue())) {
            if (obj != nullptr) {
                DLOG(FIX, "untag obj %p<%p>(%zu) ref-field@%p: %#zx -> %#zx", obj, obj->GetTypeInfo(), obj->GetSize(),
                     &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            } else {
                DLOG(FIX, "untag ref@%p: %#zx -> %#zx", &field, oldRef.GetFieldValue(), newRef.GetFieldValue());
            }
            // E4/P6: successful untag leaves plain→target; if target is already
            // From/GhostFrom, register so BulkForward can close the edge (I5).
            FixEdgeSet::Instance().MaybeAdd(obj, &field, target);
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

    // R1 I4 Trace complement: plain→from observed at scan; register so BulkForward
    // can close if tag CAS lost or P7 left plain. Consume skips tagged slots.
    if (!oldField.IsTagged() && IsFromObject(latest)) {
        FixEdgeSet::Instance().MaybeAdd(obj, &field, latest);
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

void WCollector::FixOldTaggedRefField(BaseObject* holder, RefField<>& field)
{
    RefField<> oldField(field);
    if (!IsOldPointer(oldField)) {
        return;
    }
    BaseObject* fromObj = oldField.GetTargetObject();
    BaseObject* latest = FindToVersion(fromObj);
    if (latest == nullptr) {
        latest = fromObj;
    }
    if (!Heap::IsHeapAddress(latest) || !latest->IsValidObject()) {
        CHECK_DETAIL(false,
                     "InvalidateOldTaggedRefs: old-tag %p from holder %p resolves to invalid %p "
                     "(no live to-version before dispel)",
                     fromObj, holder, latest);
        return;
    }
    RefField<> newField = GetAndTryTagRefField(latest);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        return;
    }
    if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        DLOG(FIX, "F3 fix old-tag holder %p field@%p: %#zx => %#zx -> %p", holder, &field,
             oldField.GetFieldValue(), newField.GetFieldValue(), latest);
    }
}

void WCollector::InvalidateOldTaggedRefsBeforeDispel()
{
    MRT_PHASE_TIMER("InvalidateOldTaggedRefsBeforeDispel");
    ScopedStopTheWorld stw("invalidate old tagged refs before dispel");

    RootVisitor fixRoot = [this](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        FixOldTaggedRefField(nullptr, field);
    };
    RefFieldVisitor fixRootField = [this](RefField<>& field) { FixOldTaggedRefField(nullptr, field); };

    MutatorManager::Instance().VisitAllMutators(
        [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
    Heap::GetHeap().VisitStaticRoots(fixRootField);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
    collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
    collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
    Heap::GetHeap().VisitAllExportRoots(fixRoot);

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    space.ForEachObj(
        [this](BaseObject* obj) {
            if (obj == nullptr || !obj->IsValidObject()) {
                return;
            }
            if (!IsSurvivedObject(obj)) {
                return;
            }
            if (!obj->HasRefField()) {
                return;
            }
            ForEachRefSlot(obj, [this, obj](RefField<>& field) { FixOldTaggedRefField(obj, field); });
        },
        false);
}

// After ForwardFromSpace: rewrite plain→ghost-from survivor edges to plain to.
// Predicate aligned with bulkfwd f04 (plain-only + route state + ghost live).
// holder arg is diagnostic only — P-G walks slot addresses from FixEdgeSet.
// r1route2 R2.1: consume ForwardFactTable only (copy-time side table).
// ⛔ GetRoute/RouteObject/FindToVersion (r1segv D5) and ⛔ IsValidObject(to).
// ⛔ FindLatestVersion (silent fallback returns from when to==null).
void WCollector::FixHolderForwardRefField(BaseObject* holder, RefField<>& field, size_t* skippedNoFact,
                                          size_t* interiorRewritten, BulkMissBuckets* missBuckets)
{
    RefField<> oldField(field);
    // b316 A/B producers are plain (untagged) edges; leave tagged to F3/barriers.
    if (oldField.IsTagged()) {
        return;
    }
    BaseObject* target = oldField.GetTargetObject();
    if (!Heap::IsHeapAddress(target)) {
        return;
    }
    // Only rewrite edges into ghost-from survivors with a published route state.
    // FORWARDABLE still lacks copy-time fact — leave for barriers/F3.
    RegionInfo* ghost = RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target));
    if (ghost == nullptr) {
        return;
    }
    RegionInfo::RouteState st = ghost->GetRouteState();
    if (st != RegionInfo::RouteState::ROUTED && st != RegionInfo::RouteState::FORWARDED &&
        st != RegionInfo::RouteState::COMPACTED) {
        return;
    }
    LiveInfo* ghostLive = ghost->GetGhostLiveInfo();
    if (ghostLive == nullptr) {
        return;
    }
    size_t offset = ghost->GetAddressOffset(reinterpret_cast<MAddress>(target));
    if (!ghostLive->IsSurvivedObject(offset)) {
        return;
    }
    // R2.1 sole fact source: copy-time side table. Miss = dead or not-from or
    // not yet copied under this major — loud skip is correct (not silent).
    // ⛔ no FindToVersion/GetRoute/RouteObject (geometry plan SEGV after ForwardRegion).
    BaseObject* toObj = ForwardFactTable::Instance().Lookup(target);
    size_t interiorOffset = 0;
    bool isInterior = false;
    if (toObj == nullptr &&
        ForwardFactTable::Instance().LookupContaining(target, toObj, interiorOffset)) {
        isInterior = true;
    }
    if (toObj == nullptr || toObj == target) {
        if (skippedNoFact != nullptr) {
            ++(*skippedNoFact);
            if (missBuckets != nullptr) {
                RelocationDiagnosticTable::Entry entry{ false, nullptr, nullptr, 0, nullptr };
                if (RelocationDiagnosticTable::Instance().Lookup(target, entry)) {
                    if (entry.identity) {
                        ++missBuckets->b1LegitIdentity;
                    } else {
                        ++missBuckets->b3RealLoss;
                        ++missBuckets->b3Types[entry.typeInfo];
                        ++missBuckets->b3RegionTypes[static_cast<unsigned>(ghost->GetRegionType())];
                        ++missBuckets->b3RouteStates[static_cast<unsigned>(st)];
                    }
                } else {
                    size_t containingOffset = 0;
                    if (RelocationDiagnosticTable::Instance().LookupContaining(target, entry, containingOffset)) {
                        if (entry.identity) {
                            ++missBuckets->b1LegitIdentity;
                        } else {
                            // A moved copy should already have matched the product
                            // containing lookup. Keep any residual diagnostic-only
                            // range loud and fail-closed (for example, identity).
                            ++missBuckets->b2LegitOther;
                            ++missBuckets->b2InteriorNonObjectBase;
                            static std::atomic<size_t> containedSample{ 0 };
                            size_t sample = containedSample.fetch_add(1, std::memory_order_relaxed) + 1;
                            if ((sample & (sample - 1)) == 0) {
                                VLOG(REPORT,
                                     "[MISSBUCKET_B2_INTERIOR] target=%p source=%p to=%p offset=%zu "
                                     "size=%zu type_info=%p type=%s region_type=%u route_state=%u n=%zu",
                                     target, entry.from, entry.to, containingOffset, entry.size, entry.typeInfo,
                                     entry.typeInfo == nullptr ? "<null>" : entry.typeInfo->GetName(),
                                     static_cast<unsigned>(ghost->GetRegionType()), static_cast<unsigned>(st), sample);
                            }
                        }
                    } else {
                        ++missBuckets->unclassified;
                        ++missBuckets->unclassifiedNoCopyRange;
                    }
                }
            }
            static std::atomic<size_t> skipSample{ 0 };
            size_t n = skipSample.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[FixHolder] skipped_no_fact holder=%p slot=%p target=%p st=%u reason=no_table_fact n=%zu",
                     holder, &field, target, static_cast<unsigned>(st), n);
            }
        }
        return;
    }
    if (!Heap::IsHeapAddress(toObj)) {
        if (skippedNoFact != nullptr) {
            ++(*skippedNoFact);
            if (missBuckets != nullptr) {
                ++missBuckets->unclassified;
                ++missBuckets->invalidToNotHeap;
            }
            static std::atomic<size_t> skipSampleBad{ 0 };
            size_t n = skipSampleBad.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[FixHolder] skipped_no_fact holder=%p slot=%p target=%p to=%p reason=to_not_heap n=%zu",
                     holder, &field, target, toObj, n);
            }
        }
        return;
    }
    // Fail-closed current-state and bounds only (no IsValidObject): r1segv early
    // SEGV was a header touch on uncommitted to pages. Ghost and route describe
    // historical/planning state and must not reject a current copy destination.
    RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(toObj));
    const bool toRegionMissing = toRegion == nullptr;
    const bool toFree = toRegion != nullptr && toRegion->IsFreeRegion();
    const bool toGarbage = toRegion != nullptr && toRegion->IsGarbageRegion();
    const bool toFrom = toRegion != nullptr && toRegion->IsFromRegion();
    const bool toInvalidRole = toRegion != nullptr && !toFree && !toRegion->IsValidRegion();
    if (IsInvalidCopyDestinationState(toRegionMissing, toFree, toGarbage, toFrom, toInvalidRole)) {
        if (skippedNoFact != nullptr) {
            ++(*skippedNoFact);
            if (missBuckets != nullptr) {
                ++missBuckets->unclassified;
                ++missBuckets->invalidToRegion;
                missBuckets->invalidToRegionMissing += static_cast<size_t>(toRegionMissing);
                missBuckets->invalidToRegionFree += static_cast<size_t>(toFree);
                missBuckets->invalidToRegionGarbage += static_cast<size_t>(toGarbage);
                missBuckets->invalidToRegionFrom += static_cast<size_t>(toFrom);
                missBuckets->invalidToRegionRole += static_cast<size_t>(toInvalidRole);
            }
            static std::atomic<size_t> skipSampleReg{ 0 };
            size_t n = skipSampleReg.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[FixHolder] skipped_no_fact holder=%p slot=%p target=%p to=%p reason=to_region_bad n=%zu",
                     holder, &field, target, toObj, n);
            }
        }
        return;
    }
    const MAddress toAddr = reinterpret_cast<MAddress>(toObj);
    if (toAddr < toRegion->GetRegionStart() || toAddr >= toRegion->GetRegionAllocPtr()) {
        if (skippedNoFact != nullptr) {
            ++(*skippedNoFact);
            if (missBuckets != nullptr) {
                ++missBuckets->unclassified;
                ++missBuckets->invalidToBounds;
            }
            static std::atomic<size_t> skipSampleBnd{ 0 };
            size_t n = skipSampleBnd.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[FixHolder] skipped_no_fact holder=%p slot=%p target=%p to=%p reason=to_oob n=%zu",
                     holder, &field, target, toObj, n);
            }
        }
        return;
    }
    if (toRegion->IsGhostFromRegion() && missBuckets != nullptr) {
        ++missBuckets->ghostOverlayPassedActiveGate;
    }
    RefField<> newField(toObj);
    if (oldField.GetFieldValue() == newField.GetFieldValue()) {
        return;
    }
    if (field.CompareExchange(oldField.GetFieldValue(), newField.GetFieldValue())) {
        if (isInterior && interiorRewritten != nullptr) {
            ++(*interiorRewritten);
        }
        DLOG(FIX, "r1route2 fix holder %p field@%p: %#zx => %#zx -> %p (from %p interior_offset=%zu)", holder,
             &field, oldField.GetFieldValue(), newField.GetFieldValue(), toObj, target, interiorOffset);
    }
}

// R1 BulkForward: STW scan FixEdgeSet (index-only, P-G) + roots. ⛔ no ForEachObj /
// VisitAllObjects (H1 bulkfwd SEGV path). Worst-case pause = O(|FixSet| + |Roots|) (H2).
void WCollector::BulkForwardHolderRefs()
{
    MRT_PHASE_TIMER("BulkForwardHolderRefs");
    ScopedStopTheWorld stw("bulk forward holder refs");
    const uint64_t startNs = TimeUtil::NanoSeconds();
    size_t rewritten = 0;
    size_t interiorRewritten = 0;
    size_t skippedNoFact = 0;
    BulkMissBuckets missBuckets;
    const size_t fixSetSize = FixEdgeSet::Instance().SizeApprox();
    FixEdgeSet::Instance().ResetE9GateSkipCount();
    const size_t factTableSize = ForwardFactTable::Instance().SizeApprox();
    const size_t relocationDiagnosticSize = RelocationDiagnosticTable::Instance().Size();

    auto fixOne = [this, &rewritten, &interiorRewritten, &skippedNoFact, &missBuckets](BaseObject* holder,
                                                                                     RefField<>& field) {
        RefField<> before(field);
        FixHolderForwardRefField(holder, field, &skippedNoFact, &interiorRewritten, &missBuckets);
        if (RefField<>(field).GetFieldValue() != before.GetFieldValue()) {
            ++rewritten;
        }
    };

    RootVisitor fixRoot = [&fixOne](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        fixOne(nullptr, field);
    };
    RefFieldVisitor fixRootField = [&fixOne](RefField<>& field) { fixOne(nullptr, field); };

    MutatorManager::Instance().VisitAllMutators(
        [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
    Heap::GetHeap().VisitStaticRoots(fixRootField);
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
    collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
    collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
    Heap::GetHeap().VisitAllExportRoots(fixRoot);

    // Index-only walk: each entry is a field slot address registered at store time.
    FixEdgeSet::Instance().VisitAndClear(
        [&fixOne](RefField<>& field) { fixOne(nullptr, field); });

    // R2.1: fact table lifetime ends with BulkForward (same major STW bracket).
    ForwardFactTable::Instance().Clear();
    // r1missbucket diagnostic lifetime matches the product fact window.
    RelocationDiagnosticTable::Instance().Clear();

    const uint64_t pauseUs = (TimeUtil::NanoSeconds() - startNs) / NS_PER_US;
    const size_t e9Skip = FixEdgeSet::Instance().E9GateSkipCount();
    VLOG(REPORT, "[BulkForwardHolderRefs] e9_gate_skip=%zu", e9Skip);
    const size_t bucketTotal = missBuckets.b1LegitIdentity + missBuckets.b2LegitOther +
        missBuckets.b3RealLoss + missBuckets.unclassified;
    VLOG(REPORT,
         "[BulkForwardHolderRefs] pause=%zu us rewritten=%zu interior_rewritten=%zu skipped_no_fact=%zu fixset=%zu "
         "facttable=%zu fromTarget=%zu crossRegion=%zu relocation_diag=%zu "
         "MISSBUCKET_B1=%zu_B2=%zu_B3=%zu_UNCLASSIFIED=%zu_of_%zu "
         "invalid_to_not_heap=%zu invalid_to_region=%zu invalid_to_bounds=%zu "
         "invalid_to_region_missing=%zu invalid_to_region_free=%zu invalid_to_region_garbage=%zu "
         "invalid_to_region_from=%zu invalid_to_region_role=%zu ghost_overlay_passed_active_gate=%zu "
         "b2_interior_non_object_base=%zu unclassified_no_copy_range=%zu",
         static_cast<size_t>(pauseUs), rewritten, interiorRewritten, skippedNoFact, fixSetSize, factTableSize,
         FixEdgeSet::Instance().FromTargetRegistered(), FixEdgeSet::Instance().CrossRegionRegistered(),
         relocationDiagnosticSize, missBuckets.b1LegitIdentity, missBuckets.b2LegitOther,
         missBuckets.b3RealLoss, missBuckets.unclassified, bucketTotal, missBuckets.invalidToNotHeap,
         missBuckets.invalidToRegion, missBuckets.invalidToBounds, missBuckets.invalidToRegionMissing,
         missBuckets.invalidToRegionFree, missBuckets.invalidToRegionGarbage, missBuckets.invalidToRegionFrom,
         missBuckets.invalidToRegionRole, missBuckets.ghostOverlayPassedActiveGate,
         missBuckets.b2InteriorNonObjectBase, missBuckets.unclassifiedNoCopyRange);
    for (const auto& type : missBuckets.b3Types) {
        VLOG(REPORT, "[MISSBUCKET_B3_TYPE] type_info=%p type=%s count=%zu stage=BulkForward", type.first,
             type.first == nullptr ? "<null>" : type.first->GetName(), type.second);
    }
    for (const auto& regionType : missBuckets.b3RegionTypes) {
        VLOG(REPORT, "[MISSBUCKET_B3_REGION] region_type=%u count=%zu stage=BulkForward", regionType.first,
             regionType.second);
    }
    for (const auto& routeState : missBuckets.b3RouteStates) {
        VLOG(REPORT, "[MISSBUCKET_B3_ROUTE] route_state=%u count=%zu stage=BulkForward", routeState.first,
             routeState.second);
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
    // F3: dispel previous ghost from-regions next; kill one-gen-stale tags first so
    // IsOldPointer cannot outlive FindToVersion's ghost gate (D phase).
    InvalidateOldTaggedRefsBeforeDispel();
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
            // E6 (ii) residual: to-space / never-censused regions only. Full scan
            // is correct (no silent skip). Counter for coverage; sample region.
            static std::atomic<size_t> neverExaminedRescanLines{ 0 };
            size_t n = neverExaminedRescanLines.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((n & (n - 1)) == 0) {
                VLOG(REPORT,
                     "[E6-remset] NEVER_EXAMINED rescan line region=%p start=%#zx state=NEVER n=%zu",
                     region, region->GetRegionStart(), n);
            }
            region->VisitAllObjects([&scanObject](BaseObject* object) { scanObject(object); });
        } else {
            // VALID and EMPTY are one snapshot protocol: neither may bypass the epoch check.
            if (!region->IsRetainedSnapshotValid()) {
                static std::atomic<size_t> retainedEpochMismatchCount{ 0 };
                size_t n = retainedEpochMismatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n & (n - 1)) == 0) {
                    VLOG(REPORT,
                         "[RescanRememberedSet] retained_epoch_mismatch region=%p state=%u "
                         "snapshot_epoch=%llu region_epoch=%llu covered_up_to=%#zx n=%zu",
                         region, static_cast<unsigned>(retainedState),
                         static_cast<unsigned long long>(region->GetRetainedLiveInfoEpoch()),
                         static_cast<unsigned long long>(region->GetSnapshotEpoch()),
                         region->GetRetainedLiveInfoCoveredUpTo(), n);
                }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
                CHECK_DETAIL(false, "retained snapshot epoch mismatch in validator build");
#else
                if (StickyLog::Instance().IsMinorValidatorEnabled()) {
                    CHECK_DETAIL(false, "retained snapshot epoch mismatch in validator mode");
                }
#endif
                // A mismatched bitmap may already belong to a released arena. Stable-state
                // mismatches are producer bugs; the only admitted transient is region teardown.
                return false;
            }
            if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                return false;
            }
            uintptr_t coveredUpTo = region->GetRetainedLiveInfoCoveredUpTo();
            uintptr_t allocPtr = region->GetRegionAllocPtr();
            CHECK(coveredUpTo >= region->GetRegionStart() && coveredUpTo <= allocPtr);
            if (region->IsLargeRegion()) {
                scanObject(reinterpret_cast<BaseObject*>(region->GetRegionStart()));
            } else if (region->IsSmallRegion()) {
                CHECK(retainedLiveInfo != nullptr || coveredUpTo == region->GetRegionStart());
                uintptr_t position = region->GetRegionStart();
                size_t offset = 0;
                while (position < allocPtr) {
                    BaseObject* object = reinterpret_cast<BaseObject*>(position);
                    size_t allocSize = RegionSpace::GetAllocSize(*object);
                    position += allocSize;
                    if (reinterpret_cast<uintptr_t>(object) >= coveredUpTo ||
                        retainedLiveInfo->IsSurvivedObject(offset)) {
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
                "sticky minor validator missed object=%p region=%p type=%u line=%#zx logged=%u liveBytes=%zu "
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

    // R1: index-only bulk rewrite of plain→ghost-from edges registered by runtime
    // write barriers / Trace. Compiler Idle plain stores (P5) not yet registered.
    BulkForwardHolderRefs();

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
    const uint64_t expectedEpoch = region->GetIdentityEpoch();

    if (fwdTable.RouteRegion(region)) {
        if (region->TryLockReadFromRegion()) {
            BaseObject* toVersion = ForwardObjectImpl(obj, region, expectedEpoch);
            region->UnlockReadFromRegion();
            return toVersion;
        } else {
            return GetForwardPointer(obj, region, expectedEpoch);
        }
    } else if (region->IsCompacted()) {
        return GetForwardPointer(obj, region, expectedEpoch);
    }
    return nullptr;
}

__attribute__((visibility("hidden"))) BaseObject* WCollector::ForwardObjectImpl(
    BaseObject* obj, RegionInfo* ghostFromRegion, uint64_t expectedEpoch)
{
    CHECK(GetGCPhase() == GCPhase::GC_PHASE_PREFORWARD || GetGCPhase() == GCPhase::GC_PHASE_FORWARD);
    do {
        StateWord oldWord = obj->GetStateWord();

        // 1. object has already been forwarded
        if (obj->IsForwarded()) {
            auto toObj = GetForwardPointer(obj, ghostFromRegion, expectedEpoch);
            CHECK_DETAIL(toObj != nullptr, "route epoch changed before reading forwarded object %p", obj);
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
            return ForwardObjectExclusive(obj, ghostFromRegion, expectedEpoch);
        }
    } while (true);
    LOG(RTLOG_FATAL, "forwardObject exit in wrong path");
    return nullptr;
}

__attribute__((visibility("hidden"))) BaseObject* WCollector::ForwardObjectExclusive(
    BaseObject* obj, RegionInfo* ghostFromRegion, uint64_t expectedEpoch)
{
    size_t size = RegionSpace::GetAllocSize(*obj);
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    BaseObject* toObj = space.GetRegionManager().RouteObject(obj, ghostFromRegion, expectedEpoch);
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
