#include "Heap/Verify/ArrayWalkDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace ArrayWalkDiag {
namespace {

struct WalkFrame {
    BaseObject* holder = nullptr;
    uint64_t declared = 0;
    uint64_t visits = 0;
    uint64_t push = 0;
    uint64_t skipMarked = 0;
    uint64_t skipMarkedWater = 0;
    uint64_t skipMarkedBit = 0;
    uint64_t skipMarkedRouteNo = 0;
    uint64_t skipMarkedRecentFull = 0;
    uint64_t skipGate = 0;
    uint64_t skipGateMarkGood = 0;
    uint64_t skipGatePlausible = 0;
    uint64_t skipNull = 0;
    uint8_t componentKind = 0;
    uint8_t largeRegion = 0;
};

thread_local WalkFrame t_frame;
thread_local bool t_active = false;

std::atomic<uint64_t> g_walks{ 0 };
std::atomic<uint64_t> g_largeWalks{ 0 };
std::atomic<uint64_t> g_incomplete{ 0 };
std::atomic<uint64_t> g_declaredSum{ 0 };
std::atomic<uint64_t> g_visitsSum{ 0 };
std::atomic<uint64_t> g_pushSum{ 0 };
std::atomic<uint64_t> g_skipMarkedSum{ 0 };
std::atomic<uint64_t> g_skipMarkedWaterSum{ 0 };
std::atomic<uint64_t> g_skipMarkedBitSum{ 0 };
std::atomic<uint64_t> g_skipMarkedRouteNoSum{ 0 };
std::atomic<uint64_t> g_skipMarkedRecentFullSum{ 0 };
std::atomic<uint64_t> g_skipGateSum{ 0 };
std::atomic<uint64_t> g_skipGateMarkGoodSum{ 0 };
std::atomic<uint64_t> g_skipGatePlausibleSum{ 0 };
std::atomic<uint64_t> g_skipNullSum{ 0 };
std::atomic<uint64_t> g_maxDeclared{ 0 };
std::atomic<uintptr_t> g_lastLargeHolder{ 0 };
std::atomic<uint64_t> g_lastLargeDeclared{ 0 };
std::atomic<uint64_t> g_lastLargeVisits{ 0 };
std::atomic<uintptr_t> g_slotWatchAddress{ 0 };
std::atomic<uintptr_t> g_slotWatchHolder{ 0 };
std::atomic<uint64_t> g_slotWatchDeclared{ 0 };
std::atomic<uint32_t> g_slotWatchVisitedGc{ std::numeric_limits<uint32_t>::max() };
std::atomic<uintptr_t> g_slotWatchTarget{ 0 };
std::atomic<uint32_t> g_slotWatchMarkedGc{ std::numeric_limits<uint32_t>::max() };
std::atomic<uintptr_t> g_slotWatchMarkedCarrier{ 0 };
std::atomic<uintptr_t> g_slotWatchMarkedBitmap{ 0 };
std::atomic<uint64_t> g_slotWatchMarkedEpoch{ 0 };
std::atomic<uint64_t> g_slotWatchMarkedWord8{ 0 };
std::atomic<uintptr_t> g_slotWatchMarkedRegion{ 0 };
std::atomic<size_t> g_slotWatchMarkedOffset{ 0 };

uint64_t WatchObjectId()
{
    static const uint64_t id = []() {
        const char* value = std::getenv("MRT_GCV2_WATCH_ID");
        return value == nullptr || value[0] == '\0' ? 0 : std::strtoull(value, nullptr, 10);
    }();
    return id;
}

const char* RegionKindName(RegionInfo::RegionType type)
{
    using RT = RegionInfo::RegionType;
    switch (type) {
        case RT::FREE_REGION: return "FREE";
        case RT::THREAD_LOCAL_REGION: return "THREAD_LOCAL";
        case RT::RECENT_FULL_REGION: return "RECENT_FULL";
        case RT::FROM_REGION: return "FROM";
        case RT::LONE_FROM_REGION: return "LONE_FROM";
        case RT::UNMOVABLE_FROM_REGION: return "UNMOVABLE_FROM";
        case RT::TO_REGION: return "TO";
        case RT::FULL_PINNED_REGION: return "FULL_PINNED";
        case RT::RECENT_PINNED_REGION: return "RECENT_PINNED";
        case RT::RAW_POINTER_PINNED_REGION: return "RAW_POINTER_PINNED";
        case RT::TL_RAW_POINTER_REGION: return "TL_RAW_POINTER";
        case RT::TL_LARGE_RAW_POINTER_REGION: return "TL_LARGE_RAW_POINTER";
        case RT::LARGE_REGION: return "LARGE";
        case RT::RECENT_LARGE_REGION: return "RECENT_LARGE";
        case RT::GARBAGE_REGION: return "GARBAGE";
    }
    return "UNKNOWN";
}

} // namespace

bool Enabled()
{
    static const bool on = DiagGate::LegacyOrToken("MRT_GCV2_ARRAYWALK", "arraywalk");
    return on;
}

bool SlotWatchEnabled()
{
    static const bool on = []() {
        const char* value = std::getenv("MRT_GCV2_SLOTWATCH");
        if (value == nullptr || value[0] == '\0') {
            return false;
        }
        char* end = nullptr;
        (void)std::strtoull(value, &end, 10);
        return end != value && *end == '\0';
    }();
    return on;
}

uint64_t SlotWatchIndex()
{
    static const uint64_t index = []() {
        const char* value = std::getenv("MRT_GCV2_SLOTWATCH");
        return value == nullptr ? 0 : std::strtoull(value, nullptr, 10);
    }();
    return index;
}

void Begin(BaseObject* holder, uint64_t declared, TypeInfo* component, bool largeRegion)
{
    if (!Enabled()) {
        return;
    }
    t_frame = WalkFrame{};
    t_frame.holder = holder;
    t_frame.declared = declared;
    t_frame.largeRegion = largeRegion ? 1 : 0;
    if (component != nullptr) {
        if (component->IsStructType()) {
            t_frame.componentKind = 1;
        } else if (component->IsObjectType() || component->IsArrayType() || component->IsInterface()) {
            t_frame.componentKind = 2;
        } else {
            t_frame.componentKind = 3;
        }
    }
    t_active = true;
}

void NoteVisit()
{
    if (t_active) {
        ++t_frame.visits;
    }
}

void NotePush()
{
    if (t_active) {
        ++t_frame.push;
    }
}

void NoteSkipMarked()
{
    if (t_active) {
        ++t_frame.skipMarked;
    }
}

void NoteSkipMarkedTarget(BaseObject* target)
{
    if (!t_active || target == nullptr) {
        return;
    }
    ++t_frame.skipMarked;
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (region == nullptr) {
        return;
    }
    const size_t off = region->GetAddressOffset(reinterpret_cast<MAddress>(target));
    const bool water = region->AllocatedAfterMarkStart(off);
    if (water) {
        ++t_frame.skipMarkedWater;
    } else {
        ++t_frame.skipMarkedBit;
    }
    if (!region->IsRouteSurvivedObject(off)) {
        ++t_frame.skipMarkedRouteNo;
    }
    if (region->GetRegionType() == RegionInfo::RegionType::RECENT_FULL_REGION) {
        ++t_frame.skipMarkedRecentFull;
    }
}

void NoteSkipGate()
{
    if (t_active) {
        ++t_frame.skipGate;
    }
}

void NoteSkipGateMarkGood()
{
    if (t_active) {
        ++t_frame.skipGate;
        ++t_frame.skipGateMarkGood;
    }
}

void NoteSkipGatePlausible()
{
    if (t_active) {
        ++t_frame.skipGate;
        ++t_frame.skipGatePlausible;
    }
}

void NoteSkipNull()
{
    if (t_active) {
        ++t_frame.skipNull;
    }
}

void ReportSlotWatch(BaseObject* holder, uint64_t declared, uint64_t index, uintptr_t slotAddress,
                     uint64_t slotValue, BaseObject* target, bool walkVisited, bool pushed,
                     const char* skipReason, int isMarkedObject)
{
    if (!SlotWatchEnabled()) {
        return;
    }
    const uint32_t gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    const GCPhase phase = Heap::GetHeap().GetGCPhase();
    if (walkVisited) {
        g_slotWatchAddress.store(slotAddress, std::memory_order_relaxed);
        g_slotWatchHolder.store(reinterpret_cast<uintptr_t>(holder), std::memory_order_relaxed);
        g_slotWatchDeclared.store(declared, std::memory_order_relaxed);
        g_slotWatchVisitedGc.store(gc, std::memory_order_release);
    }
    RegionInfo* region = target == nullptr
        ? nullptr : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    bool bitmapBitActual = false;
    bool bitmapPresent = false;
    bool allocatedAfterMarkStart = false;
    uint64_t bitmapEpoch = 0;
    uint64_t markEpoch = 0;
    uint64_t regionLife = 0;
    unsigned regionSeq = 0;
    unsigned regionKind = 0;
    const char* regionKindName = "NONE";
    if (region != nullptr) {
        const size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(target));
        markEpoch = region->GetMarkSnapshotEpoch<Generation::Old>();
        region->ReadMarkBitActual<Generation::Old>(offset, bitmapBitActual, bitmapPresent, bitmapEpoch);
        allocatedAfterMarkStart = region->AllocatedAfterMarkStart(offset);
        regionLife = region->GetRegionLifeId();
        regionSeq = static_cast<unsigned>(region->GetRegionLifeSeq());
        regionKind = static_cast<unsigned>(region->GetRegionType());
        regionKindName = RegionKindName(region->GetRegionType());
        if (walkVisited) {
            LiveInfo* carrier = region->GetLiveInfo();
            uintptr_t bitmapAddress = 0;
            region->ReadMarkFaceActual<Generation::Old>(carrier, offset, bitmapAddress,
                                                        bitmapBitActual, bitmapPresent, bitmapEpoch);
            g_slotWatchTarget.store(reinterpret_cast<uintptr_t>(target), std::memory_order_relaxed);
            g_slotWatchMarkedCarrier.store(reinterpret_cast<uintptr_t>(carrier), std::memory_order_relaxed);
            g_slotWatchMarkedBitmap.store(bitmapAddress, std::memory_order_relaxed);
            g_slotWatchMarkedEpoch.store(bitmapEpoch, std::memory_order_relaxed);
            g_slotWatchMarkedRegion.store(reinterpret_cast<uintptr_t>(region), std::memory_order_relaxed);
            g_slotWatchMarkedOffset.store(offset, std::memory_order_relaxed);
            g_slotWatchMarkedWord8.store(
                __atomic_load_n(reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(target) + 8),
                                __ATOMIC_RELAXED),
                std::memory_order_relaxed);
            g_slotWatchMarkedGc.store(gc, std::memory_order_release);
        }
    }
    std::fprintf(stderr,
                 "[GCV2][slotwatch] gc=%u phase=%s phaseId=%u holder=%p declared=%llu index=%llu "
                 "slotValue=%#llx target=%p walkVisited=%u pushed=%u skipReason=%s isMarkedObject=%d "
                 "bitmapBitActual=%u bitmapPresent=%u regionKind=%s regionKindId=%u regionSeq=%u regionLife=%llu "
                 "markEpoch=%llu bitmapEpoch=%llu allocatedAfterMarkStart=%u\n",
                 gc, Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase), static_cast<void*>(holder),
                 static_cast<unsigned long long>(declared), static_cast<unsigned long long>(index),
                 static_cast<unsigned long long>(slotValue), static_cast<void*>(target), walkVisited ? 1u : 0u,
                 pushed ? 1u : 0u,
                 skipReason == nullptr ? "other" : skipReason, isMarkedObject, bitmapBitActual ? 1u : 0u,
                 bitmapPresent ? 1u : 0u, regionKindName, regionKind, regionSeq,
                 static_cast<unsigned long long>(regionLife),
                 static_cast<unsigned long long>(markEpoch), static_cast<unsigned long long>(bitmapEpoch),
                 allocatedAfterMarkStart ? 1u : 0u);
    std::fflush(stderr);
}

void RefreshSlotWatchTarget(const char* point)
{
    if (!SlotWatchEnabled()) {
        return;
    }
    const uintptr_t slotAddress = g_slotWatchAddress.load(std::memory_order_acquire);
    if (slotAddress == 0) {
        return;
    }
    const uint64_t slotValue = __atomic_load_n(reinterpret_cast<const uint64_t*>(slotAddress), __ATOMIC_ACQUIRE);
    HeapSlot<> snapshot(to_zpointer(slotValue));
    BaseObject* target = to_object(snapshot.GetTargetObject());
    const uintptr_t prior = g_slotWatchTarget.exchange(reinterpret_cast<uintptr_t>(target),
                                                        std::memory_order_acq_rel);
    std::fprintf(stderr,
                 "[GCV2][twobitmaps][refresh] gc=%u phase=%s point=%s slot=%p slotValue=%#llx "
                 "priorTarget=%p target=%p markedGc=%u markedWord8=%#llx "
                 "markedCarrier=%p markedBitmap=%p markedEpoch=%llu\n",
                 static_cast<unsigned>(g_gcCount.load(std::memory_order_relaxed)),
                 Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), point == nullptr ? "?" : point,
                 reinterpret_cast<void*>(slotAddress), static_cast<unsigned long long>(slotValue),
                 reinterpret_cast<void*>(prior), static_cast<void*>(target),
                 g_slotWatchMarkedGc.load(std::memory_order_acquire),
                 static_cast<unsigned long long>(g_slotWatchMarkedWord8.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(g_slotWatchMarkedCarrier.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(g_slotWatchMarkedBitmap.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slotWatchMarkedEpoch.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

void ReportCompactDecision(RegionInfo* region, BaseObject* object, size_t offset, size_t objectSize,
                           bool survived)
{
    if (!SlotWatchEnabled() || region == nullptr || object == nullptr) {
        return;
    }
    const uint64_t payloadWord = objectSize >= 16
        ? __atomic_load_n(reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(object) + 8),
                          __ATOMIC_RELAXED)
        : 0;
    const bool addressMatch = g_slotWatchTarget.load(std::memory_order_acquire) ==
        reinterpret_cast<uintptr_t>(object);
    const uint64_t markedWord8 = g_slotWatchMarkedWord8.load(std::memory_order_acquire);
    const bool wordMatch = objectSize >= 16 && markedWord8 != 0 && payloadWord == markedWord8;
    const uint64_t watchId = WatchObjectId();
    const bool idMatch = objectSize >= 16 && watchId != 0 && payloadWord == watchId;
    const bool coordinateMatch = g_slotWatchMarkedRegion.load(std::memory_order_acquire) ==
            reinterpret_cast<uintptr_t>(region) &&
        g_slotWatchMarkedOffset.load(std::memory_order_relaxed) == offset;
    if (!addressMatch && !wordMatch && !idMatch && !coordinateMatch) {
        return;
    }
    if (!addressMatch) {
        g_slotWatchTarget.store(reinterpret_cast<uintptr_t>(object), std::memory_order_release);
    }
    LiveInfo* current = region->GetLiveInfo();
    LiveInfo* ghost = region->GetLiveInfo0ForProbe();
    uintptr_t currentYoungBitmap = 0;
    uintptr_t currentOldBitmap = 0;
    uintptr_t ghostYoungBitmap = 0;
    uintptr_t ghostOldBitmap = 0;
    bool currentYoungBit = false;
    bool currentOldBit = false;
    bool ghostYoungBit = false;
    bool ghostOldBit = false;
    bool present = false;
    uint64_t currentYoungEpoch = 0;
    uint64_t currentOldEpoch = 0;
    uint64_t ghostYoungEpoch = 0;
    uint64_t ghostOldEpoch = 0;
    region->ReadMarkFaceActual<Generation::Young>(current, offset, currentYoungBitmap,
                                                  currentYoungBit, present, currentYoungEpoch);
    region->ReadMarkFaceActual<Generation::Old>(current, offset, currentOldBitmap,
                                                currentOldBit, present, currentOldEpoch);
    region->ReadMarkFaceActual<Generation::Young>(ghost, offset, ghostYoungBitmap,
                                                  ghostYoungBit, present, ghostYoungEpoch);
    region->ReadMarkFaceActual<Generation::Old>(ghost, offset, ghostOldBitmap,
                                                ghostOldBit, present, ghostOldEpoch);
    const Generation routeGeneration = region->GetRouteMarkGeneration();
    std::fprintf(stderr,
                 "[GCV2][twobitmaps][compact] gc=%u phase=%s obj=%p word8=%#llx size=%zu region=%p "
                 "kind=%s kindId=%u youngRegion=%u life=%llu seq=%u offset=%zu survived=%u afterMark=%u "
                 "routeGen=%s routeEpoch=%llu current=%p ghost=%p "
                 "currentYoung=[bm=%p epoch=%llu bit=%u] currentOld=[bm=%p epoch=%llu bit=%u] "
                 "ghostYoung=[bm=%p epoch=%llu bit=%u] ghostOld=[bm=%p epoch=%llu bit=%u] "
                 "markedGc=%u markedWord8=%#llx match=%s markedCarrier=%p markedBitmap=%p markedEpoch=%llu "
                 "markedRegion=%p markedOffset=%zu sameCarrier=%u sameBitmap=%u\n",
                 static_cast<unsigned>(g_gcCount.load(std::memory_order_relaxed)),
                 Collector::GetGCPhaseName(Heap::GetHeap().GetGCPhase()), static_cast<void*>(object),
                 static_cast<unsigned long long>(payloadWord), objectSize, static_cast<void*>(region),
                 RegionKindName(region->GetRegionType()), static_cast<unsigned>(region->GetRegionType()),
                 region->IsYoungRegion() ? 1u : 0u,
                 static_cast<unsigned long long>(region->GetRegionLifeId()),
                 static_cast<unsigned>(region->GetRegionLifeSeq()), offset, survived ? 1u : 0u,
                 region->AllocatedAfterMarkStart(offset) ? 1u : 0u,
                 routeGeneration == Generation::Young ? "Young" : "Old",
                 static_cast<unsigned long long>(region->GetRouteMarkSnapshotEpoch()),
                 static_cast<void*>(current), static_cast<void*>(ghost),
                 reinterpret_cast<void*>(currentYoungBitmap), static_cast<unsigned long long>(currentYoungEpoch),
                 currentYoungBit ? 1u : 0u,
                 reinterpret_cast<void*>(currentOldBitmap), static_cast<unsigned long long>(currentOldEpoch),
                 currentOldBit ? 1u : 0u,
                 reinterpret_cast<void*>(ghostYoungBitmap), static_cast<unsigned long long>(ghostYoungEpoch),
                 ghostYoungBit ? 1u : 0u,
                 reinterpret_cast<void*>(ghostOldBitmap), static_cast<unsigned long long>(ghostOldEpoch),
                 ghostOldBit ? 1u : 0u,
                 g_slotWatchMarkedGc.load(std::memory_order_acquire),
                 static_cast<unsigned long long>(markedWord8),
                 addressMatch ? "address" : (wordMatch ? "word8" : (idMatch ? "watch-id" : "region-offset")),
                 reinterpret_cast<void*>(g_slotWatchMarkedCarrier.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(g_slotWatchMarkedBitmap.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_slotWatchMarkedEpoch.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(g_slotWatchMarkedRegion.load(std::memory_order_relaxed)),
                 g_slotWatchMarkedOffset.load(std::memory_order_relaxed),
                 reinterpret_cast<uintptr_t>(ghost) == g_slotWatchMarkedCarrier.load(std::memory_order_relaxed)
                     ? 1u : 0u,
                 (routeGeneration == Generation::Young ? ghostYoungBitmap : ghostOldBitmap) ==
                         g_slotWatchMarkedBitmap.load(std::memory_order_relaxed)
                     ? 1u : 0u);
    std::fflush(stderr);
}

void NoteCompactMove(BaseObject* from, BaseObject* to)
{
    if (!SlotWatchEnabled() || from == nullptr || to == nullptr) {
        return;
    }
    uintptr_t expected = reinterpret_cast<uintptr_t>(from);
    if (g_slotWatchTarget.compare_exchange_strong(expected, reinterpret_cast<uintptr_t>(to),
                                                  std::memory_order_acq_rel)) {
        std::fprintf(stderr, "[GCV2][twobitmaps][move] gc=%u from=%p to=%p\n",
                     static_cast<unsigned>(g_gcCount.load(std::memory_order_relaxed)),
                     static_cast<void*>(from), static_cast<void*>(to));
        std::fflush(stderr);
    }
}

void ReportSlotWatchCycleEnd()
{
    if (!SlotWatchEnabled()) {
        return;
    }
    const uint32_t gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    if (g_slotWatchVisitedGc.load(std::memory_order_acquire) == gc) {
        return;
    }
    const uintptr_t slotAddress = g_slotWatchAddress.load(std::memory_order_relaxed);
    if (slotAddress == 0) {
        return;
    }
    const uint64_t slotValue = __atomic_load_n(reinterpret_cast<const uint64_t*>(slotAddress), __ATOMIC_ACQUIRE);
    HeapSlot<> snapshot(to_zpointer(slotValue));
    BaseObject* target = to_object(snapshot.GetTargetObject());
    ReportSlotWatch(from_native_ref(g_slotWatchHolder.load(std::memory_order_relaxed)),
                    g_slotWatchDeclared.load(std::memory_order_relaxed), SlotWatchIndex(), slotAddress,
                    slotValue, target, false, false, "not-walked", -1);
}

void End()
{
    if (!t_active) {
        return;
    }
    t_active = false;
    g_walks.fetch_add(1, std::memory_order_relaxed);
    g_declaredSum.fetch_add(t_frame.declared, std::memory_order_relaxed);
    g_visitsSum.fetch_add(t_frame.visits, std::memory_order_relaxed);
    g_pushSum.fetch_add(t_frame.push, std::memory_order_relaxed);
    g_skipMarkedSum.fetch_add(t_frame.skipMarked, std::memory_order_relaxed);
    g_skipMarkedWaterSum.fetch_add(t_frame.skipMarkedWater, std::memory_order_relaxed);
    g_skipMarkedBitSum.fetch_add(t_frame.skipMarkedBit, std::memory_order_relaxed);
    g_skipMarkedRouteNoSum.fetch_add(t_frame.skipMarkedRouteNo, std::memory_order_relaxed);
    g_skipMarkedRecentFullSum.fetch_add(t_frame.skipMarkedRecentFull, std::memory_order_relaxed);
    g_skipGateSum.fetch_add(t_frame.skipGate, std::memory_order_relaxed);
    g_skipGateMarkGoodSum.fetch_add(t_frame.skipGateMarkGood, std::memory_order_relaxed);
    g_skipGatePlausibleSum.fetch_add(t_frame.skipGatePlausible, std::memory_order_relaxed);
    g_skipNullSum.fetch_add(t_frame.skipNull, std::memory_order_relaxed);
    uint64_t prevMax = g_maxDeclared.load(std::memory_order_relaxed);
    while (t_frame.declared > prevMax &&
           !g_maxDeclared.compare_exchange_weak(prevMax, t_frame.declared, std::memory_order_relaxed)) {
    }
    const bool incomplete = t_frame.componentKind == 2 && t_frame.visits != t_frame.declared;
    if (incomplete) {
        g_incomplete.fetch_add(1, std::memory_order_relaxed);
    }
    if (t_frame.largeRegion != 0) {
        g_largeWalks.fetch_add(1, std::memory_order_relaxed);
        g_lastLargeHolder.store(reinterpret_cast<uintptr_t>(t_frame.holder), std::memory_order_relaxed);
        g_lastLargeDeclared.store(t_frame.declared, std::memory_order_relaxed);
        g_lastLargeVisits.store(t_frame.visits, std::memory_order_relaxed);
    }
    if (incomplete || (t_frame.largeRegion != 0 && t_frame.declared >= 100000)) {
        std::fprintf(stderr,
                     "[GCV2][arraywalk] holder=%p large=%u kind=%u declared=%lu visits=%lu "
                     "push=%lu skipMarked=%lu water=%lu bit=%lu routeNo=%lu recentFull=%lu "
                     "skipGate=%lu gateMarkGood=%lu gatePlausible=%lu skipNull=%lu incomplete=%d gc=%u\n",
                     static_cast<void*>(t_frame.holder), static_cast<unsigned>(t_frame.largeRegion),
                     static_cast<unsigned>(t_frame.componentKind),
                     static_cast<unsigned long>(t_frame.declared),
                     static_cast<unsigned long>(t_frame.visits),
                     static_cast<unsigned long>(t_frame.push),
                     static_cast<unsigned long>(t_frame.skipMarked),
                     static_cast<unsigned long>(t_frame.skipMarkedWater),
                     static_cast<unsigned long>(t_frame.skipMarkedBit),
                     static_cast<unsigned long>(t_frame.skipMarkedRouteNo),
                     static_cast<unsigned long>(t_frame.skipMarkedRecentFull),
                     static_cast<unsigned long>(t_frame.skipGate),
                     static_cast<unsigned long>(t_frame.skipGateMarkGood),
                     static_cast<unsigned long>(t_frame.skipGatePlausible),
                     static_cast<unsigned long>(t_frame.skipNull), incomplete ? 1 : 0,
                     static_cast<unsigned>(g_gcCount.load(std::memory_order_relaxed)));
        std::fflush(stderr);
    }
}

void Report(const char* point)
{
    if (!Enabled()) {
        return;
    }
    std::fprintf(stderr,
                 "[GCV2][arraywalk] point=%s walks=%lu large=%lu incomplete=%lu declaredSum=%lu "
                 "visitsSum=%lu push=%lu skipMarked=%lu water=%lu bit=%lu routeNo=%lu recentFull=%lu "
                 "skipGate=%lu gateMarkGood=%lu gatePlausible=%lu skipNull=%lu maxDeclared=%lu "
                 "lastLargeHolder=%p lastLargeDeclared=%lu lastLargeVisits=%lu\n",
                 point != nullptr ? point : "?",
                 static_cast<unsigned long>(g_walks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_largeWalks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_incomplete.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_declaredSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_visitsSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_pushSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipMarkedSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipMarkedWaterSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipMarkedBitSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipMarkedRouteNoSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipMarkedRecentFullSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipGateSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipGateMarkGoodSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipGatePlausibleSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipNullSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_maxDeclared.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(g_lastLargeHolder.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_lastLargeDeclared.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_lastLargeVisits.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

} // namespace ArrayWalkDiag
} // namespace MapleRuntime
