// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/CsetEmptyWho.h"

#include <atomic>
#include <cstdio>
#include <vector>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace CsetEmptyWho {
namespace {

constexpr bool kCsetEmptyWho = true;
constexpr size_t kMaxSamplePages = 2;

enum class Who : uint8_t {
    None = 0,
    Static = 1,
    Stack = 2,
    Young = 3,
    OldUnmarked = 4,
    OldMarked = 5,
};

const char* WhoName(Who w)
{
    switch (w) {
        case Who::Static:
            return "STATIC";
        case Who::Stack:
            return "STACK";
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
    uintptr_t alloc = 0;
    size_t residual = 0;
    Who who = Who::None;
    size_t hits = 0;
};

std::vector<Sample> g_samples;

std::atomic<size_t> g_keep{ 0 };
std::atomic<size_t> g_sampledPages{ 0 };
std::atomic<size_t> g_holders{ 0 };
std::atomic<size_t> g_fields{ 0 };
std::atomic<size_t> g_pageHits{ 0 };
std::atomic<size_t> g_staticSeen{ 0 };
std::atomic<size_t> g_stackSeen{ 0 };
std::atomic<size_t> g_clsNone{ 0 };
std::atomic<size_t> g_clsStatic{ 0 };
std::atomic<size_t> g_clsStack{ 0 };
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
    if (target == nullptr) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(target);
    for (Sample& s : g_samples) {
        if (addr >= s.start && addr < s.alloc) {
            s.who = Stronger(s.who, cls);
            ++s.hits;
            g_pageHits.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace

void BeginCycle()
{
    if (!kCsetEmptyWho) {
        return;
    }
    EnsureAtexit();
    g_cycleKeep = 0;
    g_samples.clear();
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
    if (g_samples.size() >= kMaxSamplePages || residual == 0) {
        return;
    }
    Sample s;
    s.region = region;
    s.start = region->GetRegionStart();
    s.alloc = region->GetRegionAllocPtr();
    s.residual = residual;
    if (s.alloc <= s.start) {
        return;
    }
    g_samples.push_back(s);
    g_sampledPages.fetch_add(1, std::memory_order_relaxed);
}

void ClassifyCycle()
{
    if (!kCsetEmptyWho || g_samples.empty()) {
        return;
    }
    const uint64_t t0 = TimeUtil::NanoSeconds();

    Heap::GetHeap().VisitStaticRoots([](RootSlot& root) {
        zaddress_unsafe value = root.LoadPlain();
        if (is_null(value)) {
            return;
        }
        g_staticSeen.fetch_add(1, std::memory_order_relaxed);
        // StaticRootTable keeps the referent (DumpRoots proof). Peel only.
        HitTarget(to_object(safe(value)), Who::Static);
    });

    MutatorManager::Instance().VisitAllMutators([](Mutator& mutator) {
        mutator.VisitMutatorRoots([](RootSlot& root) {
            zaddress_unsafe value = root.LoadPlain();
            if (is_null(value)) {
                return;
            }
            g_stackSeen.fetch_add(1, std::memory_order_relaxed);
            HitTarget(to_object(safe(value)), Who::Stack);
        });
    });

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
    for (const Sample& s : g_samples) {
        BumpClass(s.who);
        LOG(RTLOG_ERROR,
            "[OLDROOTS][cset-who] region=%p start=%#zx residual=%zu hits=%zu page=%s walkNs=%llu",
            s.region, s.start, s.residual, s.hits, WhoName(s.who), static_cast<unsigned long long>(dt));
    }
    g_samples.clear();
}

void Report(const char* tag)
{
    std::fprintf(stderr,
                 "[OLDROOTS][cset-who] %s cycles=%zu keep=%zu sampledPages=%zu "
                 "holdersVisited=%zu fieldsSeen=%zu pageHits=%zu staticSeen=%zu stackSeen=%zu "
                 "page STATIC=%zu STACK=%zu YOUNG=%zu OLD_UNMARKED=%zu OLD_MARKED=%zu NONE=%zu "
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
                 g_clsStatic.load(std::memory_order_relaxed),
                 g_clsStack.load(std::memory_order_relaxed),
                 g_clsYoung.load(std::memory_order_relaxed),
                 g_clsOldUnmarked.load(std::memory_order_relaxed),
                 g_clsOldMarked.load(std::memory_order_relaxed),
                 g_clsNone.load(std::memory_order_relaxed),
                 g_walkNs.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

} // namespace CsetEmptyWho
} // namespace MapleRuntime
