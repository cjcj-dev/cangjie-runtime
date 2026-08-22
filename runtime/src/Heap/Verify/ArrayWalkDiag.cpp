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
