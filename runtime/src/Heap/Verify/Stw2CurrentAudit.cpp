#include "Heap/Verify/Stw2CurrentAudit.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/Allocator.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/SatbBuffer.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace Stw2CurrentAudit {

static std::atomic<uint64_t> g_slots{ 0 };
static std::atomic<uint64_t> g_water{ 0 };
static std::atomic<uint64_t> g_allocBlack{ 0 };
static std::atomic<uint64_t> g_marked{ 0 };
static std::atomic<uint64_t> g_satb{ 0 };
static std::atomic<uint64_t> g_skip{ 0 };
static std::atomic<uint64_t> g_uncovered{ 0 };
static std::atomic<uint64_t> g_minors{ 0 };
static std::atomic<bool> g_inject{ false };
static std::atomic<bool> g_atexit{ false };

bool Enabled() { return kStw2CurrentAudit; }

void ArmInject() { g_inject.store(true, std::memory_order_relaxed); }

size_t Uncovered() { return g_uncovered.load(std::memory_order_relaxed); }
size_t Water() { return g_water.load(std::memory_order_relaxed); }
size_t AllocBlack() { return g_allocBlack.load(std::memory_order_relaxed); }
size_t Marked() { return g_marked.load(std::memory_order_relaxed); }
size_t Satb() { return g_satb.load(std::memory_order_relaxed); }
size_t Skip() { return g_skip.load(std::memory_order_relaxed); }
size_t Slots() { return g_slots.load(std::memory_order_relaxed); }
size_t Minors() { return g_minors.load(std::memory_order_relaxed); }

void Report(const char* tag)
{
    if (!kStw2CurrentAudit) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][stw2current] tag=%s minors=%llu slots=%llu water=%llu allocblack=%llu marked=%llu "
        "satb=%llu skip=%llu uncovered=%llu",
        tag != nullptr ? tag : "?",
        static_cast<unsigned long long>(g_minors.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_slots.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_water.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_allocBlack.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_marked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_satb.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_skip.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_uncovered.load(std::memory_order_relaxed)));
}

static void MaybeAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

Stw2Cover ClassifyTarget(BaseObject* target, const std::unordered_set<BaseObject*>& allocBlack,
                         const std::unordered_set<BaseObject*>& satb)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return Stw2Cover::Skip;
    }
    if (!Collector::PlausibleManagedObjectGate("stw2current.target", target)) {
        BaseObject* host = Collector::TryRecoverInteriorBase(target);
        if (host != nullptr && host != target) {
            target = host;
        } else {
            return Stw2Cover::Skip;
        }
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (region == nullptr || !region->IsYoungRegion()) {
        return Stw2Cover::Skip;
    }
    size_t offset = region->GetAddressOffset(reinterpret_cast<MAddress>(target));
    if (region->AllocatedAfterMarkStart(offset)) {
        return Stw2Cover::Water;
    }
    if (allocBlack.count(target) != 0) {
        return Stw2Cover::AllocBlack;
    }
    MarkView<Generation::Young> view = region->GetMarkView<Generation::Young>();
    if (region->IsMarkedObject(view, target)) {
        return Stw2Cover::Marked;
    }
    if (satb.count(target) != 0) {
        return Stw2Cover::Satb;
    }
    return Stw2Cover::Uncovered;
}

void Census(const std::unordered_set<MAddress>& currentSlots, Allocator* allocator)
{
    if (!kStw2CurrentAudit) {
        return;
    }
    MaybeAtexit();
    const uint64_t n = g_minors.fetch_add(1, std::memory_order_relaxed) + 1;
    g_slots.fetch_add(currentSlots.size(), std::memory_order_relaxed);

    std::unordered_set<BaseObject*> allocBlack;
    if (allocator != nullptr) {
        allocator->VisitAllocBuffers([&allocBlack](AllocBuffer& buffer) {
            std::vector<BaseObject*> peek;
            buffer.PeekYoungAllocBlack(peek);
            allocBlack.insert(peek.begin(), peek.end());
        });
    }

    std::unordered_set<BaseObject*> satbSet;
    SatbBuffer::Instance().PeekRetired([&satbSet](BaseObject* obj) {
        if (obj != nullptr) {
            satbSet.insert(obj);
        }
    });
    // Product STW2 always passes theAllocator. gc_unit inject/classify uses nullptr
    // so we do not VisitAllMutators against an uninitialised scheduler.
    if (allocator != nullptr) {
        MutatorManager::Instance().VisitAllMutators([&satbSet](Mutator& mutator) {
            SatbBuffer::Node* node = mutator.PeekSatbNode();
            if (node == nullptr) {
                return;
            }
            node->PeekEntries([&satbSet](BaseObject* obj) {
                if (obj != nullptr) {
                    satbSet.insert(obj);
                }
            });
        });
    }

    uint64_t waterN = 0;
    uint64_t allocN = 0;
    uint64_t markedN = 0;
    uint64_t satbN = 0;
    uint64_t skipN = 0;
    uint64_t uncoveredN = 0;

    for (MAddress slot : currentSlots) {
        if (!Heap::IsHeapAddress(slot)) {
            ++skipN;
            continue;
        }
        RefField<>& field = HeapSlotAt<>(slot);
        zaddress rawAddr = field.GetTargetObject();
        if (is_null(rawAddr)) {
            ++skipN;
            continue;
        }
        BaseObject* target = to_object(rawAddr);
        Stw2Cover cover = ClassifyTarget(target, allocBlack, satbSet);
        switch (cover) {
            case Stw2Cover::Water:
                ++waterN;
                break;
            case Stw2Cover::AllocBlack:
                ++allocN;
                break;
            case Stw2Cover::Marked:
                ++markedN;
                break;
            case Stw2Cover::Satb:
                ++satbN;
                break;
            case Stw2Cover::Skip:
                ++skipN;
                break;
            case Stw2Cover::Uncovered:
                ++uncoveredN;
                if (uncoveredN <= 8 || (uncoveredN & (uncoveredN - 1)) == 0) {
                    LOG(RTLOG_ERROR, "[GCV2][stw2current] UNCOVERED slot=%p target=%p minor=%llu",
                        reinterpret_cast<void*>(slot), static_cast<void*>(target),
                        static_cast<unsigned long long>(n));
                }
                break;
        }
    }

    if (g_inject.exchange(false, std::memory_order_relaxed)) {
        ++uncoveredN;
        LOG(RTLOG_ERROR, "[GCV2][stw2current] INJECT uncovered+=1 minor=%llu",
            static_cast<unsigned long long>(n));
    }

    g_water.fetch_add(waterN, std::memory_order_relaxed);
    g_allocBlack.fetch_add(allocN, std::memory_order_relaxed);
    g_marked.fetch_add(markedN, std::memory_order_relaxed);
    g_satb.fetch_add(satbN, std::memory_order_relaxed);
    g_skip.fetch_add(skipN, std::memory_order_relaxed);
    g_uncovered.fetch_add(uncoveredN, std::memory_order_relaxed);

    // Zero-case line: proves the sink ran even when every slot was covered.
    if (n == 1 || (n & (n - 1)) == 0 || uncoveredN != 0) {
        LOG(RTLOG_ERROR,
            "[GCV2][stw2current] census minor=%llu slots=%zu water=%llu allocblack=%llu marked=%llu "
            "satb=%llu skip=%llu uncovered=%llu",
            static_cast<unsigned long long>(n), currentSlots.size(),
            static_cast<unsigned long long>(waterN), static_cast<unsigned long long>(allocN),
            static_cast<unsigned long long>(markedN), static_cast<unsigned long long>(satbN),
            static_cast<unsigned long long>(skipN), static_cast<unsigned long long>(uncoveredN));
    }
}

} // namespace Stw2CurrentAudit
} // namespace MapleRuntime
