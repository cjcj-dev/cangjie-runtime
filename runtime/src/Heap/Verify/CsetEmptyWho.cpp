// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/CsetEmptyWho.h"

#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Common/Runtime.h"
#include "Concurrency/ConcurrencyModel.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace CsetEmptyWho {
namespace {

constexpr bool kCsetEmptyWho = false;
constexpr size_t kMaxKeepPages = 32768;
constexpr size_t kLogPages = 4;

enum class Who : uint8_t {
    None = 0,
    Static = 1,
    Stack = 2,
    ExtraRoot = 3,
    Young = 4,
    OldUnmarked = 5,
    OldMarked = 6,
};

const char* WhoName(Who w)
{
    switch (w) {
        case Who::Static:
            return "STATIC";
        case Who::Stack:
            return "STACK";
        case Who::ExtraRoot:
            return "EXTRA_ROOT";
        case Who::Young:
            return "YOUNG";
        case Who::OldUnmarked:
            return "OLD_UNMARKED";
        case Who::OldMarked:
            return "OLD_MARKED";
        default:
            return "NONE";
    }
}

struct Sample {
    RegionInfo* region = nullptr;
    uintptr_t start = 0;
    size_t residual = 0;
    Who who = Who::None;
    size_t hits = 0;
};

std::vector<Sample> g_keeps;
std::unordered_map<RegionInfo*, size_t> g_keepIdx;

std::atomic<size_t> g_keep{ 0 };
std::atomic<size_t> g_sampledPages{ 0 };
std::atomic<size_t> g_holders{ 0 };
std::atomic<size_t> g_fields{ 0 };
std::atomic<size_t> g_pageHits{ 0 };
std::atomic<size_t> g_staticSeen{ 0 };
std::atomic<size_t> g_stackSeen{ 0 };
std::atomic<size_t> g_extraSeen{ 0 };
std::atomic<size_t> g_clsNone{ 0 };
std::atomic<size_t> g_clsStatic{ 0 };
std::atomic<size_t> g_clsStack{ 0 };
std::atomic<size_t> g_clsExtra{ 0 };
std::atomic<size_t> g_clsYoung{ 0 };
std::atomic<size_t> g_clsOldUnmarked{ 0 };
std::atomic<size_t> g_clsOldMarked{ 0 };
std::atomic<size_t> g_walkNs{ 0 };
std::atomic<size_t> g_cycles{ 0 };
std::atomic<bool> g_atexit{ false };

size_t g_cycleKeep = 0;

void BumpClass(Who w)
{
    switch (w) {
        case Who::Static:
            g_clsStatic.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::Stack:
            g_clsStack.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::ExtraRoot:
            g_clsExtra.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::Young:
            g_clsYoung.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::OldUnmarked:
            g_clsOldUnmarked.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::OldMarked:
            g_clsOldMarked.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            g_clsNone.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

Who Stronger(Who a, Who b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

Who ClassifyHolder(RegionInfo* holderRegion, BaseObject* holder)
{
    if (holderRegion == nullptr) {
        return Who::None;
    }
    if (holderRegion->IsYoungRegion()) {
        return Who::Young;
    }
    bool marked = false;
    if (holder != nullptr && Collector::PlausibleManagedObjectGate("CsetEmptyWho.holderMark", holder)) {
        marked = holderRegion->IsMarkedObject(holderRegion->GetMarkView<Generation::Old>(), holder);
    }
    return marked ? Who::OldMarked : Who::OldUnmarked;
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

void HitTarget(BaseObject* target, Who cls)
{
    if (target == nullptr || !Heap::IsHeapAddress(target)) {
        return;
    }
    RegionInfo* tr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
    if (tr == nullptr) {
        return;
    }
    auto it = g_keepIdx.find(tr);
    if (it == g_keepIdx.end()) {
        return;
    }
    Sample& s = g_keeps[it->second];
    s.who = Stronger(s.who, cls);
    ++s.hits;
    g_pageHits.fetch_add(1, std::memory_order_relaxed);
}

void VisitRootSlot(RootSlot& root, Who cls, std::atomic<size_t>& counter)
{
    zaddress_unsafe value = root.LoadPlain();
    if (is_null(value)) {
        return;
    }
    counter.fetch_add(1, std::memory_order_relaxed);
    HitTarget(to_object(safe(value)), cls);
}

} // namespace

void BeginCycle()
{
    if (!kCsetEmptyWho) {
        return;
    }
    EnsureAtexit();
    g_cycleKeep = 0;
    g_keeps.clear();
    g_keepIdx.clear();
    g_cycles.fetch_add(1, std::memory_order_relaxed);
}

void NoteKeep(RegionInfo* region, size_t residual, size_t residualFwd, size_t marked)
{
    if (!kCsetEmptyWho || region == nullptr) {
        return;
    }
    EnsureAtexit();
    g_keep.fetch_add(1, std::memory_order_relaxed);
    ++g_cycleKeep;
    (void)residualFwd;
    (void)marked;
    if (g_keeps.size() >= kMaxKeepPages || residual == 0) {
        return;
    }
    Sample s;
    s.region = region;
    s.start = region->GetRegionStart();
    s.residual = residual;
    const size_t idx = g_keeps.size();
    g_keeps.push_back(s);
    g_keepIdx[region] = idx;
    g_sampledPages.fetch_add(1, std::memory_order_relaxed);
}

void ClassifyCycle()
{
    if (!kCsetEmptyWho || g_keeps.empty()) {
        return;
    }
    const uint64_t t0 = TimeUtil::NanoSeconds();

    Heap::GetHeap().VisitStaticRoots([](RootSlot& root) { VisitRootSlot(root, Who::Static, g_staticSeen); });

    MutatorManager::Instance().VisitAllMutators([](Mutator& mutator) {
        mutator.VisitMutatorRoots([](RootSlot& root) { VisitRootSlot(root, Who::Stack, g_stackSeen); });
    });

    Heap::GetHeap().GetFinalizerProcessor().VisitRawPointers(
        [](RootSlot& root) { VisitRootSlot(root, Who::ExtraRoot, g_extraSeen); });
    Heap::GetHeap().VisitAllExportRoots(
        [](RootSlot& root) { VisitRootSlot(root, Who::ExtraRoot, g_extraSeen); });
    {
        RootVisitor conc = [](RootSlot& root) { VisitRootSlot(root, Who::ExtraRoot, g_extraSeen); };
        Runtime::Current().GetConcurrencyModel().VisitGCRoots(&conc);
    }

    Heap::GetHeap().ForEachObj(
        [](BaseObject* holder) {
            if (holder == nullptr) {
                return;
            }
            g_holders.fetch_add(1, std::memory_order_relaxed);
            if (!holder->HasRefField()) {
                return;
            }
            if (!Collector::PlausibleManagedObjectGate("CsetEmptyWho.holder", holder)) {
                return;
            }
            RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
            if (hr == nullptr || hr->IsFreeRegion() || hr->IsGarbageRegion()) {
                return;
            }
            Who cls = ClassifyHolder(hr, holder);
            holder->ForEachRefField([cls](RefField<>& field) {
                g_fields.fetch_add(1, std::memory_order_relaxed);
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                HitTarget(target, cls);
            });
        },
        false);

    const uint64_t dt = TimeUtil::NanoSeconds() - t0;
    g_walkNs.fetch_add(static_cast<size_t>(dt), std::memory_order_relaxed);
    size_t logged = 0;
    for (const Sample& s : g_keeps) {
        BumpClass(s.who);
        if (logged < kLogPages && (s.residual >= 2730 || s.hits != 0)) {
            LOG(RTLOG_ERROR,
                "[OLDROOTS][cset-who] region=%p start=%#zx residual=%zu hits=%zu page=%s walkNs=%llu keepPages=%zu",
                s.region, s.start, s.residual, s.hits, WhoName(s.who), static_cast<unsigned long long>(dt),
                g_keeps.size());
            ++logged;
        }
    }
    g_keeps.clear();
    g_keepIdx.clear();
}

void Report(const char* tag)
{
    std::fprintf(stderr,
                 "[OLDROOTS][cset-who] %s cycles=%zu keep=%zu sampledPages=%zu "
                 "holdersVisited=%zu fieldsSeen=%zu pageHits=%zu staticSeen=%zu stackSeen=%zu extraSeen=%zu "
                 "page STATIC=%zu STACK=%zu EXTRA=%zu YOUNG=%zu OLD_UNMARKED=%zu OLD_MARKED=%zu NONE=%zu "
                 "walkNs=%zu\n",
                 tag != nullptr ? tag : "?",
                 g_cycles.load(std::memory_order_relaxed),
                 g_keep.load(std::memory_order_relaxed),
                 g_sampledPages.load(std::memory_order_relaxed),
                 g_holders.load(std::memory_order_relaxed),
                 g_fields.load(std::memory_order_relaxed),
                 g_pageHits.load(std::memory_order_relaxed),
                 g_staticSeen.load(std::memory_order_relaxed),
                 g_stackSeen.load(std::memory_order_relaxed),
                 g_extraSeen.load(std::memory_order_relaxed),
                 g_clsStatic.load(std::memory_order_relaxed),
                 g_clsStack.load(std::memory_order_relaxed),
                 g_clsExtra.load(std::memory_order_relaxed),
                 g_clsYoung.load(std::memory_order_relaxed),
                 g_clsOldUnmarked.load(std::memory_order_relaxed),
                 g_clsOldMarked.load(std::memory_order_relaxed),
                 g_clsNone.load(std::memory_order_relaxed),
                 g_walkNs.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

} // namespace CsetEmptyWho
} // namespace MapleRuntime
