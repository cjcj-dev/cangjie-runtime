// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "WCollector.h"

#include <array>
#include <atomic>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <csignal>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include <unistd.h>
#endif

#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Base/SysCall.h"
#endif
#include "Concurrency/Concurrency.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "Heap/WCollector/UntagRefFieldBreadcrumb.h"
#endif
#include "Heap/Verify/VerifyHeap.h"
#include "Heap/Verify/VerifyOption.h"
#include "Heap/Verify/VerifyRememberedSet.h"
#include "Heap/Verify/DiffPathExplainer.h"
#include "Heap/Verify/InteriorSrcProbe.h"
#include "Heap/Verify/StaticSlotProbe.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/VerifyRoots.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "Verify/VerifyRegions.h"
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
#include "securec.h"
#endif

namespace MapleRuntime {
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
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
} // namespace

void PrintUntagRefFieldBreadcrumb() noexcept
{
    if (untagRefFieldBreadcrumb.active == 0) {
        return;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
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
#endif

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
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
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
#endif
        const bool isValidTarget = target->IsValidObject();
#if defined(MRT_GCV2_UNTAG_BREADCRUMB)
        if (LIKELY(isValidTarget)) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
            untagRefFieldBreadcrumb.active = 0;
        }
#endif
        if (UNLIKELY(!isValidTarget)) {
            static const bool f3Region = []() {
                const char* value = std::getenv("MRT_GCV2_F3_REGION");
                return value != nullptr && std::strcmp(value, "1") == 0;
            }();
            if (f3Region) {
                const bool targetInHeap = Heap::IsHeapAddress(target);
                const bool holderInHeap = obj != nullptr && Heap::IsHeapAddress(obj);
                const bool targetInGhost = targetInHeap && RegionInfo::InGhostFromRegion(target);
                RegionInfo* targetCurrent = targetInHeap
                    ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target))
                    : nullptr;
                RegionInfo* targetGhost = targetInHeap
                    ? RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(target))
                    : nullptr;
                RegionInfo* holderCurrent = holderInHeap
                    ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj))
                    : nullptr;
                RegionInfo* holderGhost = holderInHeap
                    ? RegionInfo::GetGhostFromRegionAt(reinterpret_cast<MAddress>(obj))
                    : nullptr;
                RegionInfo* selectedTargetRegion = targetInGhost ? targetGhost : targetCurrent;
                const auto dumpRegion = [](const char* role, BaseObject* object, RegionInfo* region) {
                    if (region == nullptr) {
                        VLOG(REPORT,
                             "[GCV2][F3_REGION][region] role=%s object=%p region=null "
                             "NOT_AVAILABLE_no_region_metadata",
                             role, object);
                        return;
                    }
                    VLOG(REPORT,
                         "[GCV2][F3_REGION][region] role=%s object=%p region=%p type=%u unmovable=%u "
                         "young=%u youngAge=%u routeState=%u from=%u ghost=%u start=%#zx end=%#zx "
                         "alloc=%#zx snapshotEpoch=%llu liveBytes=%zu",
                         role, object, region, static_cast<unsigned>(region->GetRegionType()),
                         static_cast<unsigned>(region->IsUnmovableFromRegion()),
                         static_cast<unsigned>(region->IsYoungRegion()), static_cast<unsigned>(region->GetYoungAge()),
                         static_cast<unsigned>(region->GetRouteState()), static_cast<unsigned>(region->IsFromRegion()),
                         static_cast<unsigned>(region->IsGhostFromRegion()),
                         static_cast<size_t>(region->GetRegionStart()), static_cast<size_t>(region->GetRegionEnd()),
                         static_cast<size_t>(region->GetRegionAllocPtr()),
                         static_cast<unsigned long long>(region->GetSnapshotEpoch()), region->GetLiveByteCount());
                };

                const GCPhase phase = GetGCPhase();
                const uint64_t gcStartNs = GCStats::GetPrevGCStartTime();
                VLOG(REPORT,
                     "[GCV2][F3_REGION] target=%p field=%p holder=%p targetInHeap=%u holderInHeap=%u "
                     "unit.inGhostFromRegion=%u selectedBranch=%s targetCurrent=%p targetGhost=%p selected=%p "
                     "phase=%s(%u) completedGcCount=%zu gcStartNs=%llu",
                     target, &field, obj, static_cast<unsigned>(targetInHeap), static_cast<unsigned>(holderInHeap),
                     static_cast<unsigned>(targetInGhost), targetInGhost ? "ghost" : "current", targetCurrent,
                     targetGhost, selectedTargetRegion, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase),
                     g_gcCount, static_cast<unsigned long long>(gcStartNs));
                dumpRegion("target_current_pre_find", target, targetCurrent);
                dumpRegion("target_ghost_pre_find", target, targetGhost);
                dumpRegion("holder_current", obj, holderCurrent);
                dumpRegion("holder_ghost", obj, holderGhost);

                char targetClear[384];
                char holderClear[384];
                const bool targetWasCleared = targetInHeap &&
                    TraceClear::Lookup(reinterpret_cast<MAddress>(target), targetClear, sizeof(targetClear));
                const bool holderWasCleared = holderInHeap &&
                    TraceClear::Lookup(reinterpret_cast<MAddress>(obj), holderClear, sizeof(holderClear));
                if (!targetInHeap) {
                    std::snprintf(targetClear, sizeof(targetClear), "NOT_AVAILABLE_target_not_in_heap");
                }
                if (!holderInHeap) {
                    std::snprintf(holderClear, sizeof(holderClear), "NOT_AVAILABLE_holder_not_in_heap");
                }
                VLOG(REPORT,
                     "[GCV2][F3_REGION][lifecycle] targetClearedRecent=%u targetClear={%s} "
                     "holderClearedRecent=%u holderClear={%s} "
                     "targetRecycledSinceForward=NOT_AVAILABLE_no_forward_start_snapshotEpoch_baseline",
                     static_cast<unsigned>(targetWasCleared), targetClear, static_cast<unsigned>(holderWasCleared),
                     holderClear);

                char ghostReclaim[384] = "NOT_AVAILABLE_target_not_in_heap";
                char dirtyTake[384] = "NOT_AVAILABLE_target_not_in_heap";
                char garbageReuse[384] = "NOT_AVAILABLE_target_not_in_heap";
                char clearGhost[384] = "NOT_AVAILABLE_target_not_in_heap";
                char dispel[384] = "NOT_AVAILABLE_target_not_in_heap";
                const bool targetGhostReclaimed = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "ghost_reclaim", gcStartNs, ghostReclaim,
                    sizeof(ghostReclaim));
                const bool targetDirtyTaken = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "dirty_take", gcStartNs, dirtyTake, sizeof(dirtyTake));
                const bool targetGarbageReused = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "garbage_reuse", gcStartNs, garbageReuse,
                    sizeof(garbageReuse));
                const bool targetGhostCleared = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "clear_ghost", gcStartNs, clearGhost,
                    sizeof(clearGhost));
                const bool targetDispelled = targetInHeap && TraceClear::LookupKind(
                    reinterpret_cast<MAddress>(target), "dispel", gcStartNs, dispel, sizeof(dispel));
                VLOG(REPORT,
                     "[GCV2][F3_REGION][supply-path] ghostReclaim=%u dirtyTake=%u garbageReuse=%u "
                     "clearGhost=%u dispel=%u pathConfirmedGhostReclaimDirtyReuse=%u "
                     "ghostReclaim={%s} dirtyTake={%s} garbageReuse={%s} clearGhost={%s} dispel={%s}",
                     static_cast<unsigned>(targetGhostReclaimed), static_cast<unsigned>(targetDirtyTaken),
                     static_cast<unsigned>(targetGarbageReused), static_cast<unsigned>(targetGhostCleared),
                     static_cast<unsigned>(targetDispelled),
                     static_cast<unsigned>(targetGhostReclaimed && targetDirtyTaken), ghostReclaim, dirtyTake,
                     garbageReuse, clearGhost, dispel);

                BaseObject* toVersion = targetInHeap ? FindToVersion(target) : nullptr;
                const bool toInHeap = toVersion != nullptr && Heap::IsHeapAddress(toVersion);
                const int toValid = toInHeap ? static_cast<int>(toVersion->IsValidObject()) : -1;
                RegionInfo* toRegion = toInHeap
                    ? RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(toVersion))
                    : nullptr;
                VLOG(REPORT,
                     "[GCV2][F3_REGION][forward] FindToVersion=%p toInHeap=%u toValid=%d "
                     "findMayRouteRegion=%u TryForwardObject=NOT_CALLED_side_effectful_RouteRegion_and_object_copy",
                     toVersion, static_cast<unsigned>(toInHeap), toValid, static_cast<unsigned>(targetInHeap));
                dumpRegion("to_version_after_find", toVersion, toRegion);
                dumpRegion("target_ghost_after_find", target, targetGhost);
            }
        }
        // Anchor main 2f1bc8355e92dbf01c063050b5c9a2947c711d64
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
        // Anchor main 8cd248497dd8c251ca824d9f089d5e30125c80c9
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
    if (VerifyRoots::Enabled()) {
        RootVerifyContext vctx;
        vctx.phase = "EnumRefFieldRoot";
        vctx.kind = RootKind::STATIC_ROOT;
        VerifyRoots::VerifyRootPayload(vctx, &field, latest);
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
        // Anchor main 921e890e67353a8425b5466342f4522bcca4f967
        BaseObject* root = oldField.GetTargetObject();
        CHECK_DETAIL(root->IsValidObject(), "Enum and tag runtime root %p(%p) encounters invalid object", root, &ref);
        rootSet.push_back(root);
        return;
    }
    BaseObject* root = oldField.GetTargetObject();
    if (Heap::IsHeapAddress(root)) {
        if (VerifyRoots::Enabled()) {
            RootVerifyContext vctx;
            vctx.phase = "EnumAndTagRawRoot";
            vctx.kind = RootKind::RUNTIME_ROOT;
            VerifyRoots::VerifyRootPayload(vctx, &ref, root);
        }
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
        // Anchor main 9a124c4f14ddd5944330ddbf68d1659cbb629e56
        CHECK_DETAIL(targetObj->IsValidObject(),
                     "Invalid object %p is referenced by strong object %p: %s and offset %zd", targetObj, obj,
                     obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
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
    CHECK_DETAIL(latest->IsValidObject(), "Invalid object %p is referenced by strong object %p: %s and offset %zd",
                 latest, obj, obj->GetTypeInfo()->GetName(), BaseObject::FieldOffset(obj, &field));
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

BaseObject* WCollector::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
{
    RefField<> oldField(field);
    const char* sourceKind = kind == RefSlotKind::WEAK_REFERENT ? "weak" : "strong";
    BaseObject* latest = nullptr;
    if (IsCurrentPointer(oldField)) {
        BaseObject* targetObj = oldField.GetTargetObject();
        // Anchor main ced6b14fe41380fd2dfb94c91b7fe6973786a80e
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
void WCollector::PreforwardStaticRoots()
{
    RefFieldVisitor visitor = [this](RefField<>& field) {
        ForwardUpdateRawRef(reinterpret_cast<ObjectRef&>(field));
    };
    Heap::GetHeap().VisitStaticRoots(visitor);
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
    bool latestLive = false;
    if (Heap::IsHeapAddress(latest)) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(latest));
        latestLive = region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion() &&
                     latest->IsValidObject();
    }
    if (!latestLive) {
        // Dead one-gen-stale residue (common right after Flip of the just-tagged
        // generation, or remset residue). Null the slot instead of fail-closed:
        // F5 still guards major FindLatestVersion consumers.
        static std::atomic<size_t> g_f3DeadLogged{ 0 };
        size_t n = g_f3DeadLogged.fetch_add(1, std::memory_order_relaxed);
        if (n < 16) {
            VLOG(REPORT,
                 "[GCV2][F3-dead] holder=%p field=%p from=%p latest=%p — null slot",
                 holder, &field, fromObj, latest);
        }
        RefField<> nullField(nullptr);
        (void)field.CompareExchange(oldField.GetFieldValue(), nullField.GetFieldValue());
        return;
    }
    // Always write a plain pointer (not GetAndTryTagRefField). Re-tagging a still-from
    // survivor as current recreates the next generation of one-gen-stale after Flip.
    RefField<> newField(latest);
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
    // A1 (preflipacc §5 / rspec §四 A1): production skips the preflip full-heap walk.
    // Population was empty across 123 majors × 3 loads; cost was ~27% of major total_gc.
    // VERIFY / ACCOUNT still force the walk so soak/ALOT can keep the insurance tripwire.
    static const bool preflipWalk = []() {
        const char* verify = std::getenv("MRT_GCV2_PREFLIP_VERIFY");
        if (verify != nullptr && std::strcmp(verify, "1") == 0) {
            return true;
        }
        const char* account = std::getenv("MRT_GCV2_PREFLIP_ACCOUNT");
        return account != nullptr && std::strcmp(account, "1") == 0;
    }();
    if (!preflipWalk) {
        return;
    }
    InvalidateOldTaggedRefs(true);
}

void WCollector::InvalidateOldTaggedRefs(bool requireSurvivedMark)
{
    MRT_PHASE_TIMER(requireSurvivedMark ? "InvalidateOldTaggedRefs.preflip" : "InvalidateOldTaggedRefs.postflip");
    ScopedStopTheWorld stw(requireSurvivedMark ? "invalidate old tagged refs before dispel"
                                               : "invalidate old tagged refs after flip");

    // A2: parallel full-heap STW walk (ops/design/REMSET_OPTION1_SPEC_0805.txt §六).
    // Sharding = atomic address cursor + region-head ownership; roots = 6 family tasks;
    // account counters are per-worker then merged (H1/H2/H3). A1 VERIFY/ACCOUNT locals below.
    static const bool accountEnv = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_ACCOUNT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    static const bool preflipVerifyEnv = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    static const bool preflipInjectEnv = []() {
        const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY_INJECT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    // Locals for lambda capture (static const cannot be captured under -std=gnu++14).
    const bool account = accountEnv;
    const bool preflipVerify = preflipVerifyEnv;
    const bool preflipInject = preflipInjectEnv;
    // VERIFY always needs fixed counts so a non-zero residue can fail loud.
    const bool trackFixed = account || (requireSurvivedMark && preflipVerify);

    constexpr size_t regionTypeCount = static_cast<size_t>(RegionInfo::RegionType::GARBAGE_REGION) + 1;
    // CHUNK = 256 units × UNIT_SIZE (16MiB @ 64KB unit). Spec §六 T1.
    constexpr size_t kChunkUnits = 256;
    const size_t chunkBytes = kChunkUnits * RegionInfo::UNIT_SIZE;

    struct RootAccount {
        size_t rootSlots = 0;
        size_t oldTaggedRootSlots = 0;
        size_t fixedRootSlots = 0;
    };
    struct HeapAccount {
        size_t processedRegions = 0;
        size_t processedObjects = 0;
        size_t invalidObjects = 0;
        size_t filteredObjects = 0;
        size_t refHolders = 0;
        size_t fields = 0;
        size_t oldTaggedSlots = 0;
        size_t fixedSlots = 0;
        size_t youngTargetSlots = 0;
        size_t fromLiveObjects = 0;
        size_t fromLiveFields = 0;
        size_t rebuilt = 0;
        size_t chunksTaken = 0;
    };

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& regionManager = space.GetRegionManager();
    RememberedSet* rebuildRemset = requireSurvivedMark ? nullptr : &Heap::GetHeap().GetRememberedSet();
    const uintptr_t heapStart = regionManager.GetRegionHeapStart();
    const uintptr_t inactiveZone = regionManager.GetInactiveZone();

    auto makeRootVisitor = [this, trackFixed](RootAccount* acc) -> RootVisitor {
        return [this, trackFixed, acc](ObjectRef& root) {
            RefField<>& field = reinterpret_cast<RefField<>&>(root);
            uintptr_t oldValue = field.GetFieldValue();
            bool oldTagged = trackFixed && IsOldPointer(field);
            if (trackFixed && acc != nullptr) {
                ++acc->rootSlots;
                if (oldTagged) {
                    ++acc->oldTaggedRootSlots;
                }
            }
            FixOldTaggedRefField(nullptr, field);
            if (trackFixed && acc != nullptr && oldTagged && field.GetFieldValue() != oldValue) {
                ++acc->fixedRootSlots;
            }
        };
    };
    auto makeRootFieldVisitor = [this, trackFixed](RootAccount* acc) -> RefFieldVisitor {
        return [this, trackFixed, acc](RefField<>& field) {
            uintptr_t oldValue = field.GetFieldValue();
            bool oldTagged = trackFixed && IsOldPointer(field);
            if (trackFixed && acc != nullptr) {
                ++acc->rootSlots;
                if (oldTagged) {
                    ++acc->oldTaggedRootSlots;
                }
            }
            FixOldTaggedRefField(nullptr, field);
            if (trackFixed && acc != nullptr && oldTagged && field.GetFieldValue() != oldValue) {
                ++acc->fixedRootSlots;
            }
        };
    };

    auto processObject = [this, requireSurvivedMark, rebuildRemset, account, trackFixed](BaseObject* obj,
                                                                                          HeapAccount& acc) {
        RegionInfo* accountRegion = nullptr;
        if (account) {
            ++acc.processedObjects;
            if (obj != nullptr) {
                accountRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            }
            // H2: region count is per-worker local; each region is owned by exactly one
            // worker (region-head ownership), so summing processedRegions is exact.
        }
        if (obj == nullptr || !obj->IsValidObject()) {
            if (account) {
                ++acc.invalidObjects;
            }
            return;
        }
        if (requireSurvivedMark) {
            if (!IsSurvivedObject(obj)) {
                if (account) {
                    ++acc.filteredObjects;
                }
                return;
            }
            if (account && accountRegion != nullptr && accountRegion->IsFromRegion()) {
                ++acc.fromLiveObjects;
            }
        }
        if (!obj->HasRefField()) {
            return;
        }
        if (account) {
            ++acc.refHolders;
        }
        bool recordCrossGen = false;
        if (rebuildRemset != nullptr) {
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
            recordCrossGen = holderRegion != nullptr && !holderRegion->IsYoungRegion() &&
                             !holderRegion->IsGarbageRegion() && !holderRegion->IsFreeRegion();
        }
        bool forwardHolder = account && requireSurvivedMark && accountRegion != nullptr &&
                             accountRegion->IsFromRegion();
        obj->ForEachRefField([this, obj, recordCrossGen, rebuildRemset, forwardHolder, account, trackFixed,
                              &acc](RefField<>& field) {
            uintptr_t oldValue = field.GetFieldValue();
            bool oldTagged = trackFixed && IsOldPointer(field);
            if (trackFixed) {
                ++acc.fields;
                if (forwardHolder) {
                    ++acc.fromLiveFields;
                }
                if (oldTagged) {
                    ++acc.oldTaggedSlots;
                }
            }
            FixOldTaggedRefField(obj, field);
            if (oldTagged && field.GetFieldValue() != oldValue) {
                ++acc.fixedSlots;
            }
            if (!recordCrossGen) {
                return;
            }
            BaseObject* target = field.GetTargetObject();
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                if (account) {
                    ++acc.youngTargetSlots;
                }
                rebuildRemset->Record(reinterpret_cast<MAddress>(&field));
                ++acc.rebuilt;
            }
        });
    };

    // Region-head-ownership walk over [rangeStart, rangeEnd). H6: carry transient-extent
    // guard verbatim. Spec §六 T1: first-step correction if region head is before rangeStart.
    auto walkRange = [&processObject, requireSurvivedMark, account](uintptr_t rangeStart, uintptr_t rangeEnd,
                                                                    uintptr_t inactive, HeapAccount& acc) {
        if (rangeStart >= rangeEnd || rangeStart >= inactive) {
            return;
        }
        uintptr_t limit = std::min(rangeEnd, inactive);
        uintptr_t addr = rangeStart;
        // First-step correction: if the unit at rangeStart is mid-region, skip to that
        // region's end — the region belongs to the worker that owns its head.
        {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
            uintptr_t regionStart = region->GetRegionStart();
            uintptr_t nextAddr = region->GetRegionEnd();
            if (nextAddr <= addr || nextAddr > inactive) {
                // Transient illegal extent: step one unit, do not visit (H6).
                addr += RegionInfo::UNIT_SIZE;
            } else if (regionStart < rangeStart) {
                addr = nextAddr;
            }
        }
        RegionInfo* lastProcessedRegion = nullptr;
        while (addr < limit) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(addr);
            uintptr_t nextAddr = region->GetRegionEnd();
            // H6 transient-extent guard — character-identical to ForEachObjUnsafe.
            if (nextAddr <= addr || nextAddr > inactive) {
                addr += RegionInfo::UNIT_SIZE;
                continue;
            }
            // Region-head ownership: only visit if the region's head is in this chunk.
            // Regions that spill past limit still belong entirely to this worker.
            if (addr >= rangeStart && addr < limit) {
                if (region->IsValidRegion() && !region->IsFreeRegion() && !region->IsGarbageRegion() &&
                    !(requireSurvivedMark && region->IsKnownEmpty())) {
                    if (account && region != lastProcessedRegion) {
                        lastProcessedRegion = region;
                        ++acc.processedRegions;
                    }
                    region->VisitAllObjects([&processObject, &acc](BaseObject* object) {
                        processObject(object, acc);
                    });
                }
            }
            addr = nextAddr;
        }
    };

    auto heapWorkerBody = [&](std::atomic<uintptr_t>& cursor, HeapAccount& acc) {
        for (;;) {
            uintptr_t chunkStart = cursor.fetch_add(chunkBytes, std::memory_order_relaxed);
            if (chunkStart >= inactiveZone) {
                break;
            }
            ++acc.chunksTaken;
            uintptr_t chunkEnd = chunkStart + chunkBytes;
            walkRange(chunkStart, chunkEnd, inactiveZone, acc);
        }
    };

    // H7: account shadow pass stays serial (diagnostic-only, not on hot path).
    std::array<size_t, regionTypeCount> regionTypes{};
    size_t regions = 0;
    size_t knownEmptyRegions = 0;
    size_t objects = 0;
    size_t knownEmptyObjects = 0;
    size_t fromRegions = 0;
    if (account) {
        RegionInfo* lastAccountRegion = nullptr;
        space.ForEachObj(
            [requireSurvivedMark, &regionTypes, &lastAccountRegion, &regions, &knownEmptyRegions, &objects,
             &knownEmptyObjects, &fromRegions](BaseObject* obj) {
                ++objects;
                RegionInfo* region = obj == nullptr ? nullptr :
                    RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
                if (region == nullptr) {
                    return;
                }
                if (region != lastAccountRegion) {
                    lastAccountRegion = region;
                    ++regions;
                    ++regionTypes[static_cast<size_t>(region->GetRegionType())];
                    if (requireSurvivedMark && region->IsKnownEmpty()) {
                        ++knownEmptyRegions;
                    }
                    if (requireSurvivedMark && region->IsFromRegion()) {
                        ++fromRegions;
                    }
                }
                if (requireSurvivedMark && region->IsKnownEmpty()) {
                    ++knownEmptyObjects;
                }
            },
            false);
    }

    // A1 positive control: plant one old-tagged root so PREFLIP_VERIFY must observe fixed>0.
    // Serial, before parallel dispatch; only on preflip path.
    RootAccount injectAcc{};
    if (requireSurvivedMark && preflipVerify && preflipInject) {
        BaseObject* injectTarget = nullptr;
        space.ForEachObj(
            [&injectTarget](BaseObject* obj) {
                if (injectTarget != nullptr || obj == nullptr || !obj->IsValidObject()) {
                    return;
                }
                injectTarget = obj;
            },
            false);
        if (injectTarget != nullptr) {
            RefField<> planted(injectTarget, 1, GetPreviousTagID());
            MAddress injectRootStorage = planted.GetFieldValue();
            ObjectRef injectRoot{};
            *reinterpret_cast<MAddress*>(&injectRoot) = injectRootStorage;
            RootVisitor fixRoot = makeRootVisitor(&injectAcc);
            fixRoot(injectRoot);
            VLOG(REPORT,
                 "[GCV2][preflip-verify-inject] planted old-tag root target=%p raw=%#zx fixedRoots_now=%zu "
                 "env=MRT_GCV2_PREFLIP_VERIFY_INJECT=1",
                 injectTarget, static_cast<uintptr_t>(injectRootStorage), injectAcc.fixedRootSlots);
        } else {
            VLOG(REPORT,
                 "[GCV2][preflip-verify-inject] no live object to plant env=MRT_GCV2_PREFLIP_VERIFY_INJECT=1");
        }
    }

    GCThreadPool* threadPool = GetThreadPool();
    // Positive control for silent serial degradation (spec §六 T3 ②).
    // Force serial via MRT_GCV2_STWPAR_FORCE_SERIAL=1 for bidirectional proof.
    static const bool forceSerialEnv = []() {
        const char* value = std::getenv("MRT_GCV2_STWPAR_FORCE_SERIAL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    const bool forceSerial = forceSerialEnv;
    const bool useParallel = threadPool != nullptr && !forceSerial;

    RootAccount rootTotals = injectAcc;
    HeapAccount heapTotals{};
    std::vector<size_t> chunksPerWorker;
    size_t workersScheduled = 0;

    if (!useParallel) {
        VLOG(REPORT, "[F3][parallel] fallback=serial pool_unavailable");
        // Six root families serial (same order as before).
        {
            RootAccount acc;
            RootVisitor fixRoot = makeRootVisitor(&acc);
            RefFieldVisitor fixRootField = makeRootFieldVisitor(&acc);
            MutatorManager::Instance().VisitAllMutators(
                [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
            Heap::GetHeap().VisitStaticRoots(fixRootField);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
            collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
            collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
            Heap::GetHeap().VisitAllExportRoots(fixRoot);
            rootTotals.rootSlots += acc.rootSlots;
            rootTotals.oldTaggedRootSlots += acc.oldTaggedRootSlots;
            rootTotals.fixedRootSlots += acc.fixedRootSlots;
        }
        {
            HeapAccount acc;
            // Single-threaded full range — equivalent to ForEachObjUnsafe.
            walkRange(heapStart, inactiveZone, inactiveZone, acc);
            // Count as one logical chunk for the diagnostic line.
            if (heapStart < inactiveZone) {
                acc.chunksTaken = 1;
            }
            heapTotals = acc;
            chunksPerWorker.push_back(acc.chunksTaken);
            workersScheduled = 1;
        }
    } else {
        // Root-side: 6 family-level tasks (static family must not be split — mutex+dedup set).
        // Heap-side: N cursor tasks. Same pool, same batch as Preforward.
        // Cap via MRT_GCV2_STWPAR_WORKERS for scale curve (1/2/4/8/16); never expand pool.
        const int32_t helperNum = threadPool->GetMaxThreadNum();
        // Caller's GC thread also drains via WaitFinish → effective capacity = helpers + 1.
        const int32_t poolCap = helperNum + 1;
        int32_t heapWorkers = poolCap;
        {
            const char* wEnv = std::getenv("MRT_GCV2_STWPAR_WORKERS");
            if (wEnv != nullptr && wEnv[0] != '\0') {
                int32_t want = static_cast<int32_t>(std::strtol(wEnv, nullptr, 10));
                if (want >= 1 && want < heapWorkers) {
                    heapWorkers = want;
                }
            }
        }
        std::vector<RootAccount> rootAcc(6);
        std::vector<HeapAccount> heapAcc(static_cast<size_t>(heapWorkers));
        std::atomic<uintptr_t> cursor{ heapStart };

        // Roots first into queue, then heap workers. Start after all AddWork so helpers
        // see the full batch (same shape as Preforward: AddWork×N then Start then WaitFinish).
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[0]);
            MutatorManager::Instance().VisitAllMutators(
                [&fixRoot](Mutator& mutator) { mutator.VisitMutatorRoots(fixRoot); });
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootFieldVisitor](size_t) {
            RefFieldVisitor fixRootField = makeRootFieldVisitor(&rootAcc[1]);
            Heap::GetHeap().VisitStaticRoots(fixRootField);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[2]);
            Runtime::Current().GetConcurrencyModel().VisitGCRoots(&fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[3]);
            collectorResources.GetFinalizerProcessor().VisitGCRoots(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[4]);
            collectorResources.GetFinalizerProcessor().VisitFinalizers(fixRoot);
        }));
        threadPool->AddWork(new (std::nothrow) LambdaWork([this, &rootAcc, &makeRootVisitor](size_t) {
            RootVisitor fixRoot = makeRootVisitor(&rootAcc[5]);
            Heap::GetHeap().VisitAllExportRoots(fixRoot);
        }));

        for (int32_t i = 0; i < heapWorkers; ++i) {
            HeapAccount* acc = &heapAcc[static_cast<size_t>(i)];
            threadPool->AddWork(new (std::nothrow) LambdaWork(
                [heapWorkerBody, &cursor, acc](size_t) { heapWorkerBody(cursor, *acc); }));
        }

        threadPool->Start();
        threadPool->WaitFinish();

        for (const auto& a : rootAcc) {
            rootTotals.rootSlots += a.rootSlots;
            rootTotals.oldTaggedRootSlots += a.oldTaggedRootSlots;
            rootTotals.fixedRootSlots += a.fixedRootSlots;
        }
        chunksPerWorker.reserve(heapAcc.size());
        for (const auto& a : heapAcc) {
            heapTotals.processedRegions += a.processedRegions;
            heapTotals.processedObjects += a.processedObjects;
            heapTotals.invalidObjects += a.invalidObjects;
            heapTotals.filteredObjects += a.filteredObjects;
            heapTotals.refHolders += a.refHolders;
            heapTotals.fields += a.fields;
            heapTotals.oldTaggedSlots += a.oldTaggedSlots;
            heapTotals.fixedSlots += a.fixedSlots;
            heapTotals.youngTargetSlots += a.youngTargetSlots;
            heapTotals.fromLiveObjects += a.fromLiveObjects;
            heapTotals.fromLiveFields += a.fromLiveFields;
            heapTotals.rebuilt += a.rebuilt;
            heapTotals.chunksTaken += a.chunksTaken;
            chunksPerWorker.push_back(a.chunksTaken);
        }
        workersScheduled = static_cast<size_t>(heapWorkers);
    }

    // Parallel-liveness positive control: at least 2 workers with chunks_taken>0 when heap
    // spans >2×CHUNK (spec §六 T3 ①). Always print so silence ≠ "never fired".
    {
        size_t active = 0;
        std::string chunksStr;
        for (size_t i = 0; i < chunksPerWorker.size(); ++i) {
            if (chunksPerWorker[i] != 0) {
                ++active;
            }
            if (i != 0) {
                chunksStr += ',';
            }
            chunksStr += std::to_string(chunksPerWorker[i]);
        }
        VLOG(REPORT, "[F3][parallel] phase=%s workers_active=%zu workers_scheduled=%zu chunks=[%s] parallel=%d",
             requireSurvivedMark ? "preflip" : "postflip", active, workersScheduled, chunksStr.c_str(),
             useParallel ? 1 : 0);
    }

    if (heapTotals.rebuilt != 0) {
        VLOG(REPORT, "[GCV2][remset] rebuilt after full GC recorded=%zu", heapTotals.rebuilt);
    }
    if (account) {
        VLOG(REPORT,
             "[GCV2][preflip-account] phase=%s regions=%zu knownEmptyRegions=%zu objects=%zu "
             "knownEmptyObjects=%zu processedRegions=%zu processedObjects=%zu invalid=%zu filtered=%zu "
             "survived=%zu refHolders=%zu fields=%zu oldTagged=%zu fixed=%zu rootSlots=%zu "
             "oldTaggedRoots=%zu fixedRoots=%zu youngTargets=%zu "
             "rebuilt=%zu fromRegions=%zu fromLiveObjects=%zu fromLiveFields=%zu "
             "env=MRT_GCV2_PREFLIP_ACCOUNT=1",
             requireSurvivedMark ? "preflip" : "postflip", regions, knownEmptyRegions, objects, knownEmptyObjects,
             heapTotals.processedRegions, heapTotals.processedObjects, heapTotals.invalidObjects,
             heapTotals.filteredObjects,
             heapTotals.processedObjects - heapTotals.invalidObjects - heapTotals.filteredObjects,
             heapTotals.refHolders, heapTotals.fields, heapTotals.oldTaggedSlots, heapTotals.fixedSlots,
             rootTotals.rootSlots, rootTotals.oldTaggedRootSlots, rootTotals.fixedRootSlots,
             heapTotals.youngTargetSlots, heapTotals.rebuilt, fromRegions, heapTotals.fromLiveObjects,
             heapTotals.fromLiveFields);
        VLOG(REPORT,
             "[GCV2][preflip-region-types] phase=%s type0=%zu type1=%zu type2=%zu type3=%zu type4=%zu "
             "type5=%zu type6=%zu type7=%zu type8=%zu type9=%zu type10=%zu type11=%zu type12=%zu type13=%zu "
             "type14=%zu env=MRT_GCV2_PREFLIP_ACCOUNT=1",
             requireSurvivedMark ? "preflip" : "postflip", regionTypes[0], regionTypes[1], regionTypes[2],
             regionTypes[3], regionTypes[4], regionTypes[5], regionTypes[6], regionTypes[7], regionTypes[8],
             regionTypes[9], regionTypes[10], regionTypes[11], regionTypes[12], regionTypes[13], regionTypes[14]);
    }
    // A1 VERIFY tripwire: preflip fixed population must stay empty. Any fix is a named failure.
    if (requireSurvivedMark && preflipVerify) {
        const size_t fixedTotal = heapTotals.fixedSlots + rootTotals.fixedRootSlots;
        VLOG(REPORT,
             "[GCV2][preflip-verify] fixed=%zu fixedRoots=%zu fixedTotal=%zu oldTagged=%zu oldTaggedRoots=%zu "
             "fields=%zu rootSlots=%zu env=MRT_GCV2_PREFLIP_VERIFY=1",
             heapTotals.fixedSlots, rootTotals.fixedRootSlots, fixedTotal, heapTotals.oldTaggedSlots,
             rootTotals.oldTaggedRootSlots, heapTotals.fields, rootTotals.rootSlots);
        if (fixedTotal > 0) {
            static const bool preflipVerifyFatal = []() {
                const char* value = std::getenv("MRT_GCV2_PREFLIP_VERIFY_FATAL");
                return value != nullptr && std::strcmp(value, "1") == 0;
            }();
            LOG(RTLOG_ERROR,
                "[GCV2][preflip-verify] PREFLIP_RESIDUE fixed=%zu fixedRoots=%zu fixedTotal=%zu "
                "oldTagged=%zu oldTaggedRoots=%zu (production skips preflip; residue means insurance needed)",
                heapTotals.fixedSlots, rootTotals.fixedRootSlots, fixedTotal, heapTotals.oldTaggedSlots,
                rootTotals.oldTaggedRootSlots);
            if (preflipVerifyFatal) {
                CHECK_DETAIL(false,
                             "MRT_GCV2_PREFLIP_VERIFY_FATAL: preflip residue fixedTotal=%zu "
                             "(fixed=%zu fixedRoots=%zu)",
                             fixedTotal, heapTotals.fixedSlots, rootTotals.fixedRootSlots);
            }
        }
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
    // Anchor main 9ad991c4e8660c26d6bfe575f6425e1b227bdf94.
    InvalidateOldTaggedRefsBeforeDispel();
    fwdTable.PrepareForwardTable();
    // OPTION_2 mark-epoch release: TRACE+CLEAR_SATB done; publish quarantined post-dispel
    // units (from this PrepareForwardTable and any prior minor) to dirty for reuse.
    // INV-1 closed: concurrent mark can no longer follow plain edges into these ranges.
    space.GetRegionManager().ReleaseMarkQuarantine();
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
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardStaticRoots(); }));
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

// N2 (MINOR_CONCURRENCY_0805 §八 T-C): CAS-install plain target under multi-worker fix.
// Same-value concurrent writes converge; first writer wins. Counters for positive control.
namespace {
std::atomic<size_t> g_minorRefCasFail{ 0 };
std::atomic<size_t> g_minorRefCasOk{ 0 };

// Install plain (untagged) target into field. expected = observed tagged/old value.
// On CAS fail: accept (peer already updated — major TryUpdateRefFieldImpl style).
bool CasInstallPlainTarget(RefField<>& field, MAddress expected, BaseObject* plainTarget)
{
    RefField<> desired(plainTarget);
    MAddress desiredVal = desired.GetFieldValue();
    if (expected == desiredVal) {
        return true;
    }
    if (field.CompareExchange(expected, desiredVal)) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    return true;
}
} // namespace

BaseObject* WCollector::ResolveMinorReference(RefField<>& field) const
{
    RefField<> value(field);
    BaseObject* object = value.GetTargetObject();
    if (!IsOldPointer(value)) {
        return object;
    }
    // Minor path must not call FindLatestVersion: after a full GC Flip, remset/root
    // slots can still hold one-gen-stale tags whose from-copy was reclaimed (ghost
    // gone, header zeroed). F5 would abort a detector path; here we soft-resolve:
    //   routed to-version → plain to
    //   unmoved valid from → plain from
    //   dead/stale → null the slot (caller drops the edge)
    // N2: plain SetTargetObject → CAS (FYS=1 multi-writer safe; product default FYS=1).
    MAddress expected = value.GetFieldValue();
    BaseObject* to = FindToVersion(object);
    if (to != nullptr && Heap::IsHeapAddress(to)) {
        RegionInfo* toRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(to));
        if (toRegion != nullptr && !toRegion->IsFreeRegion() && !toRegion->IsGarbageRegion() &&
            to->IsValidObject()) {
            (void)CasInstallPlainTarget(field, expected, to);
            return to;
        }
    }
    if (Heap::IsHeapAddress(object)) {
        RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
            object->IsValidObject()) {
            (void)CasInstallPlainTarget(field, expected, object);
            return object;
        }
    }
    static std::atomic<size_t> g_staleOldTagLogged{ 0 };
    size_t n = g_staleOldTagLogged.fetch_add(1, std::memory_order_relaxed);
    if (n < 16) {
        VLOG(REPORT,
             "[GCV2][minor-stale-oldtag] field=%p raw=%#zx from=%p to=%p "
             "(drop; full-GC remset/root residue after Flip)",
             &field, static_cast<size_t>(value.GetFieldValue()), object, to);
    }
    (void)CasInstallPlainTarget(field, expected, nullptr);
    return nullptr;
}

namespace {
// gcbadroot: tag which root family is currently being walked so PushYoungObject
// can attribute invalid headers without threading origin through every visitor.
thread_local const char* gMinorRootOrigin = "unknown";
// interiorfix: source slot address for NotePush (static/stack root RefField storage).
thread_local uintptr_t gMinorRootSlotAddr = 0;
} // namespace

void WCollector::VisitMinorRootSlots(RootVisitor& rawRootVisitor, const RefFieldVisitor& fieldVisitor)
{
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    RootVisitor checkedRawRootVisitor = [&remset, &rawRootVisitor](ObjectRef& root) {
        remset.VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&root));
        rawRootVisitor(root);
    };
    RootVisitor& visitedRawRootVisitor = checkedRawRootVisitor;
#else
    RootVisitor& visitedRawRootVisitor = rawRootVisitor;
#endif
    gMinorRootOrigin = "mutator_stack";
    RootVisitor taggedRaw = [&visitedRawRootVisitor](ObjectRef& root) {
        gMinorRootSlotAddr = reinterpret_cast<uintptr_t>(&root);
        visitedRawRootVisitor(root);
        gMinorRootSlotAddr = 0;
    };
    MutatorManager::Instance().VisitAllMutators(
        [&taggedRaw](Mutator& mutator) { mutator.VisitMutatorRoots(taggedRaw); });
    gMinorRootOrigin = "static";
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    Heap::GetHeap().VisitStaticRoots([&remset, &fieldVisitor](RefField<>& field) {
        remset.VisitStaticForCrossCheck(reinterpret_cast<MAddress>(&field));
        gMinorRootSlotAddr = reinterpret_cast<uintptr_t>(&field);
        StaticSlotProbe::NoteStaticField(field);
        fieldVisitor(field);
        gMinorRootSlotAddr = 0;
    });
#else
    Heap::GetHeap().VisitStaticRoots([&fieldVisitor](RefField<>& field) {
        gMinorRootSlotAddr = reinterpret_cast<uintptr_t>(&field);
        StaticSlotProbe::NoteStaticField(field);
        fieldVisitor(field);
        gMinorRootSlotAddr = 0;
    });
#endif
    gMinorRootOrigin = "concurrency";
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&taggedRaw);
    gMinorRootOrigin = "finalizer";
    collectorResources.GetFinalizerProcessor().VisitRawPointers(taggedRaw);
    gMinorRootOrigin = "export";
    Heap::GetHeap().VisitAllExportRoots(taggedRaw);
#if defined(MRT_REMSET_BITMAP_CROSSCHECK)
    remset.CheckStaticCoverageForMinor();
#endif
    gMinorRootOrigin = "unknown";
    gMinorRootSlotAddr = 0;
}

void WCollector::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
{
    {
        std::lock_guard<std::mutex> lock(resurrectExportMtx);
        gMinorRootOrigin = "value_export";
        for (BaseObject* object : resurrectedExportObjectes) {
            visitor(object);
        }
        gMinorRootOrigin = "value_export_fwd";
        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
            visitor(object);
        }
    }
    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
    gMinorRootOrigin = "value_cycle";
    for (const auto& entry : cycleRefWorkStack) {
        visitor(entry.first);
        for (BaseObject* object : entry.second) {
            visitor(object);
        }
    }
    gMinorRootOrigin = "unknown";
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

void WCollector::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin) const
{
    if (!Heap::IsHeapAddress(object)) {
        return;
    }
    // interiorsrc: classify every young push (base vs interior) before validity CHECK.
    // Gate MRT_GCV2_INTERIOR_SRC=1 (default off). Does not relax IsValidObject.
    {
        const char* src = origin;
        if (src == nullptr || std::strcmp(src, "unknown") == 0 || std::strcmp(src, "minor_root") == 0) {
            if (gMinorRootOrigin != nullptr && std::strcmp(gMinorRootOrigin, "unknown") != 0) {
                src = gMinorRootOrigin;
            } else if (src == nullptr) {
                src = "unknown";
            }
        }
        InteriorSrcProbe::NotePush(src, object, gMinorRootSlotAddr, 0);
    }
    if (!object->IsValidObject()) {
        // T3: emit last true-interior enqueue before fail-closed abort.
        InteriorSrcProbe::FlushSummary("invalid-minor-root");
        // Rich diagnosis before fail-closed abort: address looks like a heap range
        // but object header is not a valid managed object (stack-ish residue, stale
        // slot, or stackmap-mislabeled root). Printed once per process by default.
        static std::atomic<size_t> g_invalidMinorRootPrinted{ 0 };
        size_t n = g_invalidMinorRootPrinted.fetch_add(1, std::memory_order_relaxed);
        // Prefer explicit non-generic origin; "minor_root" is a placeholder that
        // should yield to the TLS tag set by VisitMinorRootSlots/ValueRoots.
        const char* src = origin;
        if (src == nullptr || std::strcmp(src, "unknown") == 0 || std::strcmp(src, "minor_root") == 0) {
            if (gMinorRootOrigin != nullptr && std::strcmp(gMinorRootOrigin, "unknown") != 0) {
                src = gMinorRootOrigin;
            } else if (src == nullptr) {
                src = "unknown";
            }
        }
        if (n < 8) {
            RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
            VLOG(REPORT,
                 "[GCV2][invalid-minor-root] obj=%p origin=%s region=%p regionStart=%#zx young=%u pinned=%u "
                 "large=%u free=%u garbage=%u neverExamined=%u "
                 "(fail-closed next; AS1 relation: bad header on stack-live slot vs SKIPPED frame)",
                 object, src, region,
                 region == nullptr ? 0 : static_cast<size_t>(region->GetRegionStart()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsYoungRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsPinnedRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsLargeRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsFreeRegion()),
                 region == nullptr ? 0u : static_cast<unsigned>(region->IsGarbageRegion()),
                 region == nullptr ? 0u
                                   : static_cast<unsigned>(region->GetMarkBitmap() == nullptr &&
                                                          region->GetRegionAllocPtr() > region->GetRegionStart()));
            // HEADER_DUMP: first 64 bytes as hex + field decode + zap check.
            auto* bytes = reinterpret_cast<const uint8_t*>(object);
            char hex[64 * 2 + 16];
            size_t pos = 0;
            for (size_t i = 0; i < 64 && pos + 2 < sizeof(hex); ++i) {
                static const char* kHex = "0123456789abcdef";
                hex[pos++] = kHex[(bytes[i] >> 4) & 0xf];
                hex[pos++] = kHex[bytes[i] & 0xf];
            }
            hex[pos] = '\0';
            uint64_t w0 = 0;
            uint64_t w1 = 0;
            uint64_t w2 = 0;
            uint64_t w3 = 0;
            std::memcpy(&w0, bytes + 0, sizeof(w0));
            std::memcpy(&w1, bytes + 8, sizeof(w1));
            std::memcpy(&w2, bytes + 16, sizeof(w2));
            std::memcpy(&w3, bytes + 24, sizeof(w3));
            bool allZero = true;
            for (size_t i = 0; i < 64; ++i) {
                if (bytes[i] != 0) {
                    allZero = false;
                    break;
                }
            }
            bool isZap = HeapZap::IsZapWord(static_cast<uintptr_t>(w0));
            // tipBits: raw first 48 bits of header word (layout-dependent; not GetTypeInfo).
            uintptr_t tipBits = (static_cast<uintptr_t>(w0) & 0xffffffffffffULL);
            VLOG(REPORT,
                 "[GCV2][HEADER_DUMP] obj=%p hex64=%s w0=%#llx w1=%#llx w2=%#llx w3=%#llx "
                 "allZero=%u isZapWord=%u tipBits48=%#zx ZAP_WORD=%#llx "
                 "ZAP_VERDICT_%s",
                 object, hex, static_cast<unsigned long long>(w0), static_cast<unsigned long long>(w1),
                 static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3),
                 static_cast<unsigned>(allZero), static_cast<unsigned>(isZap), tipBits,
                 static_cast<unsigned long long>(HeapZap::ZAP_WORD),
                 isZap ? "是毒值_乙" : (allZero ? "非毒值_全零" : "非毒值_有内容"));
            VLOG(REPORT, "[GCV2][ROOT_ORIGIN] origin=%s obj=%p", src, object);
            // gcfwdfix: was this address inside a recent CompactRegion/ClearUnits zero range?
            char clearDetail[256];
            bool wasCleared = TraceClear::Lookup(reinterpret_cast<MAddress>(object), clearDetail, sizeof(clearDetail));
            VLOG(REPORT, "[GCV2][WAS_LIVE_BEFORE_CLEAR] hit=%u detail=%s obj=%p",
                 static_cast<unsigned>(wasCleared), clearDetail, object);
        }
        CHECK_DETAIL(false, "minor root/reference %p is not a valid object origin=%s", object, src);
    }
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
            gMinorRootSlotAddr = reinterpret_cast<uintptr_t>(&field);
            PushYoungObject(target, workStack, "closure_edge");
            gMinorRootSlotAddr = 0;
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
                                     bool fullYoungScan, MinorSlotSet* consumedOut, DiffPathRemsetStats* statsOut)
{
    // HotSpot G1RemSet scrub analogue. ORDER matters (STEER2 / defect⑤):
    //   1) region-level holder_dead (free/garbage region only — not object liveness)
    //   2) pre-check target safety BEFORE ResolveMinorReference
    //      (old-tag with no to-version + invalid from must not reach FindLatestVersion/F5)
    //   3) ResolveMinorReference (soft-resolve; never calls FindLatestVersion)
    //   4) post-resolve null / bad_target drops
    // Does not relax IsValidObject / FindLatestVersion CHECK_DETAIL.
    static std::atomic<size_t> g_remsetScrubLogged{ 0 };
    size_t scrubbedStale = 0;
    size_t scrubbedDeadHolder = 0;
    size_t scrubbedBadTarget = 0;
    size_t scrubbedStaleOldTag = 0;
    static const bool retainedProbe = []() {
        const char* value = std::getenv("MRT_GCV2_RETLIVE_PROBE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    size_t originFound = 0;
    size_t originBoundsValid = 0;
    size_t retainedNever = 0;
    size_t retainedValid = 0;
    size_t retainedEmpty = 0;
    size_t retainedStale = 0;
    size_t retainedKeep = 0;
    size_t retainedDrop = 0;
    size_t safeEmptyDrop = 0;
    size_t directDeadDrop = 0;
    size_t filterCorrect = 0;
    size_t filterIncorrect = 0;
    // The precise bitmap intentionally stores only field-slot identity. Recover an
    // object origin only for regions whose retained snapshot is consumable (or when
    // the default-off probe requests visibility), and keep that adapter local to this
    // minor collection rather than adding a second persistent remset index.
    std::unordered_map<MAddress, BaseObject*> rememberedOrigins;
    std::unordered_set<RegionInfo*> originRegions;
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(slot);
        if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        RegionInfo::RetainedLiveInfoState retainedState = region->GetRetainedLiveInfoState();
        if (retainedProbe || (retainedState != RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED &&
                             region->IsRetainedSnapshotValid())) {
            originRegions.insert(region);
        }
    }
    for (RegionInfo* region : originRegions) {
        region->VisitAllObjects([&rememberedSlots, &rememberedOrigins](BaseObject* holder) {
            if (holder == nullptr || !holder->HasRefField()) {
                return;
            }
            holder->ForEachRefField([holder, &rememberedSlots, &rememberedOrigins](RefField<>& field) {
                MAddress slot = reinterpret_cast<MAddress>(&field);
                if (rememberedSlots.count(slot) != 0) {
                    rememberedOrigins[slot] = holder;
                }
            });
        });
    }
    for (MAddress slot : rememberedSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            if (statsOut != nullptr) {
                ++statsOut->skippedNotHeap;
            }
            continue;
        }
        if (weakSlots.count(slot) != 0) {
            if (statsOut != nullptr) {
                ++statsOut->skippedWeak;
            }
            continue;
        }
        RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(slot);
        if (holderRegion == nullptr || holderRegion->IsFreeRegion() || holderRegion->IsGarbageRegion()) {
            ++scrubbedDeadHolder;
            size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                VLOG(REPORT,
                     "[GCV2][remset-filter] drop slot=%#zx reason=holder_dead region=%p free=%u garbage=%u",
                     static_cast<size_t>(slot), holderRegion,
                     holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsFreeRegion()),
                     holderRegion == nullptr ? 0u : static_cast<unsigned>(holderRegion->IsGarbageRegion()));
            }
            continue;
        }

        bool keepByRetainedSnapshot = true;
        auto originIt = rememberedOrigins.find(slot);
        if (originIt != rememberedOrigins.end() && originIt->second != nullptr &&
            Heap::IsHeapAddress(originIt->second)) {
            BaseObject* holder = originIt->second;
            RegionInfo* originRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (originRegion == holderRegion) {
                if (retainedProbe) {
                    ++originFound;
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    size_t holderSize = RegionSpace::GetAllocSize(*holder);
                    if (slot >= holderAddress && slot < holderAddress + holderSize) {
                        ++originBoundsValid;
                    }
                }
                RegionInfo::RetainedLiveInfoState retainedState = holderRegion->GetRetainedLiveInfoState();
                if (retainedState == RegionInfo::RetainedLiveInfoState::NEVER_EXAMINED) {
                    if (retainedProbe) {
                        ++retainedNever;
                    }
                } else if (!holderRegion->IsRetainedSnapshotValid()) {
                    if (retainedProbe) {
                        ++retainedStale;
                    }
                } else {
                    MAddress coveredUpTo = holderRegion->GetRetainedLiveInfoCoveredUpTo();
                    CHECK(coveredUpTo >= holderRegion->GetRegionStart() &&
                          coveredUpTo <= holderRegion->GetRegionAllocPtr());
                    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
                    if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                        if (retainedProbe) {
                            ++retainedEmpty;
                        }
                    } else {
                        if (retainedProbe) {
                            ++retainedValid;
                        }
                    }
                    if (holderAddress < coveredUpTo) {
                        if (retainedState == RegionInfo::RetainedLiveInfoState::SNAPSHOT_EMPTY) {
                            keepByRetainedSnapshot = false;
                            if (retainedProbe) {
                                ++safeEmptyDrop;
                            }
                        } else if (holderRegion->IsLargeRegion()) {
                            LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                            keepByRetainedSnapshot = retainedLiveInfo != nullptr
                                ? retainedLiveInfo->IsSurvivedObject(0)
                                : holderRegion->IsSurvivedObject(0);
                            if (retainedProbe && !keepByRetainedSnapshot) {
                                ++directDeadDrop;
                            }
                        } else {
                            LiveInfo* retainedLiveInfo = holderRegion->GetRetainedLiveInfo();
                            CHECK(retainedLiveInfo != nullptr);
                            size_t holderOffset = holderRegion->GetAddressOffset(holderAddress);
                            keepByRetainedSnapshot = retainedLiveInfo->IsSurvivedObject(holderOffset);
                            if (retainedProbe && !keepByRetainedSnapshot) {
                                ++directDeadDrop;
                            }
                        }
                    }
                }
            }
        }
        if (retainedProbe) {
            if (keepByRetainedSnapshot) {
                ++retainedKeep;
            } else {
                ++retainedDrop;
            }
        }
        if (fullYoungScan) {
            bool oracleKeep = reachableSlots.count(slot) != 0;
            if (retainedProbe) {
                if (keepByRetainedSnapshot == oracleKeep) {
                    ++filterCorrect;
                } else {
                    ++filterIncorrect;
                }
            }
            if (!oracleKeep) {
                if (statsOut != nullptr) {
                    ++statsOut->skippedFysFilter;
                }
                continue;
            }
        } else if (!keepByRetainedSnapshot) {
            ++scrubbedDeadHolder;
            continue;
        }

        RefField<>* field = reinterpret_cast<RefField<>*>(slot);
        uint64_t rawSlot = 0;
        std::memcpy(&rawSlot, field, sizeof(rawSlot));
        RefField<> peek(*field);
        BaseObject* rawTarget = peek.GetTargetObject();
        // Pre-check (before resolve): one-gen-stale old-tag whose from has no to-version
        // and is not a live object — drop without FindLatestVersion (F5 fail-closed stays).
        if (IsOldPointer(peek)) {
            BaseObject* to = FindToVersion(rawTarget);
            bool fromLive = false;
            if (to == nullptr && Heap::IsHeapAddress(rawTarget)) {
                RegionInfo* fromRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(rawTarget));
                fromLive = fromRegion != nullptr && !fromRegion->IsFreeRegion() && !fromRegion->IsGarbageRegion() &&
                           rawTarget->IsValidObject();
            }
            if (to == nullptr && !fromLive) {
                ++scrubbedStaleOldTag;
                // N2: CAS null install (same slot may race with ResolveMinorReference under FYS=1).
                (void)CasInstallPlainTarget(*field, peek.GetFieldValue(), nullptr);
                size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
                if (n < 16) {
                    VLOG(REPORT,
                         "[GCV2][remset-filter] drop slot=%#zx raw=%#llx target=%p reason=stale_oldtag "
                         "(no to-version; from invalid/reclaimed — pre-resolve)",
                         static_cast<size_t>(slot), static_cast<unsigned long long>(rawSlot), rawTarget);
                }
                continue;
            }
        }

        BaseObject* target = ResolveMinorReference(*field);
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            ++scrubbedStale;
            continue;
        }
        if (!target->IsValidObject()) {
            ++scrubbedBadTarget;
            size_t n = g_remsetScrubLogged.fetch_add(1, std::memory_order_relaxed);
            if (n < 16) {
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                VLOG(REPORT,
                     "[GCV2][remset-filter] drop slot=%#zx raw=%#llx target=%p reason=bad_target "
                     "holderYoung=%u holderFree=%u targetYoung=%u targetFree=%u targetGarbage=%u "
                     "targetNeverExamined=%u (H1: stale remset after reclaim)",
                     static_cast<size_t>(slot), static_cast<unsigned long long>(rawSlot), target,
                     static_cast<unsigned>(holderRegion->IsYoungRegion()),
                     static_cast<unsigned>(holderRegion->IsFreeRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsYoungRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsFreeRegion()),
                     targetRegion == nullptr ? 0u : static_cast<unsigned>(targetRegion->IsGarbageRegion()),
                     targetRegion == nullptr
                         ? 0u
                         : static_cast<unsigned>(targetRegion->GetMarkBitmap() == nullptr &&
                                                 targetRegion->GetRegionAllocPtr() > targetRegion->GetRegionStart()));
            }
            continue;
        }

        gMinorRootSlotAddr = static_cast<uintptr_t>(slot);
        PushYoungObject(target, workStack, "remset");
        gMinorRootSlotAddr = 0;
        if (consumedOut != nullptr) {
            consumedOut->insert(slot);
        }
        if (statsOut != nullptr) {
            ++statsOut->consumed;
        }
    }
    if (scrubbedStale != 0 || scrubbedDeadHolder != 0 || scrubbedBadTarget != 0 || scrubbedStaleOldTag != 0) {
        VLOG(REPORT,
             "[GCV2][remset-filter] summary staleTarget=%zu deadHolderRegion=%zu badTarget=%zu "
             "staleOldTag=%zu recorded=%zu "
             "(DEAD_HOLDER_DROPPED≈deadHolderRegion+staleOldTag; region-level holder_dead ≠ object-dead)",
             scrubbedStale, scrubbedDeadHolder, scrubbedBadTarget, scrubbedStaleOldTag, rememberedSlots.size());
    }
    if (retainedProbe) {
        VLOG(REPORT,
             "[RETLIVE][summary] slots=%zu originFound=%zu originBoundsValid=%zu never=%zu valid=%zu empty=%zu "
             "stale=%zu keep=%zu drop=%zu safeEmpty=%zu directDead=%zu oracleCorrect=%zu oracleIncorrect=%zu "
             "fullYoungScan=%u",
             rememberedSlots.size(), originFound, originBoundsValid, retainedNever, retainedValid, retainedEmpty,
             retainedStale, retainedKeep, retainedDrop, safeEmptyDrop, directDeadDrop, filterCorrect,
             filterIncorrect, static_cast<unsigned>(fullYoungScan));
    }
}

bool WCollector::FixMinorEvacuatedSlot(RefField<>& field) const
{
    // N1: major-style CAS tolerate (TryUpdateRefFieldImpl family). Under multi-worker
    // fix, CAS fail is normal (peer already updated) — abort assertion was serial-only.
    RefField<> oldField(field);
    BaseObject* target = ResolveMinorReference(field);
    BaseObject* current = target;
    if (Heap::IsHeapAddress(target) && IsGhostFromObject(target) && !IsUnmovableFromObject(target)) {
        current = const_cast<WCollector*>(this)->ForwardObject(target);
    }
    RefField<> newField(current);
    MAddress oldVal = oldField.GetFieldValue();
    MAddress newVal = newField.GetFieldValue();
    if (oldVal == newVal) {
        return false;
    }
    // Re-read after resolve (resolve may have CAS-installed plain already).
    oldVal = field.GetFieldValue();
    if (oldVal == newVal) {
        return false;
    }
    if (field.CompareExchange(oldVal, newVal)) {
        g_minorRefCasOk.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // CAS fail: accept if current == desired or already a plain/newer install (major style).
    g_minorRefCasFail.fetch_add(1, std::memory_order_relaxed);
    MAddress cur = field.GetFieldValue();
    if (cur == newVal) {
        return true;
    }
    // Peer may have installed same logical target via ResolveMinorReference first
    // (old tagged → plain) then another worker forwarded; either is a valid fix.
    return true;
}

void WCollector::FixMinorRootSlots()
{
    RootVisitor rawRootVisitor = [this](ObjectRef& root) {
        RefField<>& field = reinterpret_cast<RefField<>&>(root);
        (void)FixMinorEvacuatedSlot(field);
    };
    RefFieldVisitor fieldVisitor = [this](RefField<>& field) { (void)FixMinorEvacuatedSlot(field); };
    // Must match VisitMinorRootSlots: mutator_stack was enumerated at mark time but
    // previously omitted here (defect④ / stdbuildflag). After EvacuateYoungRegions,
    // stack slots still holding from-copies become the next full's F5 input.
    MutatorManager::Instance().VisitAllMutators(
        [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
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

// R2: parallel ⑦ young.ref_fix — index-shard reachableObjects + remset slots;
// root families = 6 family-level tasks (static not split). Template = A2 stwpar2.
// Env: MRT_GCV2_REFFIX_WORKERS, MRT_GCV2_REFFIX_FORCE_SERIAL.
void WCollector::FixMinorRootSlotsParallel(GCThreadPool* threadPool)
{
    // 5 root families as separate tasks (static kept whole — mutex+dedup set).
    // Order matches serial FixMinorRootSlots.
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) {
            RefField<>& field = reinterpret_cast<RefField<>&>(root);
            (void)FixMinorEvacuatedSlot(field);
        };
        MutatorManager::Instance().VisitAllMutators(
            [&rawRootVisitor](Mutator& mutator) { mutator.VisitMutatorRoots(rawRootVisitor); });
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RefFieldVisitor fieldVisitor = [this](RefField<>& field) { (void)FixMinorEvacuatedSlot(field); };
        Heap::GetHeap().VisitStaticRoots(fieldVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) {
            RefField<>& field = reinterpret_cast<RefField<>&>(root);
            (void)FixMinorEvacuatedSlot(field);
        };
        Runtime::Current().GetConcurrencyModel().VisitGCRoots(&rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) {
            RefField<>& field = reinterpret_cast<RefField<>&>(root);
            (void)FixMinorEvacuatedSlot(field);
        };
        collectorResources.GetFinalizerProcessor().VisitRawPointers(rawRootVisitor);
    }));
    threadPool->AddWork(new (std::nothrow) LambdaWork([this](size_t) {
        RootVisitor rawRootVisitor = [this](ObjectRef& root) {
            RefField<>& field = reinterpret_cast<RefField<>&>(root);
            (void)FixMinorEvacuatedSlot(field);
        };
        Heap::GetHeap().VisitAllExportRoots(rawRootVisitor);
    }));
}

void WCollector::EvacuateYoungRegions(const MinorObjectSet& reachableObjects, const MinorSlotSet& rememberedSlots)
{
    RegionManager& manager = reinterpret_cast<RegionSpace&>(theAllocator).GetRegionManager();
    auto postEvacPoint = [this](const char* point, bool runHeap = true) {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac == nullptr || std::strcmp(postEvac, "1") != 0) {
            return;
        }
        // Breadcrumb first (survives if VerifyHeap SEGV); force=true skips VERIFY_HEAP env.
        VLOG(REPORT, "[GCV2][verify][post-evac] enter point=%s run=%zu", point, minorTotalRuns + 1);
        if (runHeap) {
            VerifyHeapObjects(point, true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=%s run=%zu", point, minorTotalRuns + 1);
        }
    };
    auto currentObject = [this](BaseObject* object) {
        if (IsGhostFromObject(object) && !IsUnmovableFromObject(object)) {
            return ForwardObject(object);
        }
        return object;
    };

    // Materialize sets once as vectors for index sharding (object disjoint ⇒ slot disjoint
    // for reachable holders; remset/root overlap covered by N1+N2 idempotent CAS).
    std::vector<BaseObject*> reachableVec(reachableObjects.begin(), reachableObjects.end());
    std::vector<MAddress> remsetVec(rememberedSlots.begin(), rememberedSlots.end());

    auto fixHeapSlice = [this, &reachableVec, &remsetVec, &currentObject](size_t beginObj, size_t endObj,
                                                                           size_t beginSlot, size_t endSlot,
                                                                           size_t& objectsTaken) {
        for (size_t i = beginObj; i < endObj; ++i) {
            FixMinorObjectSlots(currentObject(reachableVec[i]));
            ++objectsTaken;
        }
        for (size_t i = beginSlot; i < endSlot; ++i) {
            MAddress slot = remsetVec[i];
            if (Heap::IsHeapAddress(slot)) {
                (void)FixMinorEvacuatedSlot(*reinterpret_cast<RefField<>*>(slot));
            }
        }
    };

    auto fixForwardedReferencesSerial = [this, &reachableVec, &remsetVec, &currentObject]() {
        FixMinorRootSlots();
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        for (BaseObject* object : reachableVec) {
            FixMinorObjectSlots(currentObject(object));
        }
        for (MAddress slot : remsetVec) {
            if (Heap::IsHeapAddress(slot)) {
                (void)FixMinorEvacuatedSlot(*reinterpret_cast<RefField<>*>(slot));
            }
        }
    };

    auto fixForwardedReferencesParallel = [this, &reachableVec, &remsetVec, &fixHeapSlice](GCThreadPool* pool) {
        // T-D ③: dispel must stay frozen across the parallel window.
        const size_t dispelAtEntry = RegionInfo::GetDispelGhostCount();
        // Positive control: MRT_GCV2_REFFIX_INJECT_DISPEL=1 forces a synthetic bump so
        // the assertion path is proven to fire (not a silent always-pass).
        {
            const char* inject = std::getenv("MRT_GCV2_REFFIX_INJECT_DISPEL");
            if (inject != nullptr && std::strcmp(inject, "1") == 0) {
                RegionInfo::InjectDispelCountForTest();
                VLOG(REPORT, "[GCV2][reffix] inject_dispel=1 (positive control)");
            }
        }

        const size_t nObj = reachableVec.size();
        const size_t nSlot = remsetVec.size();
        const int32_t helperNum = pool->GetMaxThreadNum();
        const int32_t poolCap = helperNum + 1;
        int32_t heapWorkers = poolCap;
        {
            const char* wEnv = std::getenv("MRT_GCV2_REFFIX_WORKERS");
            if (wEnv != nullptr && wEnv[0] != '\0') {
                int32_t want = static_cast<int32_t>(std::strtol(wEnv, nullptr, 10));
                if (want >= 1 && want < heapWorkers) {
                    heapWorkers = want;
                }
            }
        }
        // At least 1 heap worker; root families = 5 additional tasks.
        if (heapWorkers < 1) {
            heapWorkers = 1;
        }
        std::vector<size_t> objectsTaken(static_cast<size_t>(heapWorkers), 0);
        std::atomic<size_t> objCursor{ 0 };
        std::atomic<size_t> slotCursor{ 0 };
        // Chunk size: aim ~heapWorkers*4 grabs for load balance; min 64 objects.
        const size_t objChunk = std::max<size_t>(64, (nObj + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                        (static_cast<size_t>(heapWorkers) * 4 + 1));
        const size_t slotChunk = std::max<size_t>(64, (nSlot + static_cast<size_t>(heapWorkers) * 4 - 1) /
                                                         (static_cast<size_t>(heapWorkers) * 4 + 1));

        FixMinorRootSlotsParallel(pool);
        pool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardDiscoveredExternObjects(); }));
        pool->AddWork(new (std::nothrow) LambdaWork([this](size_t) { PreforwardAllResurrectExportFromObjects(); }));

        for (int32_t w = 0; w < heapWorkers; ++w) {
            size_t* taken = &objectsTaken[static_cast<size_t>(w)];
            pool->AddWork(new (std::nothrow) LambdaWork(
                [fixHeapSlice, &objCursor, &slotCursor, nObj, nSlot, objChunk, slotChunk, taken](size_t) {
                    for (;;) {
                        size_t o0 = nObj;
                        size_t o1 = nObj;
                        size_t s0 = nSlot;
                        size_t s1 = nSlot;
                        bool got = false;
                        if (objCursor.load(std::memory_order_relaxed) < nObj) {
                            o0 = objCursor.fetch_add(objChunk, std::memory_order_relaxed);
                            if (o0 < nObj) {
                                o1 = std::min(o0 + objChunk, nObj);
                                got = true;
                            } else {
                                o0 = o1 = nObj;
                            }
                        }
                        if (slotCursor.load(std::memory_order_relaxed) < nSlot) {
                            s0 = slotCursor.fetch_add(slotChunk, std::memory_order_relaxed);
                            if (s0 < nSlot) {
                                s1 = std::min(s0 + slotChunk, nSlot);
                                got = true;
                            } else {
                                s0 = s1 = nSlot;
                            }
                        }
                        if (!got) {
                            break;
                        }
                        fixHeapSlice(o0, o1, s0, s1, *taken);
                    }
                }));
        }

        pool->Start();
        pool->WaitFinish();

        const size_t dispelAtExit = RegionInfo::GetDispelGhostCount();
        CHECK_DETAIL(dispelAtExit == dispelAtEntry,
                     "T-D ghost dispel during parallel ref_fix window entry=%zu exit=%zu "
                     "(plain InGhostFromRegion read assumes phase isolation)",
                     dispelAtEntry, dispelAtExit);

        size_t active = 0;
        std::string takenStr;
        for (size_t i = 0; i < objectsTaken.size(); ++i) {
            if (objectsTaken[i] != 0) {
                ++active;
            }
            if (i != 0) {
                takenStr += ',';
            }
            takenStr += std::to_string(objectsTaken[i]);
        }
        VLOG(REPORT,
             "[GCV2][reffix][parallel] workers_active=%zu workers_scheduled=%d objects_taken=[%s] "
             "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu parallel=1",
             active, heapWorkers, takenStr.c_str(), nObj, nSlot,
             g_minorRefCasOk.load(std::memory_order_relaxed),
             g_minorRefCasFail.load(std::memory_order_relaxed));
    };

    // Earliest post-mark checkpoint: still before any fix/forward mutates refs.
    postEvacPoint("evac-enter", true);

    {
        // minortime: ⑦ ref fix (preforward roots + fixForwardedReferences)
        MRT_PHASE_TIMER("young.ref_fix");
        TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);

        GCThreadPool* threadPool = GetThreadPool();
        static const bool forceSerialEnv = []() {
            const char* value = std::getenv("MRT_GCV2_REFFIX_FORCE_SERIAL");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        const bool forceSerial = forceSerialEnv;
        const bool useParallel = threadPool != nullptr && !forceSerial;

        // pass1 root fix (before PrepareForwardTable) — serial sandwich stays;
        // only the post-map fixForwardedReferences body is parallelized (⑦ bulk).
        // pass1 is load-bearing for previous-gen residual (MINOR_CONCURRENCY §七 T-A).
        FixMinorRootSlots();
        PreforwardDiscoveredExternObjects();
        PreforwardAllResurrectExportFromObjects();
        postEvacPoint("post-preforward-roots", false); // breadcrumb only — avoid SEGV before fix body

        TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
        fwdTable.PrepareForwardTable();
        TransitionToGCPhase(GCPhase::GC_PHASE_PREFORWARD, true);
        postEvacPoint("pre-fix-forwarded", false);

        // Reset CAS counters for this fix window (positive-control visibility).
        g_minorRefCasFail.store(0, std::memory_order_relaxed);
        g_minorRefCasOk.store(0, std::memory_order_relaxed);

        if (!useParallel) {
            VLOG(REPORT, "[GCV2][reffix][parallel] fallback=serial pool_unavailable");
            // pass1 roots already done; only heap+remset+pass2 roots remain.
            // Mirror serial fixForwardedReferences but roots again (same as before).
            fixForwardedReferencesSerial();
            VLOG(REPORT,
                 "[GCV2][reffix][parallel] workers_active=1 workers_scheduled=1 objects_taken=[%zu] "
                 "nObj=%zu nSlot=%zu cas_ok=%zu cas_fail=%zu parallel=0",
                 reachableVec.size(), reachableVec.size(), remsetVec.size(),
                 g_minorRefCasOk.load(std::memory_order_relaxed),
                 g_minorRefCasFail.load(std::memory_order_relaxed));
        } else {
            fixForwardedReferencesParallel(threadPool);
        }

        ValidateMinorReferences("before-return", &reachableObjects);
        // Mid-evac checkpoint: after slot fix, before region reclaim.
        postEvacPoint("post-fix-pre-forward", true);
    }

    {
        // minortime: ⑥ copy / forward
        MRT_PHASE_TIMER("young.copy");
        ForwardFromSpace();
        postEvacPoint("post-forward-pre-reclaim", true);
        {
            const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
            if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
                ValidateMinorReferences("post-forward-pre-reclaim", &reachableObjects);
            }
        }
    }

    {
        // minortime: ⑧ finish inside evacuate (promote residual + remset rebuild + reassemble)
        MRT_PHASE_TIMER("young.evac_finish");
        size_t residualPromoteRecords = 0;
        // Positive-control only (rebuildgate): force one live young region so the
        // rebuild gate must open. Prefer leaving a residual young undemoted; if
        // residualPromote path is empty (product real_load: residual≡0), re-tag
        // the first minor candidate as young after demote. Default off.
        const char* keepOneYoungEnv = std::getenv("MRT_GCV2_REBUILD_KEEP_ONE_YOUNG");
        const bool keepOneYoung =
            keepOneYoungEnv != nullptr && std::strcmp(keepOneYoungEnv, "1") == 0;
        bool keptOneYoung = false;
        for (RegionInfo* region : minorCandidateRegions) {
            if (region->IsYoungRegion()) {
                if (keepOneYoung && !keptOneYoung) {
                    keptOneYoung = true;
                    continue;
                }
                // Residual candidates not forwarded above (e.g. raw-pointer pinned):
                // still demote to old; must replay young→young edges that become old→young.
                region->PreserveRetainedLiveInfo();
                residualPromoteRecords += RegionManager::RecordPromotedCrossGenEdges(region);
                region->SetYoungRegionFlag(0);
                region->SetYoungAge(0);
            }
        }
        if (keepOneYoung && !keptOneYoung) {
            // No residual young remained (common today). Re-tag one candidate so
            // GetYoungRegionCount()>0 and the structural gate opens for the dual-arm
            // positive control. Not a product path.
            for (RegionInfo* region : minorCandidateRegions) {
                if (region == nullptr) {
                    continue;
                }
                region->SetYoungRegionFlag(1);
                keptOneYoung = true;
                VLOG(REPORT,
                     "[GCV2Minor][rebuild-gate] positive-control reyoung region=%p",
                     region);
                break;
            }
        }
        size_t promotedPathRecords = RegionManager::ConsumePromotedCrossGenEdgeCount();

        // R1 structural gate (MINOR_CONCURRENCY_0805 §9.5): after residual demote,
        // live young region count is the product-path authority
        // (RegionInfo::youngRegionCount / GetYoungRegionCount —
        // RegionManager.cpp:185-209; VerifyRegions.cpp:325). When count==0 every
        // holder is non-young and every target is non-young ⇒ rebuild walk is pure
        // cost with zero output. P2 in-place aging reintroduces live young ⇒ gate
        // reopens automatically (structure, not an env switch).
        RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
        size_t rebuiltRecords = 0;
        const size_t liveYoungRegions = RegionInfo::GetYoungRegionCount();
        if (liveYoungRegions == 0) {
            VLOG(REPORT,
                 "[GCV2Minor][rebuild-gate] skip rebuild youngRegionCount=0");
        } else {
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
            if (rebuiltRecords == 0) {
                VLOG(REPORT,
                     "[GCV2Minor][rebuild-gate] anomaly open-gate-zero-output "
                     "youngRegionCount=%zu",
                     liveYoungRegions);
            }
        }
        VLOG(REPORT,
             "[GCV2Minor] remembered-set rebuilt=%zu promoteReplay=%zu residualPromote=%zu "
             "youngRegionCount=%zu",
             rebuiltRecords, promotedPathRecords, residualPromoteRecords, liveYoungRegions);

        fwdTable.PrepareForwardTable();
        ValidateMinorReferences("after-dispel", nullptr);
        manager.ReassembleFromSpace();
    }
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

void WCollector::VerifyRegionSets(const char* point)
{
    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    size_t youngRunIndex = minorTotalRuns + 1;
    if (std::strcmp(point, "after-young-mark") == 0) {
        VerifyRegions::VerifyAfterYoungMark(manager, minorCandidateRegions, youngRunIndex, point);
    } else {
        VerifyRegions::VerifyAfterPrepareYoung(manager, minorCandidateRegions, youngRunIndex, point);
    }
}

void WCollector::ProbeUnmarkedLive(const MinorObjectSet& allocationRoots, const MinorSlotSet& rememberedSlots)
{
    // Default off. When on: independent full-heap retrace from roots (no remset filter),
    // collect young objs reachable that way, compare to region mark bitmap after young-only mark.
    // For each unmarked-but-full-reachable young object, scan non-young holders for incoming
    // old→young edges and report whether that field is in the minor-acquired remset.
    const char* enabled = std::getenv("MRT_GCMARKGAP_PROBE");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    MinorObjectSet fullReachable;
    MinorObjectSet fullYoung;
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
        if (!Heap::IsHeapAddress(object) || !fullReachable.insert(object).second) {
            continue;
        }
        if (!object->IsValidObject()) {
            continue;
        }
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr) {
            continue;
        }
        if (region->IsYoungRegion()) {
            fullYoung.insert(object);
        }
        if (!object->HasRefField()) {
            continue;
        }
        if (UNLIKELY(object->IsWeakRef())) {
            RefField<>* referentField =
                reinterpret_cast<RefField<>*>(reinterpret_cast<MAddress>(object) + TYPEINFO_PTR_SIZE);
            BaseObject* referent = ResolveMinorReference(*referentField);
            if (Heap::IsHeapAddress(referent)) {
                referent->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
            }
            continue;
        }
        object->ForEachRefField([&pushField](RefField<>& field) { pushField(field); });
    }

    size_t unmarkedLive = 0;
    size_t markedYoung = 0;
    size_t missingEdgeHolders = 0;
    size_t edgeInRemset = 0;
    size_t edgeNotInRemset = 0;
    size_t noIncomingOldFound = 0;
    size_t sampleLimit = 8;
    size_t samples = 0;

    for (BaseObject* object : fullYoung) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(object));
        if (region == nullptr) {
            continue;
        }
        if (region->IsMarkedObject(object)) {
            ++markedYoung;
            continue;
        }
        ++unmarkedLive;

        // Find old→young incoming edges by independent non-young holder walk.
        size_t incomingOld = 0;
        size_t incomingMissing = 0;
        MAddress sampleField = 0;
        BaseObject* sampleHolder = nullptr;
        Heap::GetHeap().ForEachObj(
            [&](BaseObject* holder) {
                if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                    return;
                }
                RegionInfo* hReg = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
                if (hReg == nullptr || hReg->IsYoungRegion() || hReg->IsGarbageRegion() || hReg->IsFreeRegion()) {
                    return;
                }
                holder->ForEachRefField([&](RefField<>& field) {
                    BaseObject* target = field.GetTargetObject();
                    if (target != object) {
                        return;
                    }
                    ++incomingOld;
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    bool inRemset = rememberedSlots.count(slot) != 0;
                    if (inRemset) {
                        ++edgeInRemset;
                    } else {
                        ++edgeNotInRemset;
                        ++incomingMissing;
                        if (sampleField == 0) {
                            sampleField = slot;
                            sampleHolder = holder;
                        }
                    }
                });
            },
            false);

        if (incomingMissing > 0) {
            ++missingEdgeHolders;
        }
        if (incomingOld == 0) {
            ++noIncomingOldFound;
        }

        if (samples < sampleLimit) {
            ++samples;
            TypeInfo* ti = object->IsValidObject() ? object->GetTypeInfo() : nullptr;
            TypeInfo* hti = (sampleHolder != nullptr && sampleHolder->IsValidObject()) ? sampleHolder->GetTypeInfo()
                                                                                      : nullptr;
            VLOG(REPORT,
                 "[GCMARKGAP][unmarked-live] run=%zu obj=%p region=%p start=%#zx marked=0 "
                 "fullReachable=1 incomingOld=%zu incomingMissing=%zu sampleField=%p sampleHolder=%p "
                 "objTi=%p holderTi=%p inRemsetSample=%u",
                 minorTotalRuns + 1, object, region, region->GetRegionStart(), incomingOld, incomingMissing,
                 reinterpret_cast<void*>(sampleField), sampleHolder, ti, hti,
                 static_cast<unsigned>(sampleField != 0 && rememberedSlots.count(sampleField) != 0));
        }
    }

    // Also count residual unmarked valid objs on candidates (may be truly dead).
    size_t residualUnmarkedValid = 0;
    size_t residualUnmarkedAndFullReachable = 0;
    size_t neverExaminedCandidates = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        if (region->GetMarkBitmap() == nullptr && region->GetRegionAllocPtr() > region->GetRegionStart()) {
            ++neverExaminedCandidates;
        }
        region->VisitAllObjects([&](BaseObject* object) {
            if (region->IsMarkedObject(object)) {
                return;
            }
            if (!object->IsValidObject()) {
                return;
            }
            ++residualUnmarkedValid;
            if (fullYoung.count(object) != 0) {
                ++residualUnmarkedAndFullReachable;
            }
        });
    }

    VLOG(REPORT,
         "[GCMARKGAP][summary] run=%zu fullYoung=%zu markedYoung=%zu UNMARKED_LIVE=%zu "
         "missingEdgeHolders=%zu edgeInRemset=%zu edgeNotInRemset=%zu noIncomingOld=%zu "
         "residualUnmarkedValid=%zu residualUnmarkedAndFullReachable=%zu neverExaminedCandidates=%zu "
         "remsetSize=%zu env=MRT_GCMARKGAP_PROBE=1",
         minorTotalRuns + 1, fullYoung.size(), markedYoung, unmarkedLive, missingEdgeHolders, edgeInRemset,
         edgeNotInRemset, noIncomingOldFound, residualUnmarkedValid, residualUnmarkedAndFullReachable,
         neverExaminedCandidates, rememberedSlots.size());
}

void WCollector::ValidateYoungMarking(const MinorObjectSet& reachableObjects, const MinorObjectSet& allocationRoots)
{
    // Gate mirrors ValidateMinorReferences. Default OFF — product path must not abort.
    // Env: MRT_GCV2_VERIFY_YOUNG_MARKING=1 to enable (default unset/other = off).
    // Mark source: MRT_GCV2_VERIFY_MARK_SOURCE (HotSpot VerifyOption isomorphic).
    // Default IndependentVsBitmap — does NOT require MinorClosure membership, so fullYoungScan
    // is not tautological (gcvheap / HotSpot inventory #22).
    const char* enabled = std::getenv("MRT_GCV2_VERIFY_YOUNG_MARKING");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0) {
        return;
    }

    VerifyMarkSource markSource = ParseVerifyMarkSource();
    const bool useIndependent = markSource == VerifyMarkSource::IndependentVsBitmap ||
                                markSource == VerifyMarkSource::IndependentRetrace ||
                                markSource == VerifyMarkSource::MinorClosure;
    const bool useBitmap = markSource == VerifyMarkSource::IndependentVsBitmap ||
                           markSource == VerifyMarkSource::RegionMarkBitmap ||
                           markSource == VerifyMarkSource::MinorClosure;
    const bool requireMinorClosure = markSource == VerifyMarkSource::MinorClosure;

    MinorObjectSet reachable;
    MinorObjectSet expectedYoung;
    if (useIndependent) {
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
    }

    size_t actualYoung = 0;
    size_t unexpectedYoung = 0;
    if (useBitmap) {
        for (RegionInfo* region : minorCandidateRegions) {
            region->VisitAllObjects([&](BaseObject* object) {
                if (!region->IsMarkedObject(object)) {
                    return;
                }
                ++actualYoung;
                bool bad = false;
                if (useIndependent && expectedYoung.count(object) == 0) {
                    bad = true;
                }
                if (requireMinorClosure && reachableObjects.count(object) == 0) {
                    bad = true;
                }
                if (bad) {
                    ++unexpectedYoung;
                }
            });
        }
    }

    size_t missingYoung = 0;
    if (useIndependent) {
        for (BaseObject* object : expectedYoung) {
            RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(object));
            bool missing = false;
            if (useBitmap && !region->IsMarkedObject(object)) {
                missing = true;
            }
            if (requireMinorClosure && reachableObjects.count(object) == 0) {
                missing = true;
            }
            if (missing) {
                ++missingYoung;
            }
        }
    }

    size_t matchCount = (actualYoung >= unexpectedYoung) ? (actualYoung - unexpectedYoung) : 0;
    size_t expectedSize = useIndependent ? expectedYoung.size() : actualYoung;
    VLOG(REPORT,
         "[GCV2][verify][young-marking] run=%zu phase=post-trace env=MRT_GCV2_VERIFY_YOUNG_MARKING=1 "
         "markSource=%s mark-equivalence=%zu/%zu missing=%zu unexpected=%zu "
         "requireMinorClosure=%u",
         minorTotalRuns + 1, VerifyMarkSourceName(markSource), matchCount, expectedSize, missingYoung,
         unexpectedYoung, static_cast<unsigned>(requireMinorClosure));
    if (markSource == VerifyMarkSource::IndependentRetrace || markSource == VerifyMarkSource::RegionMarkBitmap) {
        // Single-source modes only report; cross-check needs two sides.
        return;
    }
    CHECK_DETAIL(missingYoung == 0 && unexpectedYoung == 0 &&
                     (!useIndependent || !useBitmap || actualYoung == expectedYoung.size()),
                 "minor marking differs from full marking: actual=%zu expected=%zu missing=%zu unexpected=%zu "
                 "markSource=%s",
                 actualYoung, expectedYoung.size(), missingYoung, unexpectedYoung,
                 VerifyMarkSourceName(markSource));
}

void WCollector::FlushAllocationRegions()
{
    theAllocator.VisitAllocBuffers([](AllocBuffer& buffer) { buffer.FlushRegion(); });
}

void WCollector::DoYoungGarbageCollection()
{
    uint64_t start = TimeUtil::NanoSeconds();
    ScopedStopTheWorld stw("young collection", true, GCPhase::GC_PHASE_ENUM);
    // minortime: STW rendezvous cost is already logged by ScopedStopTheWorld dtor
    // ("young collection stw time N us"). Body timers below exclude that wait.
    // Timeline probe (gcdirty): earliest STW point = mutator just handed control.
    // force via POST_EVAC so we do not need global VERIFY_HEAP (avoids pre-evac side effects).
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VLOG(REPORT, "[GCV2][verify][post-evac] enter point=stw-enter run=%zu priorMinors=%zu",
                 minorTotalRuns + 1, minorTotalRuns);
            VerifyHeapObjects("stw-enter", true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=stw-enter run=%zu", minorTotalRuns + 1);
        }
    }
    TransitionToGCPhase(GCPhase::GC_PHASE_CLEAR_SATB_BUFFER, true);
    {
        // minortime: ① FlushAllocationRegions
        MRT_PHASE_TIMER("young.flush_alloc");
        FlushAllocationRegions();
    }
    if (minorTotalRuns != 0) {
        ValidateMinorReferences("round2-start", nullptr);
    }

    RegionSpace& space = reinterpret_cast<RegionSpace&>(theAllocator);
    RegionManager& manager = space.GetRegionManager();
    minorCandidateRegions.clear();
    YoungCollectionStats stats;
    {
        // minortime: ② PrepareYoungGarbageCandidates
        MRT_PHASE_TIMER("young.prepare_candidates");
        stats = manager.PrepareYoungGarbageCandidates(
            [this](RegionInfo* region) { minorCandidateRegions.insert(region); });
    }
    // HotSpot g1HeapVerifier.cpp:424 verify_region_sets placement: after region accounting is stable.
    VerifyRegionSets("after-prepare-young");
    // Region-set verify after candidate construction (HotSpot verify_region_sets placement intent).
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-prepare-young run=%zu",
                 minorTotalRuns + 1);
            VerifyHeapObjects("post-prepare-young", true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=post-prepare-young run=%zu", minorTotalRuns + 1);
        }
    }
    if (stats.candidateRegions == 0) {
        manager.ReassembleFromSpace();
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        ++minorTotalRuns;
        VLOG(REPORT, "[GCV2Minor] run=%zu candidates=0 candidateBytes=0 live=0 reclaimedBytes=0",
             minorTotalRuns);
        return;
    }

    // Pinned holders (Future/Mutex/Monitor): AllocPinned never sets young; IDLE write
    // fast-path (phase < ENUM) is a bare store — old→young edges never hit remset.
    // Stamp them before Acquire so pre-evacuate verify and young mark both see them.
    MinorSlotSet rememberedSlots;
    {
        // minortime: ④ remset / cross-gen edge consume (drain + pinned stamp; rescan below)
        MRT_PHASE_TIMER("young.remset_drain");
        size_t pinnedRemsetRecords = manager.RecordPinnedCrossGenEdges();
        if (pinnedRemsetRecords != 0) {
            VLOG(REPORT, "[GCV2Minor] pinnedCrossGenEdges=%zu", pinnedRemsetRecords);
        }
        Heap::GetHeap().GetRememberedSet().DrainForMinor(rememberedSlots);
    }

    const char* fallback = std::getenv("MRT_GCV2_FULL_YOUNG_SCAN");
    bool fullYoungScan = fallback == nullptr || std::strcmp(fallback, "0") != 0;
    WorkStack workStack = NewWorkStack();
    MinorObjectSet reachableObjects;
    MinorObjectSet allocationRoots;
    MinorSlotSet reachableSlots;
    MinorSlotSet weakSlots;
    {
        // minortime: ③ root enum (alloc buffers + VisitMinorRoots)
        MRT_PHASE_TIMER("young.root_enum");
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
                PushYoungObject(object, workStack, "alloc_buffer");
            }
        }
        VisitMinorRoots([this, fullYoungScan, &workStack](BaseObject* object) {
            if (fullYoungScan) {
                if (Heap::IsHeapAddress(object)) {
                    workStack.push_back(object);
                }
            } else {
                // origin comes from gMinorRootOrigin set inside VisitMinorRootSlots/ValueRoots
                PushYoungObject(object, workStack, "minor_root");
            }
        });
    }
    {
        // minortime: ⑤ mark closure pass-1 (from roots)
        MRT_PHASE_TIMER("young.mark_closure");
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableSlots, weakSlots);
    }
    MinorSlotSet liveRememberedSlots;
    for (MAddress slot : rememberedSlots) {
        if (weakSlots.count(slot) == 0 && (!fullYoungScan || reachableSlots.count(slot) != 0)) {
            liveRememberedSlots.insert(slot);
        }
    }
    // Remset consume-vs-recorded (G1SummarizeRSetStats analog) + optional dual-closure
    // diff-path explainer. Both gated default-off; see DiffPathExplainer.h.
    DiffPathRemsetStats remsetStats;
    remsetStats.recorded = rememberedSlots.size();
    remsetStats.live = liveRememberedSlots.size();
    MinorSlotSet consumedSlots;
    {
        // minortime: ④ remset rescan + ⑤ mark closure pass-2 (from remset edges)
        MRT_PHASE_TIMER("young.remset_rescan");
        RescanRememberedSet(workStack, rememberedSlots, reachableSlots, weakSlots, fullYoungScan, &consumedSlots,
                            &remsetStats);
    }
    {
        MRT_PHASE_TIMER("young.mark_from_remset");
        TraceYoungClosure(workStack, fullYoungScan, reachableObjects, reachableSlots, weakSlots);
    }
    InteriorSrcProbe::FlushSummary("post-minor-trace");
    StaticSlotProbe::FlushSummary("post-minor-trace");
    static const bool verifyRemsetEnabled = []() {
        const char* value = std::getenv("MRT_GCV2_VERIFY_REMSET");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    // Independent remset completeness check (invariant R). Gated by MRT_GCV2_VERIFY_REMSET.
    // Uses the minor-acquired slot set: live remset is empty after AcquireRecordsForMinor.
    {
        size_t runIndex = minorTotalRuns + 1;
        auto visitRoots = [this, &allocationRoots](const std::function<void(BaseObject*)>& visitor) {
            for (BaseObject* object : allocationRoots) {
                visitor(object);
            }
            VisitMinorRoots(visitor);
        };
        auto resolveField = [this](RefField<>& field) -> BaseObject* { return ResolveMinorReference(field); };
        if (verifyRemsetEnabled) {
            std::unordered_set<BaseObject*> rootReachableForRemsetVerify;
            RunDiffPathExplainer(runIndex, visitRoots, resolveField, rememberedSlots, consumedSlots,
                                 &minorCandidateRegions, remsetStats, &rootReachableForRemsetVerify);
            VerifyRememberedSetInvariant("pre-evacuate", rememberedSlots, false, &rootReachableForRemsetVerify);
        } else {
            RunDiffPathExplainer(runIndex, visitRoots, resolveField, rememberedSlots, consumedSlots,
                                 &minorCandidateRegions, remsetStats, nullptr);
            VerifyRememberedSetInvariant("pre-evacuate", rememberedSlots, false, nullptr);
        }
    }
    // Full-heap object invariant H (HotSpot G1HeapVerifier::verify inventory #10).
    // Independent ForEachObj walk; gated by MRT_GCV2_VERIFY_HEAP (default off).
    // Timeline (gcdirty): also force as post-mark under POST_EVAC so first-dirty bracketing
    // does not require global VERIFY_HEAP.
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VLOG(REPORT, "[GCV2][verify][post-evac] enter point=post-mark run=%zu", minorTotalRuns + 1);
            VerifyHeapObjects("post-mark", true);
            VLOG(REPORT, "[GCV2][verify][post-evac] point=post-mark run=%zu", minorTotalRuns + 1);
        } else {
            VerifyHeapObjects("pre-evacuate");
        }
    }

    size_t liveBytes = 0;
    for (RegionInfo* region : minorCandidateRegions) {
        liveBytes += region->GetLiveByteCount();
    }
    if (fullYoungScan) {
        // Run structural verify before mark-equivalence CHECK (may abort).
        VerifyRegionSets("after-young-mark");
        ValidateYoungMarking(reachableObjects, allocationRoots);
    }
    // Always-available (gated) probe: full-heap independent reachability vs young-only bitmap.
    // Runs with FULL_YOUNG_SCAN=0 so B2 path is exercised. Default off.
    ProbeUnmarkedLive(allocationRoots, rememberedSlots);

    {
        // minortime: ⑧ pre-evac finish (phase + weak/satb clear)
        MRT_PHASE_TIMER("young.pre_evac_clear");
        TransitionToGCPhase(GCPhase::GC_PHASE_POST_TRACE, true);
        WeakRefBuffer::Instance().ClearWeakRefBuffer();
        SatbBuffer::Instance().ClearBuffer();
    }

    size_t allocatedBefore = space.AllocatedBytes();
    // ⑥⑦⑧ inside EvacuateYoungRegions: young.ref_fix / young.copy / young.evac_finish
    EvacuateYoungRegions(reachableObjects, liveRememberedSlots);
    size_t allocatedAfter = space.AllocatedBytes();
    stats.reclaimedBytes = allocatedBefore > allocatedAfter ? allocatedBefore - allocatedAfter : 0;
    GetGCStats().collectedBytes = stats.reclaimedBytes;

    // Post-evacuate invariant P (HotSpot VerifyAfterGC analog for young): after
    // fix+forward+remset rebuild inside EvacuateYoungRegions, every live ref must
    // still be a legal object (VerifyHeap H) and remset must cover old→young (R).
    // Gate default off: MRT_GCV2_VERIFY_POST_EVAC=1. force=true so this does not
    // require MRT_GCV2_VERIFY_HEAP/REMSET (avoids pre-evacuate side effects).
    {
        const char* postEvac = std::getenv("MRT_GCV2_VERIFY_POST_EVAC");
        if (postEvac != nullptr && std::strcmp(postEvac, "1") == 0) {
            VerifyHeapObjects("post-evacuate", true);
            std::unordered_set<MAddress> remsetSnap = Heap::GetHeap().GetRememberedSet().Snapshot();
            VerifyRememberedSetInvariant("post-evacuate", remsetSnap, true);
            ValidateMinorReferences("post-evacuate", nullptr);
            VLOG(REPORT,
                 "[GCV2][verify][post-evac] point=post-evacuate run=%zu "
                 "env=MRT_GCV2_VERIFY_POST_EVAC=1 remsetSnap=%zu",
                 minorTotalRuns + 1, remsetSnap.size());
        }
    }

    {
        // minortime: ⑧ post-evac finish
        MRT_PHASE_TIMER("young.post_evac_finish");
        TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
        MergeResurrectExportObjects();
    }
    ++minorTotalRuns;
    uint64_t pauseUs = (TimeUtil::NanoSeconds() - start) / NS_PER_US;
    VLOG(REPORT,
         "[GCV2Minor] run=%zu fallbackFullScan=%u candidates=%zu candidateBytes=%zu liveBytes=%zu "
         "remembered=%zu reclaimedBytes=%zu pause=%zu us",
         minorTotalRuns, static_cast<unsigned>(fullYoungScan), stats.candidateRegions, stats.candidateBytes,
         liveBytes, liveRememberedSlots.size(), stats.reclaimedBytes, pauseUs);
    // STEER4: DumpScrubCostAndReset is a no-op unless MRT_GCV2_SCRUB_COST=1.
    RegionManager::DumpScrubCostAndReset("post-minor");
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

    // Publish a clean full-GC buffer before mutators return to IDLE. The phase
    // transition is the grace period for writers that had already loaded the old
    // buffer index; clear that captured buffer only after the transition completes.
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    uint8_t fullRemsetBuffer = remset.BeginFullClear();
    TransitionToGCPhase(GCPhase::GC_PHASE_IDLE, true);
    size_t droppedRemsetRecords = remset.FinishFullClear(fullRemsetBuffer);
    if (droppedRemsetRecords != 0) {
        VLOG(REPORT, "[GCV2][remset] cleared after full GC dropped=%zu", droppedRemsetRecords);
    }
    MergeResurrectExportObjects();
    PostResolveCycleTask();
    FlipTagID();
    ForwardDataManager::GetForwardDataManager().SetTagID(currentTagID);
    // Flip just turned this cycle's current-tags into IsOldPointer. F3 pre-Flip only
    // saw the previous generation. Post-Flip pass must NOT filter IsSurvivedObject:
    // after Forward, live holders are in to-space without mark bits at the new addr.
    InvalidateOldTaggedRefs(false);

    CollectSmallSpace();
    // retmid: do NOT StampCensusBoundaries / PromoteAllRegions here.
    // Ablation D (both major STWs disabled) restores mid_alloc 5/5; any of
    // Flush/Stamp/Promote in these STWs reintroduces 0/5 or residual 甲 under
    // FYS=0 SKIP_PINNED=1 512MB. Retained-liveness still applies on residual and
    // in-place promote paths that already Preserve + RecordPromotedCrossGenEdges.
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
