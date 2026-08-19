#include "Heap/Verify/GarbRegionDiag.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Collector/LiveInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace GarbRegionDiag {
// Compile-time gate (campaign cut live MRT_GCV2 env reads to 3). Census is a full
// heap walk — leave kCensus off in product; forensic SO flips it on.
constexpr bool kGarbRegion = true;
constexpr bool kCensus = true;

constexpr size_t kTabCap = 1u << 16;
constexpr size_t kSampleCap = 64;

struct SlotRow {
    RegionInfo* region{ nullptr };
    std::atomic<size_t> slots{ 0 };
    std::atomic<size_t> markedHolders{ 0 };
    std::atomic<size_t> unmarkedHolders{ 0 };
    std::atomic<size_t> youngHolders{ 0 };
    std::atomic<size_t> oldHolders{ 0 };
};

SlotRow g_tab[kTabCap];
std::atomic<size_t> g_tabUsed{ 0 };
std::atomic<size_t> g_tabSat{ 0 };
std::atomic<size_t> g_censusN{ 0 };
std::atomic<size_t> g_censusHolders{ 0 };
std::atomic<size_t> g_censusSlots{ 0 };
std::atomic<size_t> g_enter{ 0 };
std::atomic<size_t> g_enterKnownEmpty{ 0 };
std::atomic<size_t> g_enterJoinHit{ 0 };
std::atomic<size_t> g_enterJoinMiss{ 0 };
std::atomic<size_t> g_enterLiveGt0{ 0 };
std::atomic<size_t> g_enterLiveMarkedGt0{ 0 };
std::atomic<size_t> g_clsNoAuth{ 0 };
std::atomic<size_t> g_clsLiveinfoNull{ 0 };
std::atomic<size_t> g_clsEpochStale{ 0 };
std::atomic<size_t> g_clsLargeUnmarked{ 0 };
std::atomic<size_t> g_clsNotEmpty{ 0 };
std::atomic<size_t> g_clsNeverExamined{ 0 };
std::atomic<size_t> g_f3Hits{ 0 };
std::atomic<size_t> g_f3Garbage{ 0 };
std::atomic<size_t> g_f3Free{ 0 };
std::atomic<size_t> g_f3JoinHit{ 0 };
std::atomic<size_t> g_f3LiveGt0{ 0 };
std::atomic<size_t> g_sampleLogged{ 0 };
std::atomic<size_t> g_f3SampleLogged{ 0 };
std::atomic<bool> g_atexit{ false };
char g_lastWhere[32] = "none";

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

size_t TabIdx(RegionInfo* region)
{
    return (reinterpret_cast<uintptr_t>(region) >> 4) & (kTabCap - 1);
}

SlotRow* FindRow(RegionInfo* region)
{
    if (region == nullptr) {
        return nullptr;
    }
    size_t idx = TabIdx(region);
    for (size_t probe = 0; probe < 64; ++probe) {
        size_t i = (idx + probe) & (kTabCap - 1);
        RegionInfo* cur = __atomic_load_n(&g_tab[i].region, std::memory_order_acquire);
        if (cur == region) {
            return &g_tab[i];
        }
        if (cur == nullptr) {
            return nullptr;
        }
    }
    return nullptr;
}

SlotRow* FindOrInsert(RegionInfo* region)
{
    if (region == nullptr) {
        return nullptr;
    }
    size_t idx = TabIdx(region);
    for (size_t probe = 0; probe < 64; ++probe) {
        size_t i = (idx + probe) & (kTabCap - 1);
        RegionInfo* cur = __atomic_load_n(&g_tab[i].region, std::memory_order_acquire);
        if (cur == region) {
            return &g_tab[i];
        }
        if (cur == nullptr) {
            RegionInfo* expected = nullptr;
            if (__atomic_compare_exchange_n(&g_tab[i].region, &expected, region, false, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                g_tabUsed.fetch_add(1, std::memory_order_relaxed);
                return &g_tab[i];
            }
            if (expected == region) {
                return &g_tab[i];
            }
        }
    }
    g_tabSat.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void ClearTab()
{
    for (size_t i = 0; i < kTabCap; ++i) {
        __atomic_store_n(&g_tab[i].region, static_cast<RegionInfo*>(nullptr), std::memory_order_relaxed);
        g_tab[i].slots.store(0, std::memory_order_relaxed);
        g_tab[i].markedHolders.store(0, std::memory_order_relaxed);
        g_tab[i].unmarkedHolders.store(0, std::memory_order_relaxed);
        g_tab[i].youngHolders.store(0, std::memory_order_relaxed);
        g_tab[i].oldHolders.store(0, std::memory_order_relaxed);
    }
    g_tabUsed.store(0, std::memory_order_relaxed);
    g_tabSat.store(0, std::memory_order_relaxed);
}

bool HolderMarked(RegionInfo* holderRegion, BaseObject* holder)
{
    if (holderRegion->IsYoungRegion()) {
        return holderRegion->IsMarkedObject(holderRegion->GetMarkView<Generation::Young>(), holder);
    }
    return holderRegion->IsMarkedObject(holderRegion->GetMarkView<Generation::Old>(), holder);
}

const char* EmptyClass(bool auth, bool large, bool liveinfoNull, bool epochStale, bool knownEmpty)
{
    if (!knownEmpty) {
        return "not_empty";
    }
    if (!auth) {
        return "no_auth";
    }
    if (large) {
        return "large_unmarked";
    }
    if (liveinfoNull) {
        return "liveinfo_null";
    }
    if (epochStale) {
        return "epoch_stale";
    }
    return "empty_other";
}

bool Enabled() { return kGarbRegion; }

void CensusBeforeForward(const char* where)
{
    if (!kGarbRegion || !kCensus) {
        return;
    }
    EnsureAtexit();
    ClearTab();
    size_t n = g_censusN.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t holders = 0;
    size_t slots = 0;
    if (where != nullptr) {
        std::snprintf(g_lastWhere, sizeof(g_lastWhere), "%s", where);
    }
    Heap::GetHeap().ForEachObj(
        [&holders, &slots](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* holderRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (holderRegion == nullptr || holderRegion->IsGarbageRegion() || holderRegion->IsFreeRegion()) {
                return;
            }
            if (!Collector::PlausibleManagedObjectGate("garbregion-census", holder)) {
                return;
            }
            ++holders;
            const bool holderMarked = HolderMarked(holderRegion, holder);
            const bool holderYoung = holderRegion->IsYoungRegion();
            holder->ForEachRefField([holderMarked, holderYoung, &slots](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
                if (targetRegion == nullptr) {
                    return;
                }
                SlotRow* row = FindOrInsert(targetRegion);
                if (row == nullptr) {
                    return;
                }
                row->slots.fetch_add(1, std::memory_order_relaxed);
                if (holderMarked) {
                    row->markedHolders.fetch_add(1, std::memory_order_relaxed);
                } else {
                    row->unmarkedHolders.fetch_add(1, std::memory_order_relaxed);
                }
                if (holderYoung) {
                    row->youngHolders.fetch_add(1, std::memory_order_relaxed);
                } else {
                    row->oldHolders.fetch_add(1, std::memory_order_relaxed);
                }
                ++slots;
            });
        },
        false);
    g_censusHolders.store(holders, std::memory_order_relaxed);
    g_censusSlots.store(slots, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[GCV2][garbregion][census] n=%zu where=%s holders=%zu slots=%zu regionsHit=%zu sat=%zu gc=%zu\n",
                 n, g_lastWhere, holders, slots, g_tabUsed.load(std::memory_order_relaxed),
                 g_tabSat.load(std::memory_order_relaxed), g_gcCount.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

void NoteCollectEnter(RegionInfo* region)
{
    if (!kGarbRegion) {
        return;
    }
    EnsureAtexit();
    size_t n = g_enter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        LOG(RTLOG_ERROR, "[GCV2][garbregion] armed first n=1 census=%u", static_cast<unsigned>(kCensus));
    }
    if (region == nullptr) {
        return;
    }
    const bool knownEmpty = region->IsRouteKnownEmpty();
    const bool auth = region->IsLiveCountAuthoritative();
    const uint64_t liveBytes = region->GetLiveByteCount();
    const bool young = region->IsYoungRegion();
    const bool large = region->IsLargeRegion();
    LiveInfo* liveInfo = region->GetLiveInfo();
    LiveInfo* routeFace = region->GetLiveInfo0ForProbe();
    if (routeFace == nullptr) {
        routeFace = liveInfo;
    }
    const bool liveinfoNull = liveInfo == nullptr;
    const uint64_t snapEp = region->GetSnapshotEpoch();
    uint64_t faceEp = 0;
    bool epochStale = false;
    if (routeFace != nullptr) {
        faceEp = region->GetRouteMarkEpoch(routeFace);
        epochStale = (faceEp != snapEp);
    }
    RegionBitmap* mb = region->GetRouteMarkBitmap(routeFace);
    const bool neverExamined = knownEmpty && mb == nullptr && region->GetRegionAllocPtr() > region->GetRegionStart();
    if (knownEmpty) {
        g_enterKnownEmpty.fetch_add(1, std::memory_order_relaxed);
    }
    if (!auth) {
        g_clsNoAuth.fetch_add(1, std::memory_order_relaxed);
    } else if (large && knownEmpty) {
        g_clsLargeUnmarked.fetch_add(1, std::memory_order_relaxed);
    } else if (liveinfoNull && knownEmpty) {
        g_clsLiveinfoNull.fetch_add(1, std::memory_order_relaxed);
    } else if (epochStale && knownEmpty) {
        g_clsEpochStale.fetch_add(1, std::memory_order_relaxed);
    } else if (!knownEmpty) {
        g_clsNotEmpty.fetch_add(1, std::memory_order_relaxed);
    }
    if (neverExamined) {
        g_clsNeverExamined.fetch_add(1, std::memory_order_relaxed);
    }

    SlotRow* row = FindRow(region);
    size_t slots = 0;
    size_t markedH = 0;
    size_t unmarkedH = 0;
    size_t youngH = 0;
    size_t oldH = 0;
    if (row != nullptr) {
        g_enterJoinHit.fetch_add(1, std::memory_order_relaxed);
        slots = row->slots.load(std::memory_order_relaxed);
        markedH = row->markedHolders.load(std::memory_order_relaxed);
        unmarkedH = row->unmarkedHolders.load(std::memory_order_relaxed);
        youngH = row->youngHolders.load(std::memory_order_relaxed);
        oldH = row->oldHolders.load(std::memory_order_relaxed);
    } else {
        g_enterJoinMiss.fetch_add(1, std::memory_order_relaxed);
    }
    if (slots > 0) {
        g_enterLiveGt0.fetch_add(1, std::memory_order_relaxed);
        if (markedH > 0) {
            g_enterLiveMarkedGt0.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const char* cls = EmptyClass(auth, large, liveinfoNull, epochStale, knownEmpty);
    size_t slog = g_sampleLogged.fetch_add(1, std::memory_order_relaxed);
    const bool loud = (slots > 0) || (slog < kSampleCap);
    if (loud) {
        std::fprintf(stderr,
                     "[GCV2][garbregion][collect] n=%zu region=%p start=%#zx alloc=%#zx type=%u route=%u "
                     "routeGen=%u young=%u large=%u live=%llu auth=%u knownEmpty=%u neverExamined=%u "
                     "liveinfo=%p faceEp=%llu snapEp=%llu bitmap=%p cls=%s "
                     "slots=%zu markedH=%zu unmarkedH=%zu youngH=%zu oldH=%zu join=%u gc=%zu where=%s\n",
                     n, region, region->GetRegionStart(), region->GetRegionAllocPtr(),
                     static_cast<unsigned>(region->GetRegionType()),
                     static_cast<unsigned>(region->GetRouteState()),
                     static_cast<unsigned>(region->GetRouteMarkGeneration()),
                     static_cast<unsigned>(young), static_cast<unsigned>(large),
                     static_cast<unsigned long long>(liveBytes), static_cast<unsigned>(auth),
                     static_cast<unsigned>(knownEmpty), static_cast<unsigned>(neverExamined), liveInfo,
                     static_cast<unsigned long long>(faceEp), static_cast<unsigned long long>(snapEp), mb, cls,
                     slots, markedH, unmarkedH, youngH, oldH, static_cast<unsigned>(row != nullptr),
                     g_gcCount.load(std::memory_order_relaxed), g_lastWhere);
        std::fflush(stderr);
    }
}

void NoteF3Join(RegionInfo* latestRegion, BaseObject* latest, const char* reason)
{
    if (!kGarbRegion) {
        return;
    }
    EnsureAtexit();
    size_t n = g_f3Hits.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool isFree = reason != nullptr && std::strcmp(reason, "region_free") == 0;
    const bool isGarbage = reason != nullptr && std::strcmp(reason, "region_garbage") == 0;
    if (isFree) {
        g_f3Free.fetch_add(1, std::memory_order_relaxed);
    }
    if (isGarbage) {
        g_f3Garbage.fetch_add(1, std::memory_order_relaxed);
    }
    if (latestRegion == nullptr) {
        return;
    }
    SlotRow* row = FindRow(latestRegion);
    size_t slots = 0;
    size_t markedH = 0;
    size_t unmarkedH = 0;
    if (row != nullptr) {
        g_f3JoinHit.fetch_add(1, std::memory_order_relaxed);
        slots = row->slots.load(std::memory_order_relaxed);
        markedH = row->markedHolders.load(std::memory_order_relaxed);
        unmarkedH = row->unmarkedHolders.load(std::memory_order_relaxed);
    }
    if (slots > 0) {
        g_f3LiveGt0.fetch_add(1, std::memory_order_relaxed);
    }
    const bool knownEmpty = latestRegion->IsRouteKnownEmpty();
    const bool auth = latestRegion->IsLiveCountAuthoritative();
    LiveInfo* liveInfo = latestRegion->GetLiveInfo();
    size_t slog = g_f3SampleLogged.fetch_add(1, std::memory_order_relaxed);
    if (slots > 0 || slog < kSampleCap) {
        std::fprintf(stderr,
                     "[GCV2][garbregion][f3] n=%zu reason=%s region=%p latest=%p type=%u route=%u "
                     "routeGen=%u live=%llu auth=%u knownEmpty=%u liveinfo=%p "
                     "slots=%zu markedH=%zu unmarkedH=%zu join=%u gc=%zu\n",
                     n, reason != nullptr ? reason : "?", latestRegion, latest,
                     static_cast<unsigned>(latestRegion->GetRegionType()),
                     static_cast<unsigned>(latestRegion->GetRouteState()),
                     static_cast<unsigned>(latestRegion->GetRouteMarkGeneration()),
                     static_cast<unsigned long long>(latestRegion->GetLiveByteCount()),
                     static_cast<unsigned>(auth), static_cast<unsigned>(knownEmpty), liveInfo, slots, markedH,
                     unmarkedH, static_cast<unsigned>(row != nullptr), g_gcCount.load(std::memory_order_relaxed));
        std::fflush(stderr);
    }
}

void Report(const char* point)
{
    if (!kGarbRegion) {
        return;
    }
    std::fprintf(stderr,
                 "[GCV2][garbregion] point=%s censusN=%zu holders=%zu slots=%zu regionsHit=%zu sat=%zu "
                 "enter=%zu enter_ke=%zu enter_join=%zu enter_miss=%zu enter_liveGt0=%zu enter_liveMarkedGt0=%zu "
                 "cls_noAuth=%zu cls_liveinfoNull=%zu cls_epochStale=%zu cls_largeUnmarked=%zu "
                 "cls_notEmpty=%zu cls_neverExamined=%zu "
                 "f3=%zu f3_garbage=%zu f3_free=%zu f3_join=%zu f3_liveGt0=%zu where=%s\n",
                 point != nullptr ? point : "?", g_censusN.load(std::memory_order_relaxed),
                 g_censusHolders.load(std::memory_order_relaxed), g_censusSlots.load(std::memory_order_relaxed),
                 g_tabUsed.load(std::memory_order_relaxed), g_tabSat.load(std::memory_order_relaxed),
                 g_enter.load(std::memory_order_relaxed), g_enterKnownEmpty.load(std::memory_order_relaxed),
                 g_enterJoinHit.load(std::memory_order_relaxed), g_enterJoinMiss.load(std::memory_order_relaxed),
                 g_enterLiveGt0.load(std::memory_order_relaxed), g_enterLiveMarkedGt0.load(std::memory_order_relaxed),
                 g_clsNoAuth.load(std::memory_order_relaxed), g_clsLiveinfoNull.load(std::memory_order_relaxed),
                 g_clsEpochStale.load(std::memory_order_relaxed), g_clsLargeUnmarked.load(std::memory_order_relaxed),
                 g_clsNotEmpty.load(std::memory_order_relaxed), g_clsNeverExamined.load(std::memory_order_relaxed),
                 g_f3Hits.load(std::memory_order_relaxed), g_f3Garbage.load(std::memory_order_relaxed),
                 g_f3Free.load(std::memory_order_relaxed), g_f3JoinHit.load(std::memory_order_relaxed),
                 g_f3LiveGt0.load(std::memory_order_relaxed), g_lastWhere);
    std::fflush(stderr);
}

} // namespace GarbRegionDiag
} // namespace MapleRuntime
