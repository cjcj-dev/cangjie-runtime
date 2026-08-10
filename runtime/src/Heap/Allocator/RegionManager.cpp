// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator/RegionManager.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>

#include "Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/TimeUtils.h"
#include "Collector/Collector.h"
#include "Collector/CopyCollector.h"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/IdleEdgeDiag.h"
#include "Heap/Verify/TraceClear.h"
#include "Heap/Verify/Zap.h"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
uintptr_t RegionInfo::UnitInfo::totalUnitCount = 0;
uintptr_t RegionInfo::UnitInfo::heapStartAddress = 0;
std::atomic<size_t> RegionInfo::youngRegionCount { 0 };
std::atomic<size_t> RegionInfo::dispelGhostCount { 0 };
std::atomic<size_t> RegionInfo::tipInHeapHits { 0 };
std::mutex RegionInfo::youngRegionFlagMutex;
std::atomic<size_t> g_promotedCrossGenEdgeCount { 0 };

// promotegap: offset histogram for promote re-registration (MRT_GCV2_PROMOTEGAP_PROBE=1).
namespace {
constexpr size_t kPromoteGapOffBuckets = 64;
std::atomic<uint64_t> g_pgInplaceSeen { 0 };
std::atomic<uint64_t> g_pgInplaceRec { 0 };
std::atomic<uint64_t> g_pgInplaceNode { 0 };
std::atomic<uint64_t> g_pgInplaceNode10Seen { 0 };
std::atomic<uint64_t> g_pgInplaceNode10Rec { 0 };
std::atomic<uint64_t> g_pgInplaceNode10SkipOldT { 0 };
std::atomic<uint64_t> g_pgInplaceNode10SkipNull { 0 };
std::atomic<uint64_t> g_pgFwdSeen { 0 };
std::atomic<uint64_t> g_pgFwdRec { 0 };
std::atomic<uint64_t> g_pgFwdNode { 0 };
std::atomic<uint64_t> g_pgFwdNode10Seen { 0 };
std::atomic<uint64_t> g_pgFwdNode10Rec { 0 };
std::atomic<uint64_t> g_pgFwdNode10SkipOldT { 0 };
std::atomic<uint64_t> g_pgFwdNode10SkipNull { 0 };
std::atomic<uint64_t> g_pgOffInplace[kPromoteGapOffBuckets] {};
std::atomic<uint64_t> g_pgOffFwd[kPromoteGapOffBuckets] {};
std::atomic<uint64_t> g_pgDumpSeq { 0 };

bool PromoteGapProbeOn()
{
    static const bool on = []() {
        return DiagGate::LegacyOrToken("MRT_GCV2_PROMOTEGAP_PROBE", "promote") ||
            DiagGate::LegacyOrToken("MRT_GCV2_PROMOTEGAP_PROBE", "promotegap");
    }();
    return on;
}

bool IsDefaultNode(BaseObject* object)
{
    if (object == nullptr) {
        return false;
    }
    TypeInfo* ti = object->GetTypeInfo();
    if (ti == nullptr) {
        return false;
    }
    const char* name = ti->GetName();
    return name != nullptr && std::strcmp(name, "default:Node") == 0;
}

void NotePromoteGapField(BaseObject* object, RefField<>& field, bool recorded, bool fwdPath)
{
    if (!PromoteGapProbeOn() || object == nullptr) {
        return;
    }
    MAddress base = reinterpret_cast<MAddress>(object);
    MAddress slot = reinterpret_cast<MAddress>(&field);
    if (slot < base) {
        return;
    }
    size_t off = static_cast<size_t>(slot - base);
    if (fwdPath) {
        g_pgFwdSeen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgFwdRec.fetch_add(1, std::memory_order_relaxed);
        }
        if (off < kPromoteGapOffBuckets) {
            g_pgOffFwd[off].fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_pgInplaceSeen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgInplaceRec.fetch_add(1, std::memory_order_relaxed);
        }
        if (off < kPromoteGapOffBuckets) {
            g_pgOffInplace[off].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!IsDefaultNode(object)) {
        return;
    }
    if (fwdPath) {
        g_pgFwdNode.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_pgInplaceNode.fetch_add(1, std::memory_order_relaxed);
    }
    if (off != 0x10) {
        return;
    }
    BaseObject* target = to_object(field.GetTargetObject());
    if (fwdPath) {
        g_pgFwdNode10Seen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgFwdNode10Rec.fetch_add(1, std::memory_order_relaxed);
        } else if (target == nullptr || !Heap::IsHeapAddress(target)) {
            g_pgFwdNode10SkipNull.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_pgFwdNode10SkipOldT.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_pgInplaceNode10Seen.fetch_add(1, std::memory_order_relaxed);
        if (recorded) {
            g_pgInplaceNode10Rec.fetch_add(1, std::memory_order_relaxed);
        } else if (target == nullptr || !Heap::IsHeapAddress(target)) {
            g_pgInplaceNode10SkipNull.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_pgInplaceNode10SkipOldT.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void DumpPromoteGapProbe(const char* tag)
{
    if (!PromoteGapProbeOn()) {
        return;
    }
    uint64_t seq = g_pgDumpSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    VLOG(REPORT,
         "[PROMOTEGAP][%s] seq=%llu inplace seen=%llu rec=%llu node=%llu "
         "node10seen=%llu node10rec=%llu node10skipOldT=%llu node10skipNull=%llu | "
         "fwd seen=%llu rec=%llu node=%llu node10seen=%llu node10rec=%llu "
         "node10skipOldT=%llu node10skipNull=%llu",
         tag, static_cast<unsigned long long>(seq),
         static_cast<unsigned long long>(g_pgInplaceSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceRec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10Seen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10Rec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10SkipOldT.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgInplaceNode10SkipNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdRec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10Seen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10Rec.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10SkipOldT.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_pgFwdNode10SkipNull.load(std::memory_order_relaxed)));
    for (size_t off = 0; off < kPromoteGapOffBuckets; ++off) {
        uint64_t a = g_pgOffInplace[off].load(std::memory_order_relaxed);
        uint64_t b = g_pgOffFwd[off].load(std::memory_order_relaxed);
        if (a == 0 && b == 0) {
            continue;
        }
        VLOG(REPORT,
             "[PROMOTEGAP][OFF] seq=%llu offset=0x%zx inplace=%llu fwd=%llu",
             static_cast<unsigned long long>(seq), off,
             static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    }
}
} // namespace

size_t RegionManager::RecordPromotedCrossGenEdges(RegionInfo* region)
{
    if (region == nullptr || !region->IsYoungRegion()) {
        return 0;
    }
    static const bool fysGapProbe = []() {
        const char* value = std::getenv("MRT_GCV2_FYSGAP_PROBE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (region->IsSafeKnownEmpty()) {
        if (fysGapProbe) {
            VLOG(REPORT,
                 "[FYSGAP][promotion-summary] region=%p recorded=0 live=0 dead=0 unknown=0 "
                 "knownEmpty=1 hasBitmap=%u mode=safe-empty",
                 region,
                 static_cast<unsigned>(region->GetMarkBitmap() != nullptr ||
                                       region->GetResurrectBitmap() != nullptr));
        }
        return 0;
    }
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t recorded = 0;
    size_t liveEdges = 0;
    size_t deadEdges = 0;
    size_t unknownEdges = 0;
    bool hasObjectLiveness = region->IsLargeRegion() || region->GetMarkBitmap() != nullptr ||
        region->GetResurrectBitmap() != nullptr;
    bool useLiveOnly = hasObjectLiveness && region->IsLiveCountAuthoritative();
    auto recordFromObject = [region, &rememberedSet, &recorded, &liveEdges, &deadEdges,
                             &unknownEdges, hasObjectLiveness, useLiveOnly](BaseObject* object) {
        if (object == nullptr || !object->HasRefField()) {
            return;
        }
        bool survived = hasObjectLiveness &&
            region->IsSurvivedObject(region->GetAddressOffset(reinterpret_cast<MAddress>(object)));
        if (useLiveOnly && !survived) {
            if (fysGapProbe) {
                object->ForEachRefField([&deadEdges](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                        ++deadEdges;
                    }
                });
            }
            return;
        }
        object->ForEachRefField([&rememberedSet, &recorded, &liveEdges, &deadEdges, &unknownEdges,
                                hasObjectLiveness, survived, object](RefField<>& field) {
            BaseObject* target = to_object(field.GetTargetObject());
            MAddress slot = reinterpret_cast<MAddress>(&field);
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                NotePromoteGapField(object, field, false, false);
                IdleEdgeDiag::NotePromoteTimeTarget(slot, /*null/nonheap*/ 3, false);
                return;
            }
            RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                rememberedSet.Record(slot);
                ++recorded;
                NotePromoteGapField(object, field, true, false);
                IdleEdgeDiag::NotePromoteTimeTarget(slot, /*young*/ 1, true);
                if (fysGapProbe) {
                    if (!hasObjectLiveness) {
                        ++unknownEdges;
                    } else if (survived) {
                        ++liveEdges;
                    } else {
                        ++deadEdges;
                    }
                }
            } else {
                NotePromoteGapField(object, field, false, false);
                IdleEdgeDiag::NotePromoteTimeTarget(slot, /*old*/ 2, false);
            }
        });
    };
    region->VisitAllObjects([&recordFromObject](BaseObject* object) { recordFromObject(object); });
    if (recorded != 0) {
        g_promotedCrossGenEdgeCount.fetch_add(recorded, std::memory_order_relaxed);
    }
    if (fysGapProbe) {
        VLOG(REPORT,
             "[FYSGAP][promotion-summary] region=%p recorded=%zu live=%zu dead=%zu unknown=%zu "
             "knownEmpty=%u hasBitmap=%u mode=%s",
             region, recorded, liveEdges, deadEdges, unknownEdges, static_cast<unsigned>(region->IsKnownEmpty()),
             static_cast<unsigned>(hasObjectLiveness), useLiveOnly ? "live-only" : "scan-all");
    }
    return recorded;
}

size_t RegionManager::ConsumePromotedCrossGenEdgeCount()
{
    size_t n = g_promotedCrossGenEdgeCount.exchange(0, std::memory_order_relaxed);
    DumpPromoteGapProbe("consume");
    return n;
}

size_t RegionManager::RecordPinnedCrossGenEdges()
{
    // gcscanoff blocking test: skip whole conservative pinned/old scan (default off).
    {
        static const bool skip = []() {
            const char* v = std::getenv("MRT_GCV2_SKIP_PINNED_SCAN");
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        if (skip) {
            VLOG(REPORT, "[GCV2][block] skip RecordPinnedCrossGenEdges env=MRT_GCV2_SKIP_PINNED_SCAN=1");
            return 0;
        }
    }
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t recorded = 0;
    auto scanRegion = [&rememberedSet, &recorded](RegionInfo* region) {
        if (region == nullptr || region->IsYoungRegion() || region->IsGarbageRegion()) {
            return;
        }
        region->VisitAllObjects([&rememberedSet, &recorded](BaseObject* object) {
            if (object == nullptr || !object->HasRefField()) {
                return;
            }
            object->ForEachRefField([&rememberedSet, &recorded, object](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                    rememberedSet.Record(reinterpret_cast<MAddress>(&field));
                    ++recorded;
                }
            });
        });
    };
    // All never-young alloc paths + post-promote old holders (IDLE bare-store gap).
    // scanRegion already skips IsYoungRegion, so candidate young lists are free.
    recentPinnedRegionList.VisitAllRegions(scanRegion);
    oldPinnedRegionList.VisitAllRegions(scanRegion);
    rawPointerPinnedRegionList.VisitAllRegions(scanRegion);
    recentLargeRegionList.VisitAllRegions(scanRegion);
    oldLargeRegionList.VisitAllRegions(scanRegion);
    largeTraceRegions.VisitAllRegions(scanRegion);
    recentFullRegionList.VisitAllRegions(scanRegion);
    fullTraceRegions.VisitAllRegions(scanRegion);
    unmovableFromRegionList.VisitAllRegions(scanRegion);
    fromRegionList.VisitAllRegions(scanRegion);
    tlRegionList.VisitAllRegions(scanRegion);
    return recorded;
}

void RegionInfo::SetYoungRegionFlag(uint8_t flag)
{
    std::lock_guard<std::mutex> lock(youngRegionFlagMutex);
    bool wasYoung = IsYoungRegion();
    bool makeYoung = flag != 0;
    if (!wasYoung && makeYoung) {
        youngRegionCount.fetch_add(1, std::memory_order_release);
    }
    metadata.regionStateBitField.SetAtomicValue(
        RegionStateBitPos::YOUNG_REGION_FLAG, YOUNG_STATE_BIT_LENGTH, makeYoung ? 1 : 0);
    if (wasYoung && !makeYoung) {
        size_t count = youngRegionCount.load(std::memory_order_relaxed);
        CHECK(count > 0);
        youngRegionCount.fetch_sub(1, std::memory_order_release);
    }
}

size_t RegionInfo::GetYoungRegionCount()
{
    return youngRegionCount.load(std::memory_order_acquire);
}

bool RegionInfo::HasYoungRegions()
{
    return GetYoungRegionCount() != 0;
}

static size_t GetPageSize() noexcept
{
    size_t pageSize = 0;
#if defined(_WIN64)
    SYSTEM_INFO systeminfo;
    GetSystemInfo(&systeminfo);
    if (systeminfo.dwPageSize != 0) {
        pageSize = systeminfo.dwPageSize;
    } else {
        // default page size is 4KB if get system page size failed.
        pageSize = 4 * KB;
    }
#elif defined(__APPLE__)
    pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    pageSize = static_cast<size_t>(getpagesize());
#endif
    return pageSize;
}

// System default page size
const size_t MRT_PAGE_SIZE = GetPageSize();
const size_t AllocatorUtils::ALLOC_PAGE_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// region unit size: same as system page size
const size_t RegionInfo::UNIT_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// regarding a object as a large object when the size is greater than 32KB or one page size,
// depending on the system page size.
const size_t RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD = MapleRuntime::MRT_PAGE_SIZE > (32 * KB) ?
                                                            MapleRuntime::MRT_PAGE_SIZE : 32 * KB;
// max size of per region is 128KB.
const size_t RegionManager::MAX_UNIT_COUNT_PER_REGION = (128 * KB) / MapleRuntime::MRT_PAGE_SIZE;
// size of huge page is 2048KB.
const size_t RegionManager::HUGE_PAGE = (2048 * KB) / MapleRuntime::MRT_PAGE_SIZE;;

class ForwardTask : public HeapWork {
public:
    ForwardTask(RegionManager& manager, RegionList& fromSpace)
        : regionManager(manager), fromRegionList(fromSpace) {}

    ~ForwardTask() = default;

    void Execute(size_t) override
    {
        while (true) {
            RegionInfo* region = fromRegionList.TakeHeadRegion(RegionInfo::RegionType::LONE_FROM_REGION);
            if (region == nullptr) { break; }
            regionManager.ForwardRegion(region);
        }
    }

private:
    RegionManager& regionManager;
    RegionList& fromRegionList;
};

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionInfo::DumpRegionInfo(LogType type) const
{
    DLOG(type, "Region index: %zu, type: %s, address: 0x%zx-0x%zx, allocated(B) %zu, live(B) %zu", GetUnitIdx(),
         GetTypeName(), GetRegionStart(), GetRegionEnd(), GetRegionAllocatedSize(), GetLiveByteCount());
}

const char* RegionInfo::GetTypeName() const
{
    static constexpr const char* regionNames[] = {
        "undefined region",
        "thread local region",
        "recent fullregion",
        "from region",
        "unmovable from region",
        "to region",
        "full pinned region",
        "recent pinned region",
        "raw pointer pinned region",
        "tl raw pointer region",
        "large region",
        "recent large region",
        "garbage region",
    };
    return regionNames[static_cast<uint8_t>(GetRegionType())];
}
#endif

void RegionInfo::VisitAllObjects(const std::function<void(BaseObject*)>&& func)
{
    if (IsLargeRegion()) {
        BaseObject* obj = from_region_addr(GetRegionStart());
        // getsize7: dense walk steps via GetSize; reject bad headers instead of SEGV.
        // On reject: stop the walk (cannot invent a step size). Caller sees partial visit.
        if (!Collector::PlausibleManagedObjectGate("VisitAllObjects", obj)) {
            return;
        }
        func(obj);
    } else if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        uintptr_t allocPtr = GetRegionAllocPtr();
        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            // getsize7: GetAllocSize → GetSize reads TypeInfo; interiors/holes SEGV here
            // (deadlock_enqfrontier: VisitLiveObjectsUntilFalse ← RouteRegion ← TryForward).
            // Refuse: break without inventing size — remaining stream is unwalkable.
            if (!Collector::PlausibleManagedObjectGate("VisitAllObjects", obj)) {
                break;
            }
            // GetAllocSize should before call func, because object maybe destroy in compact gc.
            size_t size = RegionSpace::GetAllocSize(*obj);
            func(obj);
            position += size;
        }
    }
}

bool RegionInfo::VisitLiveObjectsUntilFalse(const std::function<bool(BaseObject*)>&& func)
{
    // Skip only when a mark phase established live==0. Bare zero (e.g. non-young under minor)
    // is not an emptiness proof — fall through and consult the mark bitmap.
    if (IsKnownEmpty()) {
        return true;
    }
    // tipnull arm R: Admit/GetRoute use liveInfo0 after PrepareForwardable; walk same face.
    LiveInfo* ghostFace = metadata.liveInfo0;
    auto survivedAt = [this, ghostFace](size_t offset) -> bool {
        if (ghostFace != nullptr) {
            return ghostFace->IsSurvivedObject(offset);
        }
        return IsSurvivedObject(offset);
    };
    if (IsLargeRegion()) {
        BaseObject* obj = from_region_addr(GetRegionStart());
        if (!Collector::PlausibleManagedObjectGate("VisitLiveObjects", obj)) {
            return !survivedAt(0);
        }
        return func(obj);
    }
    if (IsSmallRegion()) {
        uintptr_t position = GetRegionStart();
        size_t offset = 0;
        uintptr_t allocPtr = GetRegionAllocPtr();

        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            // getsize7: bitten site — PreForward → ForwardObject → RouteRegion → here → GetSize.
            // tipnull: fail only if this size-walk offset is a survivor start.
            if (!Collector::PlausibleManagedObjectGate("VisitLiveObjects", obj)) {
                return !survivedAt(offset);
            }
            size_t allocSize = RegionSpace::GetAllocSize(*obj);
            position += allocSize;
            if (survivedAt(offset) && !func(obj)) { return false; }
            offset += allocSize;
        }
    }
    return true;
}

void RegionList::MergeRegionList(RegionList& srcList, RegionInfo::RegionType regionType)
{
    RegionList regionList("region list cache");
    srcList.MoveTo(regionList);
    RegionInfo* head = regionList.GetHeadRegion();
    RegionInfo* tail = regionList.GetTailRegion();
    if (head == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(listMutex);
    regionList.SetElementType(regionType);
    IncCounts(regionList.GetRegionCount(), regionList.GetUnitCount());
    if (listHead == nullptr) {
        listHead = head;
        listTail = tail;
    } else {
        tail->SetNextRegion(listHead);
        listHead->SetPrevRegion(tail);
        listHead = head;
    }
}

void RegionList::PrependRegion(RegionInfo* region, RegionInfo::RegionType type)
{
    std::lock_guard<std::mutex> lock(listMutex);
    PrependRegionLocked(region, type);
}

void RegionList::PrependRegionLocked(RegionInfo* region, RegionInfo::RegionType type)
{
    if (region == nullptr) {
        return;
    }

    DLOG(REGION, "list %p (%zu, %zu)+(%zu, %zu) prepend region %p@[%#zx+%zu, %#zx) type %u->%u", this,
        regionCount, unitCount, 1llu, region->GetUnitCount(), region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType(), type);

    region->SetRegionType(type);
    region->SetPrevRegion(nullptr);
    IncCounts(1, region->GetUnitCount());
    region->SetNextRegion(listHead);
    if (listHead == nullptr) {
        MRT_ASSERT(listTail == nullptr, "PrependRegion listTail is not null");
        listTail = region;
    } else {
        listHead->SetPrevRegion(region);
    }
    listHead = region;
}

void RegionList::DeleteRegionLocked(RegionInfo* del)
{
    MRT_ASSERT(listHead != nullptr && listTail != nullptr, "illegal region list");

    RegionInfo* pre = del->GetPrevRegion();
    RegionInfo* next = del->GetNextRegion();

    del->SetNextRegion(nullptr);
    del->SetPrevRegion(nullptr);

    DLOG(REGION, "list %p (%zu, %zu)-(%zu, %zu) delete region %p@[%#zx+%zu, %#zx) type %u", this,
        regionCount, unitCount, 1llu, del->GetUnitCount(),
        del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(), del->GetRegionType());

    DecCounts(1, del->GetUnitCount());

    if (listHead == del) { // delete head
        MRT_ASSERT(pre == nullptr, "Delete Region pre is not null");
        listHead = next;
        if (listHead == nullptr) { // now empty
            listTail = nullptr;
            return;
        }
    } else {
        pre->SetNextRegion(next);
    }

    if (listTail == del) { // delete tail
        MRT_ASSERT(next == nullptr, "Delete Region next is not null");
        listTail = pre;
        if (listTail == nullptr) { // now empty
            listHead = nullptr;
            return;
        }
    } else {
        next->SetPrevRegion(pre);
    }
}

#ifdef MRT_DEBUG
void RegionList::DumpRegionList(const char* msg)
{
    DLOG(REGION, "dump region list %s", msg);
    std::lock_guard<std::mutex> lock(listMutex);
    for (RegionInfo *region = listHead; region != nullptr; region = region->GetNextRegion()) {
        DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) units [%zu+%zu, %zu) type %u prev %p next %p", region,
            region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
            region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
            region->GetRegionType(), region->GetPrevRegion(), region->GetNextRegion());
    }
}
#endif
inline void RegionManager::TagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_HUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

inline void RegionManager::UntagHugePage(RegionInfo* region, size_t num) const
{
#if defined (__linux__) || defined(__OHOS__) || defined(__ANDROID__)
    (void)madvise(reinterpret_cast<void*>(region->GetRegionStart()), num * RegionInfo::UNIT_SIZE, MADV_NOHUGEPAGE);
#else
    (void)region;
    (void)num;
#endif
}

size_t FreeRegionManager::ReleaseGarbageRegions(size_t targetCachedSize)
{
    size_t dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
    if (dirtyBytes <= targetCachedSize) {
        VLOG(REPORT, "release heap garbage memory 0 bytes, cache %zu(%zu) bytes", dirtyBytes, targetCachedSize);
        return 0;
    }

    size_t releasedBytes = 0;
    while (dirtyBytes > targetCachedSize) {
        std::lock_guard<std::mutex> lock1(dirtyUnitTreeMutex);
        auto node = dirtyUnitTree.RootNode();
        if (node == nullptr) { break; }
        Index idx = node->GetIndex();
        UnitCount num = node->GetCount();
        dirtyUnitTree.ReleaseRootNode();

        std::lock_guard<std::mutex> lock2(releasedUnitTreeMutex);
        CHECK_DETAIL(releasedUnitTree.MergeInsert(idx, num, true), "tid %d: failed to release garbage units[%u+%u, %u)",
                     GetTid(), idx, num, idx + num);
        releasedBytes += (num * RegionInfo::UNIT_SIZE);
        dirtyBytes = dirtyUnitTree.GetTotalCount() * RegionInfo::UNIT_SIZE;
    }
    VLOG(REPORT, "release heap garbage memory %zu bytes, cache %zu(%zu) bytes",
         releasedBytes, dirtyBytes, targetCachedSize);
    return releasedBytes;
}

void RegionManager::SetMaxUnitCountForRegion()
{
    maxUnitCountPerRegion = CangjieRuntime::GetHeapParam().regionSize * KB / RegionInfo::UNIT_SIZE;
}

void RegionManager::SetMaxUnitCountForPinnedRegion()
{
    auto env = std::getenv("cjPinnedRegionSize");
    if (env == nullptr) {
        maxUnitCountPerPinnedRegion = maxUnitCountPerRegion;
        return;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    if (size >= minSize && size <= CangjieRuntime::GetHeapParam().regionSize) {
        maxUnitCountPerPinnedRegion = size * KB / RegionInfo::UNIT_SIZE;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjPinnedRegionSize parameter. Valid cjPinnedRegionSize"
            "range is [%zuKB, %zuKB].\n", minSize, CangjieRuntime::GetHeapParam().regionSize);
    }
}

void RegionManager::SetLargeObjectThreshold()
{
    auto env = std::getenv("cjLargeThresholdSize");
    if (env == nullptr) {
        // default value is 32 KB
        largeObjectThreshold = 32 * KB;
    }
    size_t size = CString::ParseSizeFromEnv(env);
    // The minimum region size is system page size, measured in KB.
    size_t minSize = MapleRuntime::MRT_PAGE_SIZE / KB;
    // 64UL: The maximum region size, measured in KB, the value is 2048 KB.
    size_t maxSize = 10 * 1024UL;
    if (size >= minSize && size <= maxSize) {
        largeObjectThreshold = size * KB;
    } else if (size != 0) {
        LOG(RTLOG_ERROR, "Unsupported cjLargeThresholdSize parameter. Valid cjLargeThresholdSize"
            "range is [%zuKB, 2048KB].\n", minSize);
    }
    size_t regionSize = CangjieRuntime::GetHeapParam().regionSize * KB;
    largeObjectThreshold = largeObjectThreshold > regionSize ? regionSize :  largeObjectThreshold;
}

void RegionManager::SetGarbageThreshold()
{
    fromSpaceGarbageThreshold = CangjieRuntime::GetGCParam().garbageThreshold;
}

#if defined(__EULER__)
void RegionManager::SetCacheRatio(double minSize, double maxSize, double defaultParam)
{
    auto env = std::getenv("cjCacheRatio");
    if (env == nullptr) {
        cacheRatio = defaultParam;
        return;
    }
    double size = CString::ParsePosDecFromEnv(env);
    if (size - minSize >= 0 && maxSize - size >= 0) {
        cacheRatio = size;
        return;
    } else {
        LOG(RTLOG_ERROR, "Unsupported cjCacheRatio parameter.Valid cjCacheRatio range is [%f, %f].\n",
            minSize, maxSize);
    }
    cacheRatio = defaultParam;
}
#endif

void RegionManager::Initialize(size_t nUnit, uintptr_t regionInfoAddr)
{
    size_t metadataSize = GetMetadataSize(nUnit);
#ifdef _WIN64
    MemMap::CommitMemory(reinterpret_cast<void*>(regionInfoAddr), metadataSize);
#endif
    this->regionInfoStart = regionInfoAddr;
    this->regionHeapStart = regionInfoAddr + metadataSize;
    this->regionHeapEnd = regionHeapStart + nUnit * RegionInfo::UNIT_SIZE;
    this->inactiveZone = regionHeapStart;
    SetMaxUnitCountForRegion();
    SetMaxUnitCountForPinnedRegion();
    SetLargeObjectThreshold();
    SetGarbageThreshold();
#if defined(__EULER__)
    SetCacheRatio(0.0, 1.0, 1.0);
#endif
    // propagate region heap layout
    RegionInfo::Initialize(nUnit, regionHeapStart);
    freeRegionManager.Initialize(nUnit);
    this->exemptedRegionThreshold = CangjieRuntime::GetHeapParam().exemptionThreshold;
    DLOG(REPORT, "region info @0x%zx+%zu, heap [0x%zx, 0x%zx), unit count %zu", regionInfoAddr, metadataSize,
         regionHeapStart, regionHeapEnd, nUnit);
}

namespace {
// STEER4: metering gated by MRT_GCV2_SCRUB_COST (default off). Product path must not
// emit per-Collect VLOG floods (calls_per_run ~1161 under ALOT).
bool ScrubCostMeterEnabled()
{
    static const bool on = []() {
        const char* v = std::getenv("MRT_GCV2_SCRUB_COST");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

std::atomic<uint64_t> g_scrubCalls{ 0 };
std::atomic<uint64_t> g_scrubNs{ 0 };
std::atomic<uint64_t> g_scrubWordsSum{ 0 };
std::atomic<uint64_t> g_scrubErasedSum{ 0 };
std::atomic<size_t> g_scrubWordsMax{ 0 };
std::atomic<size_t> g_staleAtCollect{ 0 };
} // namespace

void RegionManager::ScrubRememberedSetForRegion(RegionInfo* region)
{
    if (region == nullptr) {
        return;
    }
    MAddress rStart = static_cast<MAddress>(region->GetRegionStart());
    MAddress rEnd = static_cast<MAddress>(region->GetRegionEnd());
    // Product path clears only the two bitmap slices owned by this region.
    if (!ScrubCostMeterEnabled()) {
        (void)Heap::GetHeap().GetRememberedSet().ClearRegion(rStart, rEnd, nullptr);
        return;
    }
    size_t words = 0;
    uint64_t t0 = TimeUtil::NanoSeconds();
    size_t scrubbed = Heap::GetHeap().GetRememberedSet().ClearRegion(rStart, rEnd, &words);
    uint64_t dt = TimeUtil::NanoSeconds() - t0;
    uint64_t callNo = g_scrubCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_scrubNs.fetch_add(dt, std::memory_order_relaxed);
    g_scrubWordsSum.fetch_add(words, std::memory_order_relaxed);
    g_scrubErasedSum.fetch_add(scrubbed, std::memory_order_relaxed);
    size_t prevMax = g_scrubWordsMax.load(std::memory_order_relaxed);
    while (words > prevMax && !g_scrubWordsMax.compare_exchange_weak(prevMax, words, std::memory_order_relaxed)) {
    }
    VLOG(REPORT,
         "[GCV2][scrub-cost] call=%llu ns=%llu bitmapWords=%zu erased=%zu young=%u type=%u "
         "env=MRT_GCV2_SCRUB_COST=1",
         static_cast<unsigned long long>(callNo), static_cast<unsigned long long>(dt), words, scrubbed,
         static_cast<unsigned>(region->IsYoungRegion()), region->GetRegionType());
    if (scrubbed != 0) {
        size_t n = g_staleAtCollect.fetch_add(1, std::memory_order_relaxed);
        VLOG(REPORT,
             "[GCV2][STALE_ENTRY_AT_COLLECT] yes scrubbed=%zu bitmapWords=%zu ns=%llu region=%p "
             "[%#zx,%#zx) type=%u young=%u sample=%zu env=MRT_GCV2_SCRUB_COST=1",
             scrubbed, words, static_cast<unsigned long long>(dt), region,
             static_cast<size_t>(rStart), static_cast<size_t>(rEnd), region->GetRegionType(),
             static_cast<unsigned>(region->IsYoungRegion()), n);
    }
}

void RegionManager::DumpScrubCostAndReset(const char* point)
{
    if (!ScrubCostMeterEnabled()) {
        return;
    }
    uint64_t calls = g_scrubCalls.exchange(0, std::memory_order_relaxed);
    uint64_t ns = g_scrubNs.exchange(0, std::memory_order_relaxed);
    uint64_t wordsSum = g_scrubWordsSum.exchange(0, std::memory_order_relaxed);
    uint64_t erasedSum = g_scrubErasedSum.exchange(0, std::memory_order_relaxed);
    size_t wordsMax = g_scrubWordsMax.exchange(0, std::memory_order_relaxed);
    if (calls == 0) {
        return;
    }
    VLOG(REPORT,
         "[GCV2][scrub-cost] point=%s calls=%llu ns=%llu avgNs=%llu bitmapWordsSum=%llu "
         "bitmapWordsMax=%zu erasedSum=%llu env=MRT_GCV2_SCRUB_COST=1",
         point == nullptr ? "?" : point, static_cast<unsigned long long>(calls),
         static_cast<unsigned long long>(ns), static_cast<unsigned long long>(ns / calls),
         static_cast<unsigned long long>(wordsSum), wordsMax,
         static_cast<unsigned long long>(erasedSum));
}

void RegionManager::ReclaimRegion(RegionInfo* region)
{
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "reclaim region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    // STEER3: scrub is at CollectRegion only (see header). Reclaim/TakeRegion reuse
    // must not re-scan O(N) under remset mutex.

    // gcvroot Z2: poison reclaimed payload so use-after-free roots are identifiable (MRT_GCV2_ZAP_RECLAIM=1).
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    freeRegionManager.AddGarbageUnits(unitIndex, num);
}

void RegionManager::ReclaimRegionToMarkQuarantine(RegionInfo* region)
{
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "mark-quarantine region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
         region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());
    HeapZap::ZapReclaimedRegion(region->GetRegionStart(), region->GetRegionEnd());
    region->InitFreeUnits();
    freeRegionManager.AddMarkQuarantineUnits(unitIndex, num);
}

size_t RegionManager::ReleaseRegion(RegionInfo* region)
{
    size_t res = region->GetRegionSize();
    size_t num = region->GetUnitCount();
    size_t unitIndex = region->GetUnitIdx();
    // Large regions above the release threshold bypass CollectRegion. Invalidate
    // their two owned bitmap slices before the address range can be unmapped/reused.
    ScrubRememberedSetForRegion(region);
    if (num >= HUGE_PAGE) {
        UntagHugePage(region, num);
    }
    DLOG(REGION, "release region %p @[%#zx+%zu, %#zx) type %u", region, region->GetRegionStart(),
        region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

    region->InitFreeUnits();
    RegionInfo::ReleaseUnits(unitIndex, num);
    freeRegionManager.AddReleaseUnits(unitIndex, num);
    return res;
}

void RegionManager::ReassembleFromSpace()
{
    fromRegionList.MergeRegionList(unmovableFromRegionList, RegionInfo::RegionType::FROM_REGION);
}

void RegionManager::CountLiveObject(const BaseObject* obj)
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    region->AddLiveByteCount(obj->GetSize());
}

void RegionManager::AssembleSmallGarbageCandidates()
{
    fromRegionList.MergeRegionList(rawPointerPinnedRegionList, RegionInfo::RegionType::FROM_REGION);
    // twoflags: regions stamped post-mark-start of the previous major stay off from-space
    // until PrepareTrace clears the stamp (after this Assemble).
    {
        RegionInfo* region = recentFullRegionList.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextRegion();
            if (!region->IsNotRelocatableThisCycle()) {
                recentFullRegionList.DeleteRegion(region);
                fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
            }
            region = next;
        }
    }
    {
        RegionInfo* region = unmovableFromRegionList.GetHeadRegion();
        while (region != nullptr) {
            RegionInfo* next = region->GetNextRegion();
            if (!region->IsNotRelocatableThisCycle()) {
                unmovableFromRegionList.DeleteRegion(region);
                fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
            }
            region = next;
        }
    }

    fromRegionList.VisitAllRegions([](RegionInfo* region) { region->ClearLiveInfo(); });
}

void RegionManager::AssembleLargeGarbageCandidates()
{
    oldLargeRegionList.MergeRegionList(recentLargeRegionList, RegionInfo::RegionType::LARGE_REGION);
    for (RegionInfo* region = oldLargeRegionList.GetHeadRegion(); region != nullptr; region = region->GetNextRegion()) {
        region->ClearLiveInfo();
    }
}

void RegionManager::ClearNotRelocatableThisCycleFlags()
{
    auto clearList = [](RegionList& list) {
        list.VisitAllRegions([](RegionInfo* region) { region->SetNotRelocatableThisCycle(0); });
    };
    clearList(tlRegionList);
    clearList(recentFullRegionList);
    clearList(unmovableFromRegionList);
    clearList(fromRegionList);
    clearList(recentPinnedRegionList);
    clearList(oldPinnedRegionList);
    clearList(rawPointerPinnedRegionList);
    clearList(recentLargeRegionList);
    clearList(oldLargeRegionList);
    // Region caches may hold stamped regions until HandleTraceRegions merges them.
    clearList(fullTraceRegions);
    clearList(largeTraceRegions);
}

void RegionManager::AssemblePinnedGarbageCandidates(bool collectAll)
{
    oldPinnedRegionList.MergeRegionList(recentPinnedRegionList, RegionInfo::RegionType::FULL_PINNED_REGION);
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* nextRegion = region->GetNextRegion();
        if (collectAll && (region->GetRawPointerObjectCount() > 0)) {
            oldPinnedRegionList.DeleteRegion(region);
            rawPointerPinnedRegionList.PrependRegion(region, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
        }
        region->ClearLiveInfo();
        region = nextRegion;
    }
}

YoungCollectionStats RegionManager::PrepareYoungGarbageCandidates(const std::function<void(RegionInfo*)>& visitor)
{
    YoungCollectionStats stats;
    RegionInfo* oldRegion = fromRegionList.GetHeadRegion();
    while (oldRegion != nullptr) {
        RegionInfo* next = oldRegion->GetNextRegion();
        fromRegionList.DeleteRegion(oldRegion);
        ExemptFromRegion(oldRegion);
        oldRegion = next;
    }

    RegionInfo* region = unmovableFromRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        // twoflags: notRelocatable is major-Assemble only. Young mark re-establishes
        // liveness for POST_TRACE-stamped regions — do not skip minor CSet.
        region->ClearLiveInfo();
        visitor(region);
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() == 0) {
            unmovableFromRegionList.DeleteRegion(region);
            fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        }
        region = next;
    }

    region = recentFullRegionList.GetHeadRegion();
    while (region != nullptr) {
        RegionInfo* next = region->GetNextRegion();
        if (!region->IsYoungRegion()) {
            region = next;
            continue;
        }
        region->ClearLiveInfo();
        visitor(region);
        ++stats.candidateRegions;
        stats.candidateBytes += region->GetRegionAllocatedSize();
        if (region->GetRawPointerObjectCount() != 0) {
            region = next;
            continue;
        }
        recentFullRegionList.DeleteRegion(region);
        fromRegionList.PrependRegion(region, RegionInfo::RegionType::FROM_REGION);
        region = next;
    }
    return stats;
}

void RemoveRegionLocked(RegionList* regionList, RegionInfo* region)
{
    regionList->DeleteRegionLocked(region);
}

// forward only regions whose garbage bytes is greater than or equal to exemptedRegionThreshold.
size_t RegionManager::ExemptFromRegions()
{
    size_t forwardBytes = 0;
    size_t floatingGarbage = 0;
    size_t oldFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    double exempt = exemptedRegionThreshold;
    rawPointerPinnedRegionList.VisitAllRegions([](RegionInfo* region) {
        if (region->GetLiveByteCount() > 0) {
            region->PreserveRetainedLiveInfoUpTo(
                std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
        }
    });
    auto visitor = [this, exempt, &floatingGarbage](RegionInfo* fromRegion) {
        size_t threshold = static_cast<size_t>(exempt * fromRegion->GetRegionSize());
        size_t liveBytes = fromRegion->GetLiveByteCount();
        long rawPtrCnt = fromRegion->GetRawPointerObjectCount();
        if (liveBytes > threshold) { // ignore this region
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) exempted by forwarding: %zu units, %zu live bytes", del,
                del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount());

            CHECK(del->IsFromRegion());
            del->PreserveRetainedLiveInfo();
            RemoveRegionLocked(&fromRegionList, del);
            ExemptFromRegion(del);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        } else if (rawPtrCnt > 0) {
            RegionInfo* del = fromRegion;
            DLOG(REGION, "region %p @[0x%zx+%zu, 0x%zx) pinned by forwarding: %zu units, %zu live bytes rawPtr cnt %u",
                del, del->GetRegionStart(), del->GetRegionAllocatedSize(), del->GetRegionEnd(),
                del->GetUnitCount(), del->GetLiveByteCount(), rawPtrCnt);
            CHECK(del->IsFromRegion());
            if (liveBytes > 0) {
                del->PreserveRetainedLiveInfo();
            }
            RemoveRegionLocked(&fromRegionList, del);
            rawPointerPinnedRegionList.PrependRegion(del, RegionInfo::RegionType::RAW_POINTER_PINNED_REGION);
            floatingGarbage += (del->GetRegionSize() - del->GetLiveByteCount());
        }
    };
    fromRegionList.VisitAllRegions(visitor);

    size_t newFromBytes = fromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    size_t exemptedFromBytes = unmovableFromRegionList.GetUnitCount() * RegionInfo::UNIT_SIZE;
    VLOG(REPORT, "exempt from-space: %zu B - %zu B -> %zu B, %zu B floating garbage, %zu B to forward",
         oldFromBytes, exemptedFromBytes, newFromBytes, floatingGarbage, forwardBytes);
    return newFromBytes - forwardBytes;
}

void RegionManager::ForEachObjUnsafe(const std::function<void(BaseObject*)>& visitor,
                                     bool skipKnownEmptyRegions) const
{
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        // Finalizer reclaims concurrently (not a mutator ⇒ STW does not stop it). A unit
        // mid-InitRegionInfo can expose a transient extent (0/garbage) before the final
        // role is published. Following a bogus end lands GetUnitIdxAt(0) → named fatal+abort
        // (S1: SIGABRT under InvalidateOldTaggedRefs). Step one unit instead —
        // such units are never visitable.
        // Anchor: a1f81854 (fix/gcfix), landed here as e2293c2b; the guard below is
        // character-identical to it. Its other hunk targeted PromoteAllRegions, which
        // no longer exists on this line.
        uintptr_t nextAddr = region->GetRegionEnd();
        if (nextAddr <= regionAddr || nextAddr > inactiveZone) {
            regionAddr += RegionInfo::UNIT_SIZE;
            continue;
        }
        regionAddr = nextAddr;
        if (!region->IsValidRegion() || region->IsFreeRegion() || region->IsGarbageRegion()) {
            continue;
        }
        if (skipKnownEmptyRegions && region->IsKnownEmpty()) {
            continue;
        }
        region->VisitAllObjects([&visitor](BaseObject* object) { visitor(object); });
    }
}

void RegionManager::ForEachObjSafe(const std::function<void(BaseObject*)>& visitor) const
{
    ScopedEnterSaferegion enterSaferegion(false);
    ScopedStopTheWorld stw("visit all objects");
    ForEachObjUnsafe(visitor);
}

void RegionManager::StampCensusBoundaries()
{
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            region->StampCensusBoundary();
        }
    }
}

void RegionManager::PromoteAllRegions()
{
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (region->IsValidRegion() && !region->IsGarbageRegion()) {
            size_t liveBytes = region->GetLiveByteCount();
            if (liveBytes > 0) {
                region->PreserveRetainedLiveInfoUpTo(
                    std::min(region->GetCensusBoundary(), region->GetRegionAllocPtr()));
            } else if (region->GetRawPointerObjectCount() == 0) {
                region->PreserveRetainedLiveInfo(region->GetRegionStart());
            }
            region->SetYoungRegionFlag(0);
            region->SetYoungAge(0);
        }
    }
}

RegionInfo* RegionManager::TakeRegion(size_t num, RegionInfo::UnitRole type, bool expectPhysicalMem)
{
    // a chance to invoke heuristic gc.
    if (!Heap::GetHeap().IsGcStarted()) {
        Collector& collector = Heap::GetHeap().GetCollector();
        size_t heapThreshold = collector.GetGCStats().GetThreshold();
        size_t youngRegionTriggerBytes = 32 * MB;
        // genperf: default-off arm B — raise young trigger out of reach so minor never fires;
        // barriers/remset still run. Unset must match product path bit-for-bit.
        const char* disableMinorEnv = std::getenv("MRT_GCV2_DISABLE_MINOR");
        const bool disableMinor =
            disableMinorEnv != nullptr && std::strcmp(disableMinorEnv, "1") == 0;
        if (disableMinor) {
            youngRegionTriggerBytes = std::numeric_limits<size_t>::max();
        }
        const char* jvmYoungTriggerEnv = std::getenv("MRT_GCV2_JVM_YOUNG_TRIGGER");
        const bool useJvmYoungTrigger =
            !disableMinor && jvmYoungTriggerEnv != nullptr && std::strcmp(jvmYoungTriggerEnv, "1") == 0;
        size_t youngTriggerFloor = 0;
        size_t youngTriggerTarget = 0;
        size_t youngTriggerCeiling = 0;
        if (useJvmYoungTrigger) {
            // G1 sizes young between 5% and 60% of its heap. This runtime has no eden/survivor
            // pause controller, so apply those bounds to the HEU budget and target half that budget.
            constexpr size_t youngTriggerFloorPercent = 5;
            constexpr size_t youngTriggerTargetPercent = 50;
            constexpr size_t youngTriggerCeilingPercent = 60;
            youngTriggerFloor = heapThreshold * youngTriggerFloorPercent / 100;
            youngTriggerTarget = heapThreshold * youngTriggerTargetPercent / 100;
            youngTriggerCeiling = heapThreshold * youngTriggerCeilingPercent / 100;
            youngRegionTriggerBytes =
                std::min(std::max(youngTriggerTarget, youngTriggerFloor), youngTriggerCeiling);
            CHECK_DETAIL(youngRegionTriggerBytes < heapThreshold,
                         "young GC threshold %zu must stay below HEU threshold %zu",
                         youngRegionTriggerBytes, heapThreshold);
        }
        size_t youngAllocated = GetYoungAllocatedSize();
        if (youngAllocated >= youngRegionTriggerBytes) {
            if (useJvmYoungTrigger) {
                VLOG(REPORT,
                     "[GCV2][jvm-young-trigger] young=%zu trigger=%zu HEU=%zu floor=%zu target=%zu ceiling=%zu "
                     "invariant=%d",
                     youngAllocated, youngRegionTriggerBytes, heapThreshold, youngTriggerFloor, youngTriggerTarget,
                     youngTriggerCeiling, youngRegionTriggerBytes < heapThreshold);
            }
            DLOG(ALLOC, "request young gc: allocated %zu, threshold %zu", youngAllocated, youngRegionTriggerBytes);
            collector.RequestGC(GC_REASON_YOUNG, true);
        } else {
            size_t allocated = Heap::GetHeap().GetAllocator().AllocatedBytes();
            if (allocated >= heapThreshold) {
                DLOG(ALLOC, "request heu gc: allocated %zu, threshold %zu", allocated, heapThreshold);
                collector.RequestGC(GC_REASON_HEU, true);
            }
        }
    }

    // check for allocation since we do not want gc threads and mutators do any harm to each other.
    size_t size = num * RegionInfo::UNIT_SIZE;
    RequestForRegion(size);

#if !defined(__OHOS__)
    size_t gatedBytes = 0;
    RegionInfo* head = TakeReclaimableGarbageRegion(&gatedBytes);
    if (head != nullptr) {
        DLOG(REGION, "take garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
        if (head->GetUnitCount() == num) {
            TraceClear::NoteRegionEvent(head->GetRegionStart(), head->GetRegionSize(), "garbage_reuse", head,
                                        head->GetLiveByteCount(),
                                        static_cast<unsigned int>(head->IsGhostFromRegion()),
                                        static_cast<unsigned int>(head->GetRegionType()),
                                        static_cast<unsigned int>(head->GetRouteState()));
            auto idx = head->GetUnitIdx();
            RegionInfo::ClearUnits(idx, num);
            DLOG(REGION, "reuse garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            return RegionInfo::InitRegion(idx, num, type);
        } else {
            DLOG(REGION, "reclaim garbage region %p@[%#zx, %#zx)", head, head->GetRegionStart(), head->GetRegionEnd());
            ReclaimRegion(head);
        }
    }
#else
    size_t gatedBytes = GetGatedGarbageBytes();
#endif

    RegionInfo* region = freeRegionManager.TakeRegion(num, type, expectPhysicalMem);
    if (region != nullptr) {
        if (num >= HUGE_PAGE) {
            TagHugePage(region, num);
        }
        return region;
    }

    // when free regions are not enough for allocation
    if (num <= GetInactiveUnitCount()) {
        uintptr_t addr = inactiveZone.fetch_add(size);
        if (addr < regionHeapEnd - size) {
            region = RegionInfo::InitRegionAt(addr, num, type);
            size_t idx = region->GetUnitIdx();
#ifdef _WIN64
            MemMap::CommitMemory(
                reinterpret_cast<void*>(RegionInfo::GetUnitAddress(idx)), num * RegionInfo::UNIT_SIZE);
#endif
            (void)idx; // eliminate compilation warning
            DLOG(REGION, "take inactive units [%zu+%zu, %zu) at [0x%zx, 0x%zx)", idx, num, idx + num,
                 RegionInfo::GetUnitAddress(idx), RegionInfo::GetUnitAddress(idx + num));
            if (num >= HUGE_PAGE) {
                TagHugePage(region, num);
            }
            if (expectPhysicalMem) {
                RegionInfo::ClearUnits(idx, num);
            }
            return region;
        } else {
            (void)inactiveZone.fetch_sub(size);
        }
    }

    if (gatedBytes > 0) {
        static std::atomic<size_t> supplyGatedPressureCount { 0 };
        size_t n = supplyGatedPressureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            VLOG(REPORT, "[Alloc] supply_gated_pressure gated_bytes=%zu n=%zu", gatedBytes, n);
        }
    }
    return nullptr;
}

void RegionManager::ForwardFromRegions(GCThreadPool* threadPool)
{
    if (threadPool != nullptr) {
        int32_t threadNum = threadPool->GetMaxThreadNum() + 1;
        // We won't change fromRegionList during gc, so we can use it without lock.
        size_t regionCount = fromRegionList.GetRegionCount();
        if (UNLIKELY(regionCount == 0)) {
            return;
        }

        // we start threadPool before adding work so that we can concurrently add tasks;
        threadPool->Start();
        for (int32_t i = 0; i < threadNum; ++i) {
            threadPool->AddWork(new (std::nothrow) ForwardTask(*this, fromRegionList));
        }
        threadPool->WaitFinish();
    } else {
        ForwardFromRegions();
    }
}

void RegionManager::ExemptFromRegion(RegionInfo* region)
{
    unmovableFromRegionList.PrependRegion(region, RegionInfo::RegionType::UNMOVABLE_FROM_REGION);
}

void RegionManager::ForwardFromRegions()
{
    RegionInfo* fromRegion = fromRegionList.GetHeadRegion();
    while (fromRegion != nullptr) {
        MRT_ASSERT(fromRegion->IsValidRegion(), "the head region of fromRegionList is invalid");
        RegionInfo* region = fromRegion;
        fromRegion = fromRegion->GetNextRegion();
        ForwardRegion(region);
    }

    VLOG(REPORT, "forward %zu from-region units", fromRegionList.GetUnitCount());

    AllocBuffer* allocBuffer = AllocBuffer::GetAllocBuffer();
    if (LIKELY(allocBuffer != nullptr)) {
        allocBuffer->ClearRegion(); // clear region for next GC
    }
}

size_t RegionManager::CollectFreePinnedSlots(RegionInfo* region)
{
    // traverse pinned region to reclaim free pinned objects.
    size_t start = region->GetRegionStart();
    size_t garbageSize = 0;
    region->VisitAllObjects([this, region, start, &garbageSize](BaseObject* object) {
        size_t offset = reinterpret_cast<MAddress>(object) - start;
        if (!region->IsSurvivedObject(offset)) {
            size_t objSize = object->GetSize();
            DLOG(ALLOC, "reclaim pinned obj %p<%p>(%zu)", object, object->GetTypeInfo(), objSize);
            garbageSize += objSize;
            std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
            ReleaseNativeResource(object);
            freePinnedSlotLists.PushFront(object);
        }
    });
    return garbageSize;
}

size_t RegionManager::CollectPinnedGarbage()
{
    {
        std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
        freePinnedSlotLists.Clear();
    }
    size_t garbageSize = 0;
    RegionInfo* region = oldPinnedRegionList.GetHeadRegion();
    while (region != nullptr) {
        if (region->IsKnownEmpty()) {
            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldPinnedRegionList.DeleteRegion(del);

            auto fixToObj = [](BaseObject* obj) { ReleaseNativeResource(obj); };
            del->VisitAllObjects(fixToObj);

            garbageSize += CollectRegion(del);
            continue;
        } else {
            garbageSize += CollectFreePinnedSlots(region);
            region = region->GetNextRegion();
        }
    }
    return garbageSize;
}

size_t RegionManager::CollectLargeGarbage()
{
    size_t garbageSize = 0;
    RegionInfo* region = oldLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        // for large region, the offset of obj is 0
        if (!region->IsSurvivedObject(0)) {
            DLOG(REGION, "reclaim large region %p@[0x%zx+%zu, 0x%zx) type %u", region, region->GetRegionStart(),
                 region->GetRegionAllocatedSize(), region->GetRegionEnd(), region->GetRegionType());

            RegionInfo* del = region;
            region = region->GetNextRegion();
            oldLargeRegionList.DeleteRegion(del);
            if (del->GetRegionSize() > RegionInfo::LARGE_OBJECT_RELEASE_THRESHOLD) {
                garbageSize += ReleaseRegion(del);
            } else {
                garbageSize += CollectRegion(del);
            }
        } else {
            region->ResetMarkBit();
            region = region->GetNextRegion();
        }
    }

    region = recentLargeRegionList.GetHeadRegion();
    while (region != nullptr) {
        region->ResetMarkBit();
        region = region->GetNextRegion();
    }

    return garbageSize;
}

#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
void RegionManager::DumpRegionInfo() const
{
    if (!ENABLE_LOG(ALLOC)) {
        return;
    }
    for (uintptr_t regionAddr = regionHeapStart; regionAddr < inactiveZone;) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(regionAddr);
        regionAddr = region->GetRegionEnd();
        if (!region->IsFreeRegion()) {
            region->DumpRegionInfo(ALLOC);
        }
    }
}
#endif

void RegionManager::DumpRegionStats(const char* msg, bool dumpToError) const
{
    size_t totalSize = regionHeapEnd - regionHeapStart;
    size_t totalUnits = totalSize / RegionInfo::UNIT_SIZE;
    size_t activeSize = inactiveZone - regionHeapStart;
    size_t activeUnits = activeSize / RegionInfo::UNIT_SIZE;

    size_t tlRegions = tlRegionList.GetRegionCount();
    size_t tlUnits = tlRegionList.GetUnitCount();
    size_t tlSize = tlUnits * RegionInfo::UNIT_SIZE;
    size_t allocTLSize = tlRegionList.GetAllocatedSize();

    size_t fromRegions = fromRegionList.GetRegionCount();
    size_t fromUnits = fromRegionList.GetUnitCount();
    size_t fromSize = fromUnits * RegionInfo::UNIT_SIZE;
    size_t allocFromSize = fromRegionList.GetAllocatedSize();

    size_t recentFullRegions = recentFullRegionList.GetRegionCount();
    size_t recentFullUnits = recentFullRegionList.GetUnitCount();
    size_t recentFullSize = recentFullUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentFullSize = recentFullRegionList.GetAllocatedSize();

    size_t garbageRegions = garbageRegionList.GetRegionCount();
    size_t garbageUnits = garbageRegionList.GetUnitCount();
    size_t garbageSize = garbageUnits * RegionInfo::UNIT_SIZE;
    size_t allocGarbageSize = garbageRegionList.GetAllocatedSize();

    size_t pinnedRegions = oldPinnedRegionList.GetRegionCount();
    size_t pinnedUnits = oldPinnedRegionList.GetUnitCount();
    size_t pinnedSize = pinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocPinnedSize = oldPinnedRegionList.GetAllocatedSize();

    size_t recentPinnedRegions = recentPinnedRegionList.GetRegionCount();
    size_t recentPinnedUnits = recentPinnedRegionList.GetUnitCount();
    size_t recentPinnedSize = recentPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentPinnedSize = recentPinnedRegionList.GetAllocatedSize();

    size_t rawPointerPinnedRegions = rawPointerPinnedRegionList.GetRegionCount();
    size_t rawPointerPinnedUnits = rawPointerPinnedRegionList.GetUnitCount();
    size_t rawPointerPinnedSize = rawPointerPinnedUnits * RegionInfo::UNIT_SIZE;
    size_t allocRawPointerPinnedSize = rawPointerPinnedRegionList.GetAllocatedSize();

    size_t largeRegions = oldLargeRegionList.GetRegionCount();
    size_t largeUnits = oldLargeRegionList.GetUnitCount();
    size_t largeSize = largeUnits * RegionInfo::UNIT_SIZE;
    size_t allocLargeSize = oldLargeRegionList.GetAllocatedSize();

    size_t recentlargeRegions = recentLargeRegionList.GetRegionCount();
    size_t recentlargeUnits = recentLargeRegionList.GetUnitCount();
    size_t recentLargeSize = recentlargeUnits * RegionInfo::UNIT_SIZE;
    size_t allocRecentLargeSize = recentLargeRegionList.GetAllocatedSize();

    size_t allHeapSize = regionHeapEnd - regionHeapStart;
    size_t allUnits = allHeapSize / RegionInfo::UNIT_SIZE;
    size_t inactiveUnits = (regionHeapEnd - inactiveZone) / RegionInfo::UNIT_SIZE;
    
    size_t usedUnitCount = GetUsedUnitCount();
    size_t usedObjSize = GetAllocatedSize();
    size_t releasedUnits = freeRegionManager.GetReleasedUnitCount();
    size_t dirtyUnits = freeRegionManager.GetDirtyUnitCount();
    size_t dirtySize = dirtyUnits * RegionInfo::UNIT_SIZE;

    size_t totalUnitCount = usedUnitCount + garbageUnits + dirtyUnits;
    size_t totalObjSize = usedObjSize + garbageSize + dirtyUnits * RegionInfo::UNIT_SIZE;

    double objectCapacity = (allHeapSize > 0) ? static_cast<double>(totalObjSize) / allHeapSize : 0.0;
    double unitCapacity = (allUnits > 0) ? static_cast<double>(totalUnitCount) / allUnits : 0.0;
    double usedObjectCapacity = (allHeapSize > 0) ? static_cast<double>(usedObjSize) / allHeapSize : 0.0;
    double usedUnitCapacity = (allUnits > 0) ? static_cast<double>(usedUnitCount) / allUnits : 0.0;
    double objFragRate = 1.0 - objectCapacity;
    double unitFragRate = 1.0 - unitCapacity;
    double usedObjFragRate = 1.0 - usedObjectCapacity;
    double usedUnitFragRate = 1.0 - usedUnitCapacity;

#define DUMP_REGION_STATS_LOG(format, ...)                  \
    do {                                                    \
        VLOG(REPORT, format, ##__VA_ARGS__);                \
        if (dumpToError) {                                  \
            LOG(RTLOG_ERROR, format, ##__VA_ARGS__);        \
        }                                                   \
    } while (false)

    DUMP_REGION_STATS_LOG("%s", msg);

    DUMP_REGION_STATS_LOG("\ttotal units: %zu (%zu B)", totalUnits, totalSize);
    DUMP_REGION_STATS_LOG("\tactive units: %zu (%zu B)", activeUnits, activeSize);
    DUMP_REGION_STATS_LOG("\tinactive units: %zu (%zu B)", inactiveUnits, inactiveUnits * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\ttl-regions %zu: %zu units (%zu B, alloc %zu)", tlRegions,  tlUnits, tlSize, allocTLSize);
    DUMP_REGION_STATS_LOG("\tfrom-regions %zu: %zu units (%zu B, alloc %zu)", fromRegions,  fromUnits, fromSize,
                          allocFromSize);
    DUMP_REGION_STATS_LOG("\trecent-full regions %zu: %zu units (%zu B, alloc %zu)",
                          recentFullRegions, recentFullUnits, recentFullSize, allocRecentFullSize);
    DUMP_REGION_STATS_LOG("\tgarbage regions %zu: %zu units (%zu B, alloc %zu)",
                          garbageRegions, garbageUnits, garbageSize, allocGarbageSize);
    DUMP_REGION_STATS_LOG("\tpinned regions %zu: %zu units (%zu B, alloc %zu)",
                          pinnedRegions, pinnedUnits, pinnedSize, allocPinnedSize);
    DUMP_REGION_STATS_LOG("\trecent pinned regions %zu: %zu units (%zu B, alloc %zu)",
                          recentPinnedRegions, recentPinnedUnits, recentPinnedSize, allocRecentPinnedSize);
    DUMP_REGION_STATS_LOG("\trawPointer pinned regions %zu: %zu units (%zu B, alloc %zu)",
                          rawPointerPinnedRegions, rawPointerPinnedUnits, rawPointerPinnedSize,
                          allocRawPointerPinnedSize);
    DUMP_REGION_STATS_LOG("\tlarge-object regions %zu: %zu units (%zu B, alloc %zu)",
                          largeRegions, largeUnits, largeSize, allocLargeSize);
    DUMP_REGION_STATS_LOG("\trecent large-object regions %zu: %zu units (%zu B, alloc %zu)",
                          recentlargeRegions, recentlargeUnits, recentLargeSize, allocRecentLargeSize);
    DUMP_REGION_STATS_LOG("\tused summary: usedUnits %zu (%zu B), usedObjSize %zu B",
                          usedUnitCount, usedUnitCount * RegionInfo::UNIT_SIZE, usedObjSize);

    size_t releasedMaxBlock = freeRegionManager.GetReleasedMaxBlock();
    size_t dirtyMaxBlock = freeRegionManager.GetDirtyMaxBlock();
    size_t releasedNodeCount = freeRegionManager.GetReleasedNodeCount();
    size_t dirtyNodeCount = freeRegionManager.GetDirtyNodeCount();
    DUMP_REGION_STATS_LOG("\treleased units: %zu (%zu B), nodes: %zu, maxBlock: %zu units (%zu B)",
                          releasedUnits, releasedUnits * RegionInfo::UNIT_SIZE,
                          releasedNodeCount,
                          releasedMaxBlock, releasedMaxBlock * RegionInfo::UNIT_SIZE);
    DUMP_REGION_STATS_LOG("\tdirty units: %zu (%zu B), nodes: %zu, maxBlock: %zu units (%zu B)",
                          dirtyUnits, dirtyUnits * RegionInfo::UNIT_SIZE, dirtyNodeCount,
                          dirtyMaxBlock,
                          dirtyMaxBlock * RegionInfo::UNIT_SIZE);

    DUMP_REGION_STATS_LOG("\tgarbage+dirty summary: garbageUnits %zu (%zu B, allocObj %zu), dirtyUnits %zu (%zu B)",
                          garbageUnits, garbageSize, allocGarbageSize, dirtyUnits, dirtySize);
    DUMP_REGION_STATS_LOG("\tobjectCapacity: %.4f (totalObjSize %zu / allHeapSize %zu), objFragRate: %.4f",
                          objectCapacity, totalObjSize, allHeapSize, objFragRate);
    DUMP_REGION_STATS_LOG("\tunitCapacity: %.4f (totalUnitCount %zu / allUnits %zu), unitFragRate: %.4f",
                          unitCapacity, totalUnitCount, allUnits, unitFragRate);
    DUMP_REGION_STATS_LOG("\tusedObjectCapacity: %.4f (usedObjSize %zu / allHeapSize %zu), usedObjFragRate: %.4f",
                          usedObjectCapacity, usedObjSize, allHeapSize, usedObjFragRate);
    DUMP_REGION_STATS_LOG("\tusedUnitCapacity: %.4f (usedUnitCount %zu / allUnits %zu), usedUnitFragRate: %.4f",
                          usedUnitCapacity, usedUnitCount, allUnits, usedUnitFragRate);
#undef DUMP_REGION_STATS_LOG

    TRACE_COUNT("CJRT_GC_totalSize", totalSize);
    TRACE_COUNT("CJRT_GC_totalUnits", totalUnits);
    TRACE_COUNT("CJRT_GC_activeSize", activeSize);
    TRACE_COUNT("CJRT_GC_activeUnits", activeUnits);
    TRACE_COUNT("CJRT_GC_tlRegions", tlRegions);
    TRACE_COUNT("CJRT_GC_tlUnits", tlUnits);
    TRACE_COUNT("CJRT_GC_tlSize", tlSize);
    TRACE_COUNT("CJRT_GC_allocTLSize", allocTLSize);
    TRACE_COUNT("CJRT_GC_fromRegions", fromRegions);
    TRACE_COUNT("CJRT_GC_fromUnits", fromUnits);
    TRACE_COUNT("CJRT_GC_fromSize", fromSize);
    TRACE_COUNT("CJRT_GC_allocFromSize", allocFromSize);
    TRACE_COUNT("CJRT_GC_recentFullRegions", recentFullRegions);
    TRACE_COUNT("CJRT_GC_recentFullUnits", recentFullUnits);
    TRACE_COUNT("CJRT_GC_recentFullSize", recentFullSize);
    TRACE_COUNT("CJRT_GC_allocRecentFullSize", allocRecentFullSize);
    TRACE_COUNT("CJRT_GC_garbageRegions", garbageRegions);
    TRACE_COUNT("CJRT_GC_garbageUnits", garbageUnits);
    TRACE_COUNT("CJRT_GC_garbageSize", garbageSize);
    TRACE_COUNT("CJRT_GC_allocGarbageSize", allocGarbageSize);
    TRACE_COUNT("CJRT_GC_pinnedRegions", pinnedRegions);
    TRACE_COUNT("CJRT_GC_pinnedUnits", pinnedUnits);
    TRACE_COUNT("CJRT_GC_pinnedSize", pinnedSize);
    TRACE_COUNT("CJRT_GC_allocPinnedSize", allocPinnedSize);
    TRACE_COUNT("CJRT_GC_recentPinnedRegions", recentPinnedRegions);
    TRACE_COUNT("CJRT_GC_recentPinnedUnits", recentPinnedUnits);
    TRACE_COUNT("CJRT_GC_recentPinnedSize", recentPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRecentPinnedSize", allocRecentPinnedSize);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedRegions", rawPointerPinnedRegions);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedUnits", rawPointerPinnedUnits);
    TRACE_COUNT("CJRT_GC_rawPointerPinnedSize", rawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_allocRawPointerPinnedSize", allocRawPointerPinnedSize);
    TRACE_COUNT("CJRT_GC_largeRegions", largeRegions);
    TRACE_COUNT("CJRT_GC_largeUnits", largeUnits);
    TRACE_COUNT("CJRT_GC_largeSize", largeSize);
    TRACE_COUNT("CJRT_GC_allocLargeSize", allocLargeSize);
    TRACE_COUNT("CJRT_GC_recentlargeRegions", recentlargeRegions);
    TRACE_COUNT("CJRT_GC_recentlargeUnits", recentlargeUnits);
    TRACE_COUNT("CJRT_GC_recentLargeSize", recentLargeSize);
    TRACE_COUNT("CJRT_GC_allocRecentLargeSize", allocRecentLargeSize);
    TRACE_COUNT("CJRT_GC_usedUnits", usedUnitCount);
    TRACE_COUNT("CJRT_GC_releasedUnits", releasedUnits);
    TRACE_COUNT("CJRT_GC_dirtyUnits", dirtyUnits);
    TRACE_COUNT("CJRT_GC_listedUnits", totalUnitCount);
    [[maybe_unused]] constexpr size_t decimalPrecision = 10000;
    TRACE_COUNT("CJRT_GC_objectCapacity", static_cast<size_t>(objectCapacity * decimalPrecision));
    TRACE_COUNT("CJRT_GC_unitCapacity", static_cast<size_t>(unitCapacity * decimalPrecision));
}

RegionInfo* RegionManager::AllocateThreadLocalRegion(bool expectPhysicalMem, bool youngRegion)
{
    RegionInfo* region = TakeRegion(maxUnitCountPerRegion, RegionInfo::UnitRole::SMALL_SIZED_UNITS, expectPhysicalMem);
    if (region != nullptr) {
        {
            region->SetYoungRegionFlag(youngRegion ? 1 : 0);
            GCPhase phase = Heap::GetHeap().GetCollector().GetGCPhase();
            if (phase == GC_PHASE_TRACE || phase == GC_PHASE_CLEAR_SATB_BUFFER) {
                region->SetTraceRegionFlag(1);
            }
            // twoflags: POST_TRACE+ only (TRACE uses isTraceRegion). No CLEAR_SATB.
            if (phase == GC_PHASE_POST_TRACE || phase == GC_PHASE_PREFORWARD ||
                phase == GC_PHASE_FORWARD) {
                region->SetNotRelocatableThisCycle(1);
            }
            tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
            DLOG(REGION, "alloc tl-region %p @[0x%zx+%zu, 0x%zx) units[%zu+%zu, %zu) type %u",
                region, region->GetRegionStart(), region->GetRegionSize(), region->GetRegionEnd(),
                region->GetUnitIdx(), region->GetUnitCount(), region->GetUnitIdx() + region->GetUnitCount(),
                region->GetRegionType());
        }
    }

    return region;
}

void RegionManager::RequestForRegion(size_t size)
{
    if (IsGcThread()) {
        // gc thread is always permitted for allocation.
        return;
    }

    Heap& heap = Heap::GetHeap();
    GCStats& gcstats = heap.GetCollector().GetGCStats();
    size_t allocatedBytes = GetAllocatedSize() - gcstats.liveBytesAfterGC;
    constexpr double pi = 3.14;
    size_t availableBytesAfterGC = heap.GetMaxCapacity() - gcstats.liveBytesAfterGC;
    double heuAllocRate = std::cos((pi / 2.0) * allocatedBytes / availableBytesAfterGC) * gcstats.collectionRate;
    // for maximum performance, choose the larger one.
    double allocRate = std::max(
        static_cast<double>(CangjieRuntime::GetHeapParam().allocationRate) * MB / SECOND_TO_NANO_SECOND, heuAllocRate);
    size_t waitTime = static_cast<size_t>(size / allocRate);
    uint64_t now = TimeUtil::NanoSeconds();
    if (prevRegionAllocTime + waitTime <= now) {
        prevRegionAllocTime = TimeUtil::NanoSeconds();
        return;
    }

    uint64_t sleepTime = std::min<uint64_t>(CangjieRuntime::GetHeapParam().allocationWaitTime,
                                  prevRegionAllocTime + waitTime - now);
    DLOG(ALLOC, "wait %zu ns to alloc %zu(B)", sleepTime, size);
    std::this_thread::sleep_for(std::chrono::nanoseconds{ sleepTime });
    prevRegionAllocTime = TimeUtil::NanoSeconds();
}

bool RegionManager::RouteOrCompactRegionImpl(RegionInfo* region)
{
    CHECK(region->IsRoutingState());
    CHECK_DETAIL(region->GetRawPointerObjectCount() <= 0, "pinned region shouldn't be moved");
    // tipnull densify (FULL size-walk only): MarkBits multi-bit ranges let interiors pass
    // Admit while VisitLive only copies starts → FORWARDED + null tip. Rebuild liveInfo0
    // to size-walk ∩ prior-survived starts so Admit domain ⊆ Copy domain.
    //
    // SEGV fix (si_addr=0x8 MAPERR in CJ_MCC_ReadRefField, rdi=0 rbx=0x8): earlier densify
    // cleared bitmaps on *partial* walk (gate break mid-region), zeroing liveByteCount and
    // wiping unwalked survivors → ROUTED/FORWARDED/Collect with mutator still holding from
    // → ReadRefField(null,+8). Only densify when walk reaches allocPtr.
    if (region->IsSmallRegion() && region->GetLiveInfo0ForProbe() != nullptr &&
        !region->IsKnownEmpty()) {
        LiveInfo* ghost = region->GetLiveInfo0ForProbe();
        RegionBitmap* mb = ghost->markBitmap;
        RegionBitmap* rb = ghost->resurrectBitmap;
        constexpr size_t kMaxStarts = 8192;
        size_t startOff[kMaxStarts];
        size_t startSz[kMaxStarts];
        size_t nStarts = 0;
        size_t liveBytes = 0;
        uintptr_t position = region->GetRegionStart();
        uintptr_t allocPtr = region->GetRegionAllocPtr();
        bool fullWalk = true;
        while (position < allocPtr) {
            BaseObject* obj = from_region_addr(position);
            if (!Collector::PlausibleManagedObjectGate("tipnull-densify", obj)) {
                fullWalk = false;
                break;
            }
            size_t allocSize = RegionSpace::GetAllocSize(*obj);
            if (allocSize == 0) {
                fullWalk = false;
                break;
            }
            size_t offset = position - region->GetRegionStart();
            if (ghost->IsSurvivedObject(offset)) {
                if (nStarts >= kMaxStarts) {
                    fullWalk = false;
                    break;
                }
                startOff[nStarts] = offset;
                startSz[nStarts] = allocSize;
                ++nStarts;
                liveBytes += allocSize;
            }
            position += allocSize;
        }
        if (fullWalk && position == allocPtr) {
            auto clearAll = [](RegionBitmap* bm) {
                if (bm == nullptr) {
                    return;
                }
                size_t wc = bm->wordCnt.load(std::memory_order_acquire);
                for (size_t i = 0; i < wc; ++i) {
                    bm->markWords[i].store(0, std::memory_order_relaxed);
                }
                for (uint8_t p = 0; p < RegionBitmap::factor; ++p) {
                    bm->partLiveBytes[p].store(0, std::memory_order_relaxed);
                }
                bm->liveBytes.store(0, std::memory_order_relaxed);
            };
            clearAll(mb);
            clearAll(rb);
            size_t regionSize = region->GetGhostRegionSize();
            if (regionSize == 0) {
                regionSize = region->GetRegionSize();
            }
            for (size_t i = 0; i < nStarts; ++i) {
                if (mb != nullptr) {
                    (void)mb->MarkBits(startOff[i], startSz[i], regionSize);
                }
            }
            region->ResetLiveByteCount();
            if (liveBytes > 0) {
                region->AddLiveByteCount(liveBytes);
            }
            static std::atomic<size_t> densifyN{ 0 };
            size_t dn = densifyN.fetch_add(1, std::memory_order_relaxed) + 1;
            if (dn <= 32) {
                LOG(RTLOG_ERROR,
                    "[GCV2][tipnull] densify region=%p starts=%zu liveBytes=%zu n=%zu",
                    region, nStarts, liveBytes, dn);
            }
        }
    }
    size_t fromBytes = region->GetLiveByteCount();
    AllocBuffer* buffer = AllocBuffer::GetOrCreateAllocBuffer();
    RegionInfo* toRegion1 = buffer->GetRegion();
    CHECK(region != toRegion1);
    bool result;
    if (toRegion1 == RegionInfo::NullRegion()) {
        toRegion1 = AllocateThreadLocalRegion(false, false);
        if (toRegion1 == nullptr) {
            CompactRegion(region);
            toRegion1 = region;
            result = false;
        } else {
            toRegion1->Alloc(fromBytes);
            result = true;
        }
        buffer->SetRegion(toRegion1);
        size_t toRegion1Start = toRegion1->GetRegionStart();
        region->SetRouteInfo(toRegion1Start, fromBytes);
        DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx~%#zx, %#zx)",
            region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
            toRegion1Start, toRegion1Start + fromBytes, toRegion1->GetRegionEnd());
        return result;
    }

    size_t toRegion1Capacity = toRegion1->GetAvailableSize();
    MAddress toRegion1Addr = toRegion1->GetRegionAllocPtr();
    if (fromBytes <= toRegion1Capacity) {
        toRegion1->Alloc(fromBytes);
        region->SetRouteInfo(toRegion1Addr, fromBytes);
        DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx, %#zx~%#zx, %#zx)",
            region, region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1,
            toRegion1->GetRegionStart(), toRegion1Addr, toRegion1Addr + fromBytes, toRegion1->GetRegionEnd());
        return true;
    }
    size_t toRegion1Waste = toRegion1Capacity;
    BaseObject* leftObject = nullptr;
    (void)region->VisitLiveObjectsUntilFalse([&toRegion1Waste, &leftObject](BaseObject* obj) {
        size_t objSz = RegionSpace::GetAllocSize(*obj);
        if (toRegion1Waste >= objSz) {
            toRegion1Waste -= objSz;
            return true;
        } else {
            leftObject = obj;
            return false;
        }
    });
    MAddress usedBytes1 = toRegion1Capacity - toRegion1Waste;
    MAddress usedBytes2 = fromBytes - usedBytes1;
    CHECK(toRegion1->IsThreadLocalRegion());
    {
        RemoveThreadLocalRegion(toRegion1);
        EnlistFullThreadLocalRegion(toRegion1);
    }

    RegionInfo* toRegion2 = AllocateThreadLocalRegion(false, false);
    CHECK(region != toRegion2);
    if (toRegion2 != nullptr) {
        toRegion1->Alloc(usedBytes1);
        CHECK(toRegion2->Alloc(usedBytes2) != 0);
        result = true;
    } else {
        CompactRegion(region, toRegion1);
        toRegion2 = region; // region is partially compacted into itself.
        result = false;
    }
    buffer->SetRegion(toRegion2);
    uint32_t toRegion2Idx = toRegion2->GetUnitIdx();
    region->SetRouteInfo(toRegion1Addr, usedBytes1, toRegion2Idx);
    DLOG(FORWARD, "route region %p@[%#zx+%zu, %#zx) => %p@[%#zx, %#zx~%#zx, %#zx) & %p@[%#zx~%#zx, %#zx)", region,
        region->GetRegionStart(), fromBytes, region->GetRegionEnd(), toRegion1, toRegion1->GetRegionStart(),
        toRegion1Addr, toRegion1Addr + usedBytes1, toRegion1->GetRegionEnd(), toRegion2,
        toRegion2->GetRegionStart(), toRegion2->GetRegionStart() + usedBytes2, toRegion2->GetRegionEnd());
    return result;
}

void RegionManager::CompactRegion(RegionInfo* region)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u", region, regionStart,
        region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType());
    MAddress regionLimit = region->GetRegionAllocPtr();
    region->SetRegionAllocPtr(regionStart);
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    for (MAddress currentPtr = regionStart; currentPtr < regionLimit;) {
        BaseObject* currentObj = from_region_addr(currentPtr);
        // getsize7: dense compact walk — same GetSize hazard as VisitLiveObjects.
        // On reject: stop; leave remaining bytes uncleared beyond current allocPtr rebuild.
        if (!Collector::PlausibleManagedObjectGate("CompactRegion", currentObj)) {
            break;
        }
        size_t size = currentObj->GetSize();
        size_t offset = currentPtr - regionStart;
        if (region->IsSurvivedObject(offset)) {
            MAddress toAddress = region->Alloc(size);
            BaseObject* toObj = from_region_addr(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
        }
        currentPtr += size;
    }
    std::atomic_thread_fence(std::memory_order_release);

    // clear unused space which is free after compaction.
    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        TraceClear::NoteRange(cur, reclaimSize, "compact", region, region->GetLiveByteCount());
        if (!TraceClear::SkipCompactMemset()) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(cur), reclaimSize, 0, reclaimSize) == EOK,
                         "clear buffer failed");
        } else {
            VLOG(REPORT, "[GCV2][block] skip compact memset range=[%#zx,%#zx) env=MRT_GCV2_SKIP_COMPACT_MEMSET=1",
                 static_cast<size_t>(cur), static_cast<size_t>(regionLimit));
        }
    }

    region->ResetCensusBoundary();

    if (region->IsFromRegion()) {
        fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
            RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
}

void RegionManager::CompactRegion(RegionInfo* region, RegionInfo* toRegion1)
{
    MAddress regionStart = region->GetRegionStart();
    DLOG(REGION, "compact region %p@[%#zx+%zu, %#zx) type %u to region %p@%#zx:%#zx",
        region, regionStart, region->GetLiveByteCount(), region->GetRegionEnd(), region->GetRegionType(),
        toRegion1, toRegion1->GetRegionStart(), toRegion1->GetRegionAllocPtr());
    MAddress currentPtr = regionStart;
    BaseObject* currentObj = from_region_addr(currentPtr);
    CopyCollector& collector = reinterpret_cast<CopyCollector&>(Heap::GetHeap().GetCollector());
    while (true) {
        CHECK(currentPtr>=regionStart);
        size_t offset = currentPtr - regionStart;
        if (!Collector::PlausibleManagedObjectGate("CompactRegion", currentObj)) {
            break;
        }
        size_t size = currentObj->GetSize();
        if (region->IsSurvivedObject(offset)) {
            MAddress toAddress = toRegion1->Alloc(size);
            if (toAddress == 0) {
                break;
            }
            BaseObject* toObj = from_region_addr(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
        }
        currentPtr += size;
        currentObj = from_region_addr(currentPtr);
    };

    MAddress regionLimit = region->GetRegionAllocPtr();
    region->SetRegionAllocPtr(regionStart);
    while (currentPtr < regionLimit) {
        CHECK(currentPtr >= regionStart);
        size_t offset = currentPtr - regionStart;
        BaseObject* currentObj = from_region_addr(currentPtr);
        if (!Collector::PlausibleManagedObjectGate("CompactRegion", currentObj)) {
            break;
        }
        size_t size = currentObj->GetSize();
        if (region->IsSurvivedObject(offset)) {
            MAddress toAddress = region->Alloc(size);
            BaseObject* toObj = from_region_addr(toAddress);
            DLOG(FORWARD, "compact obj %p<%p>(%zu) to %p", currentObj, currentObj->GetTypeInfo(), size, toObj);
            collector.CopyObject(*currentObj, *toObj, size);
            toObj->SetStateCode(ObjectState::NORMAL);
            std::atomic_thread_fence(std::memory_order_release);
        }
        currentPtr += size;
    }

    // clear unused space which is free after compaction.
    MAddress cur = region->GetRegionAllocPtr();
    if (regionLimit > cur) {
        size_t reclaimSize = regionLimit - cur;
        TraceClear::NoteRange(cur, reclaimSize, "compact_partial", region, region->GetLiveByteCount());
        if (!TraceClear::SkipCompactMemset()) {
            CHECK_DETAIL(memset_s(reinterpret_cast<void*>(cur), reclaimSize, 0, reclaimSize) == EOK,
                         "clear buffer failed");
        } else {
            VLOG(REPORT, "[GCV2][block] skip compact_partial memset range=[%#zx,%#zx) env=MRT_GCV2_SKIP_COMPACT_MEMSET=1",
                 static_cast<size_t>(cur), static_cast<size_t>(regionLimit));
        }
    }

    region->ResetCensusBoundary();

    if (region->IsFromRegion()) {
        fromRegionList.TryDeleteRegion(region, RegionInfo::RegionType::FROM_REGION,
            RegionInfo::RegionType::THREAD_LOCAL_REGION);
    }
    tlRegionList.PrependRegion(region, RegionInfo::RegionType::THREAD_LOCAL_REGION);
}

void RegionManager::ForwardRegion(RegionInfo* region)
{
    CHECK_DETAIL(region->IsFromRegion() || region->IsLoneFromRegion() || (region->IsThreadLocalRegion() &&
        (region->IsRoutingState() || region->IsCompacted())), "region type %u", region->GetRegionType());

    DLOG(FORWARD, "try forward region %p @[0x%zx+%zu, 0x%zx) type %u, live bytes %zu",
        region, region->GetRegionStart(), region->GetRegionAllocatedSize(), region->GetRegionEnd(),
        region->GetRegionType(), region->GetLiveByteCount());

    bool youngRegion = region->IsYoungRegion();
    if (region->IsKnownEmpty()) {
        // ClearLiveInfo arms LIVE_AUTHORITY|0 before mark. MarkObject is the only path that
        // allocates the mark bitmap and raises live bytes. A region with allocated payload but
        // no mark bitmap was never entered by MarkObject — under a correct mark that means
        // nothing reachable points into it, so it is dead.
        //
        // hangfloor (0808): the young-only "fwd-empty-keep" arm (d6b77bc0) promoted every such
        // region to unmovable-from instead of CollectRegion. Under PLAIN_ROOTS arm A' that was
        // ~500 regions x 64KiB per minor with liveBytes~64 and reclaimedBytes~65KiB — young
        // thrash (10/10 HANG, minor+major alternating, promoteReplay~420k). Full GC already
        // reclaimed the same shape (1ec07b3c); young must match. B2 survivors-wiped is a mark
        // completeness defect, not a reclaim-policy defect: papering over it by keeping dead
        // young regions is what produces the hang.
        bool neverExamined = region->GetMarkBitmap() == nullptr &&
            region->GetRegionAllocPtr() > region->GetRegionStart();
        if (neverExamined) {
            // Volume control, not detail reduction. This line fired 50,282 times in a 60s run
            // (nwdiag 0808) and every one of them said the same thing, which buried the gate
            // samples that explain *why*. Print the first few of each GC cycle, then only at
            // power-of-four milestones so the final magnitude is still on the record.
            static std::atomic<size_t> emptyCollectGc{ std::numeric_limits<size_t>::max() };
            static std::atomic<size_t> emptyCollectSeq{ 0 };
            if (emptyCollectGc.load(std::memory_order_relaxed) != g_gcCount) {
                emptyCollectGc.store(g_gcCount, std::memory_order_relaxed);
                emptyCollectSeq.store(0, std::memory_order_relaxed);
            }
            size_t seq = emptyCollectSeq.fetch_add(1, std::memory_order_relaxed) + 1;
            bool milestone = (seq & (seq - 1)) == 0;   // 1,2,4,8,16,...
            if (seq <= 8 || milestone) {
                GCReason gcReason = Heap::GetHeap().GetCollector().GetGCStats().reason;
                const char* reasonName =
                    gcReason < GC_REASON_MAX ? g_gcRequests[gcReason].name : "invalid";
                // markBitmap and allocPtr>start are the two inputs behind neverExamined; print
                // them rather than only the verdict, and carry gc= so this stream can be joined
                // against [GCV2][markfloor-obj-gate] REJECT lines from the same cycle.
                VLOG(REPORT,
                     "[GCRECLAIM][fwd-empty-collect] gc=%zu seq=%zu region=%p start=%#zx alloc=%#zx "
                     "end=%#zx young=%u live=%zu bitmap=%p neverExamined=1 reason=%s(%d) — CollectRegion",
                     g_gcCount, seq, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                     region->GetRegionEnd(), static_cast<unsigned>(youngRegion),
                     region->GetLiveByteCount(), region->GetMarkBitmap(), reasonName,
                     static_cast<int>(gcReason));
            }
        }
        if (youngRegion) {
            // No live objects → no out-edges; still demote so young-count stays honest.
            region->SetYoungRegionFlag(0);
            region->SetYoungAge(0);
        }
        CollectRegion(region);
        return;
    }

    if (!RouteRegion(region)) {
        if (youngRegion) {
            // In-place promote (compacted / unrouted): scan before clearing young flag.
            region->PreserveRetainedLiveInfo();
            (void)RecordPromotedCrossGenEdges(region);
            region->SetYoungRegionFlag(0);
            region->SetYoungAge(0);
        }
        return;
    }

    int32_t rawPointerCount = region->GetRawPointerObjectCount();
    CHECK(rawPointerCount == 0);
    Collector& collector = Heap::GetHeap().GetCollector();
    RememberedSet& rememberedSet = Heap::GetHeap().GetRememberedSet();
    size_t promotedRecords = 0;
    bool forwarded = region->VisitLiveObjectsUntilFalse(
        [&collector, youngRegion, &rememberedSet, &promotedRecords](BaseObject* obj) {
            BaseObject* toObj = collector.ForwardObject(obj);
            // Remset slots must address the surviving (to-space) holder, not the from copy
            // that CollectRegion is about to reclaim.
            if (youngRegion && toObj != nullptr && toObj->HasRefField()) {
                toObj->ForEachRefField([&rememberedSet, &promotedRecords, toObj](RefField<>& field) {
                    BaseObject* target = to_object(field.GetTargetObject());
                    MAddress slot = reinterpret_cast<MAddress>(&field);
                    if (target == nullptr || !Heap::IsHeapAddress(target)) {
                        NotePromoteGapField(toObj, field, false, true);
                        IdleEdgeDiag::NotePromoteTimeTarget(slot, /*null/nonheap*/ 3, false);
                        return;
                    }
                    RegionInfo* targetRegion = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(target));
                    if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                        rememberedSet.Record(slot);
                        ++promotedRecords;
                        NotePromoteGapField(toObj, field, true, true);
                        IdleEdgeDiag::NotePromoteTimeTarget(slot, /*young*/ 1, true);
                    } else {
                        NotePromoteGapField(toObj, field, false, true);
                        IdleEdgeDiag::NotePromoteTimeTarget(slot, /*old*/ 2, false);
                    }
                });
            }
            // tipnull arm R: receipt = object FORWARDED (Copy wrote tip), not soft-keep from.
            return obj->IsForwarded();
        });

    // tipnull arm R (receipt only, no densify): FORWARDED only after every liveInfo0
    // size-walk survivor is object-FORWARDED. Prior unconditional SetRouteState(FORWARDED)
    // after VisitLive true published empty geometric to → region_FORWARDED_tip_null.
    auto allSurvivorsForwarded = [region]() -> bool {
        LiveInfo* ghost = region->GetLiveInfo0ForProbe();
        auto survivedAt = [region, ghost](size_t offset) -> bool {
            if (ghost != nullptr) {
                return ghost->IsSurvivedObject(offset);
            }
            return region->IsSurvivedObject(offset);
        };
        if (region->IsLargeRegion()) {
            if (!survivedAt(0)) {
                return true;
            }
            BaseObject* o = from_region_addr(region->GetRegionStart());
            if (!Collector::PlausibleManagedObjectGate("ForwardRegion-complete", o)) {
                return false;
            }
            return o->IsForwarded();
        }
        if (!region->IsSmallRegion()) {
            return true;
        }
        uintptr_t position = region->GetRegionStart();
        size_t offset = 0;
        uintptr_t allocPtr = region->GetRegionAllocPtr();
        while (position < allocPtr) {
            BaseObject* o = from_region_addr(position);
            if (!Collector::PlausibleManagedObjectGate("ForwardRegion-complete", o)) {
                return !survivedAt(offset);
            }
            size_t allocSize = RegionSpace::GetAllocSize(*o);
            if (survivedAt(offset) && !o->IsForwarded()) {
                return false;
            }
            position += allocSize;
            offset += allocSize;
        }
        return true;
    };

    if (!forwarded || !allSurvivorsForwarded()) {
        forwarded = region->VisitLiveObjectsUntilFalse([&collector](BaseObject* obj) {
            if (obj->IsForwarded()) {
                return true;
            }
            (void)collector.ForwardObject(obj);
            return obj->IsForwarded();
        });
    }

    CHECK_DETAIL(forwarded && allSurvivorsForwarded(),
                 "[GCV2][tipnull] ForwardRegion incomplete region=%p start=%#zx live=%zu "
                 "route=%u — refuse FORWARDED without receipts",
                 region, region->GetRegionStart(), region->GetLiveByteCount(),
                 static_cast<unsigned>(region->GetRouteState()));
    {
        static const bool probe = []() {
            const char* v = std::getenv("MRT_GCRECLAIM_PROBE");
            return v != nullptr && std::strcmp(v, "1") == 0;
        }();
        if (probe && !region->IsLargeRegion()) {
            size_t start = region->GetRegionStart();
            size_t alloc = region->GetRegionAllocPtr();
            size_t totalObjs = 0;
            size_t survivedObjs = 0;
            size_t residualValid = 0;
            uintptr_t pos = start;
            while (pos < alloc) {
                BaseObject* o = from_region_addr(pos);
                if (!o->IsValidObject()) {
                    break;
                }
                size_t sz = o->GetSize();
                if (sz == 0) {
                    break;
                }
                ++totalObjs;
                size_t off = pos - start;
                if (region->IsSurvivedObject(off)) {
                    ++survivedObjs;
                } else {
                    ++residualValid;
                }
                pos += sz;
            }
            if (residualValid > 0) {
                VLOG(REPORT,
                     "[GCRECLAIM][fwd-residual] region=%p start=%#zx alloc=%#zx live=%zu totalObjs=%zu "
                     "survived=%zu residualUnmarked=%zu young=%u BYPASS=1",
                     region, start, alloc, region->GetLiveByteCount(), totalObjs, survivedObjs, residualValid,
                     static_cast<unsigned>(youngRegion));
            }
        }
        region->SetRouteState(RegionInfo::RouteState::FORWARDED);
        if (youngRegion) {
            if (promotedRecords != 0) {
                g_promotedCrossGenEdgeCount.fetch_add(promotedRecords, std::memory_order_relaxed);
            }
            region->ResetLiveByteCount();
            region->SetYoungRegionFlag(0);
            region->SetYoungAge(0);
        }
        CollectRegion(region);
    }
}

uintptr_t RegionManager::AllocPinnedFromFreeList(size_t size)
{
    std::lock_guard<std::mutex> lock(freePinnedSlotListMutex);
    GCPhase mutatorPhase = Mutator::GetMutator()->GetMutatorPhase();
    // For preventing missing mark, do not allocate object from slot list when gc phase is post trace.
    if (mutatorPhase == GCPhase::GC_PHASE_POST_TRACE) {
        return 0;
    }
    uintptr_t allocPtr = freePinnedSlotLists.PopFront(size);
    if (allocPtr != 0) {
        RegionInfo* region = RegionInfo::GetRegionInfoAt(allocPtr);
        region->ResetCensusBoundary();
        region->PreserveRetainedLiveInfoUpTo(region->GetRegionStart());
    }
    // For making bitmap comform with live object count, do not mark object repeated.
    bool barrierClosedMarking = mutatorPhase == GCPhase::GC_PHASE_ENUM ||
        mutatorPhase == GCPhase::GC_PHASE_TRACE ||
        mutatorPhase == GCPhase::GC_PHASE_CLEAR_SATB_BUFFER;
    bool censusSafeMarking = mutatorPhase == GCPhase::GC_PHASE_PREFORWARD ||
        mutatorPhase == GCPhase::GC_PHASE_FORWARD ||
        (mutatorPhase == GCPhase::GC_PHASE_IDLE && !Heap::GetHeap().IsGcStarted());
    if (allocPtr == 0 || (!barrierClosedMarking && !censusSafeMarking)) {
        return allocPtr;
    }

    // Mark new allocated pinned object.
    BaseObject* object = from_alloc_addr(allocPtr);
    (reinterpret_cast<CopyCollector*>(&Heap::GetHeap().GetCollector()))->MarkObject(object);
    return allocPtr;
}
} // namespace MapleRuntime
