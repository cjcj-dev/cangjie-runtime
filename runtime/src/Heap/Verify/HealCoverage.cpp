#include "Heap/Verify/HealCoverage.h"

#include <atomic>
#include <cstdio>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace HealCoverage {

static std::atomic<uint64_t> g_censusN{ 0 };
static std::atomic<uint64_t> g_lastStale{ 0 };

static Face ClassifyFace(BaseObject* holder, BaseObject* target)
{
    RegionInfo* hr = holder == nullptr ? nullptr :
        RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    RegionInfo* tr = target == nullptr || !Heap::IsHeapAddress(target) ? nullptr :
        RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    const bool holderYoung = hr != nullptr && hr->IsYoungRegion();
    const bool holderPinned = hr != nullptr && hr->IsPinnedRegion();
    const bool holderFrom = hr != nullptr && hr->IsFromRegion();
    const bool targetYoung = tr != nullptr && tr->IsYoungRegion();
    return FaceOf(holderYoung, holderPinned, holderFrom, targetYoung);
}

void CensusAfterPublication(uintptr_t currentRemap, uint64_t flipSeq)
{
    if (!kHealCoverageCensus) {
        (void)currentRemap;
        (void)flipSeq;
        return;
    }

    const uintptr_t loadBad = ::g_cjLoadBadMask;
    Counts c{};
#ifndef MRT_HEAL_COVERAGE_INJECT
#define MRT_HEAL_COVERAGE_INJECT 0
#endif
    if (MRT_HEAL_COVERAGE_INJECT != 0) {
        const uintptr_t injected = PaintStale(0x00007f00'00001000ULL, currentRemap);
        Add(c, Classify(injected, loadBad));
        if (IsCoverageMiss(Classify(injected, loadBad))) {
            ++c.injectHits;
        }
    }

    Heap::GetHeap().ForEachObj(
        [&c, loadBad](BaseObject* holder) {
            if (holder == nullptr || !holder->IsValidObject() || !holder->HasRefField()) {
                return;
            }
            RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (hr == nullptr || hr->IsGarbageRegion() || hr->IsFreeRegion()) {
                return;
            }
            if (!Collector::PlausibleManagedObjectGate("healcov-census", holder)) {
                return;
            }
            holder->ForEachRefField([&c, loadBad, holder](RefField<>& field) {
                const uintptr_t value = raw(field.GetFieldValue());
                const Kind k = Classify(value, loadBad);
                Add(c, k);
                if (!IsCoverageMiss(k)) {
                    return;
                }
                BaseObject* target = to_object(field.GetTargetObject());
                AddFace(c, ClassifyFace(holder, target));
            });
        },
        false);

    const uint64_t n = g_censusN.fetch_add(1, std::memory_order_relaxed) + 1;
    g_lastStale.store(c.stale, std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "[HEALCOV] n=%llu flipSeq=%llu remap=%#lx loadBad=%#lx "
        "null=%zu plain=%zu loadGood=%zu stale=%zu "
        "face young=%zu o2y=%zu o2o=%zu pin=%zu from=%zu unk=%zu inject=%zu",
        static_cast<unsigned long long>(n), static_cast<unsigned long long>(flipSeq),
        static_cast<unsigned long>(currentRemap), static_cast<unsigned long>(loadBad),
        c.nulls, c.plains, c.loadGood, c.stale, c.youngHolder, c.oldToYoung, c.oldToOld,
        c.pinnedHolder, c.fromHolder, c.unknownFace, c.injectHits);
}

} // namespace HealCoverage
} // namespace MapleRuntime
