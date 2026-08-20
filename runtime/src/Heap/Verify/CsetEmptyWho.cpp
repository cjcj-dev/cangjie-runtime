// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/CsetEmptyWho.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace CsetEmptyWho {
namespace {

constexpr bool kCsetEmptyWho = true;
constexpr size_t kMaxSamplePages = 2;
constexpr size_t kMaxSampleObjs = 8;

enum class Who : uint8_t {
    None = 0,
    Static = 1,
    Young = 2,
    OldUnmarked = 3,
    OldMarked = 4,
};

const char* WhoName(Who w)
{
    switch (w) {
        case Who::Static:
            return "STATIC";
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

std::atomic<size_t> g_keep{ 0 };
std::atomic<size_t> g_sampledPages{ 0 };
std::atomic<size_t> g_sampledObjs{ 0 };
std::atomic<size_t> g_clsNone{ 0 };
std::atomic<size_t> g_clsStatic{ 0 };
std::atomic<size_t> g_clsYoung{ 0 };
std::atomic<size_t> g_clsOldUnmarked{ 0 };
std::atomic<size_t> g_clsOldMarked{ 0 };
std::atomic<size_t> g_pageNone{ 0 };
std::atomic<size_t> g_pageStatic{ 0 };
std::atomic<size_t> g_pageYoung{ 0 };
std::atomic<size_t> g_pageOldUnmarked{ 0 };
std::atomic<size_t> g_pageOldMarked{ 0 };
std::atomic<size_t> g_walkNs{ 0 };
std::atomic<size_t> g_cycles{ 0 };
std::atomic<bool> g_atexit{ false };

size_t g_cycleKeep = 0;
size_t g_cycleSampled = 0;

void BumpClass(Who w)
{
    switch (w) {
        case Who::Static:
            g_clsStatic.fetch_add(1, std::memory_order_relaxed);
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

void BumpPageClass(Who w)
{
    switch (w) {
        case Who::Static:
            g_pageStatic.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::Young:
            g_pageYoung.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::OldUnmarked:
            g_pageOldUnmarked.fetch_add(1, std::memory_order_relaxed);
            break;
        case Who::OldMarked:
            g_pageOldMarked.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            g_pageNone.fetch_add(1, std::memory_order_relaxed);
            break;
    }
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

Who Stronger(Who a, Who b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

void ClassifyPage(RegionInfo* region, const std::vector<BaseObject*>& objs)
{
    std::unordered_map<BaseObject*, Who> who;
    who.reserve(objs.size());
    for (BaseObject* o : objs) {
        who[o] = Who::None;
    }

    auto consider = [&who](BaseObject* target, Who cls) {
        if (target == nullptr) {
            return;
        }
        auto it = who.find(target);
        if (it == who.end()) {
            if (!Collector::PlausibleManagedObjectGate("CsetEmptyWho.target", target)) {
                BaseObject* host = Collector::TryRecoverInteriorBase(target);
                if (host == nullptr) {
                    return;
                }
                it = who.find(host);
                if (it == who.end()) {
                    return;
                }
            } else {
                return;
            }
        }
        it->second = Stronger(it->second, cls);
    };

    Heap::GetHeap().VisitStaticRoots([&consider](RootSlot& root) {
        zaddress_unsafe value = root.LoadPlain();
        if (is_null(value)) {
            return;
        }
        // StaticRootTable keeps the referent while this observe-only decode runs
        // (same proof as TracingCollector::DumpRoots).
        consider(to_object(safe(value)), Who::Static);
    });

    Heap::GetHeap().ForEachObj(
        [&consider](BaseObject* holder) {
            if (holder == nullptr || !holder->HasRefField()) {
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
            holder->ForEachRefField([&consider, cls](RefField<>& field) {
                BaseObject* target = to_object(field.GetTargetObject());
                if (target == nullptr || !Heap::IsHeapAddress(target)) {
                    return;
                }
                consider(target, cls);
            });
        },
        false);

    Who pageWho = Who::None;
    for (BaseObject* o : objs) {
        Who w = who[o];
        BumpClass(w);
        pageWho = Stronger(pageWho, w);
    }
    BumpPageClass(pageWho);
    LOG(RTLOG_ERROR,
        "[OLDROOTS][cset-who] region=%p start=%#zx objs=%zu page=%s "
        "(STATIC/YOUNG/OLD_UNMARKED/OLD_MARKED/NONE object totals next atexit)",
        region, region->GetRegionStart(), objs.size(), WhoName(pageWho));
}

} // namespace

void BeginCycle()
{
    if (!kCsetEmptyWho) {
        return;
    }
    EnsureAtexit();
    g_cycleKeep = 0;
    g_cycleSampled = 0;
    g_cycles.fetch_add(1, std::memory_order_relaxed);
}

void NoteKeep(RegionInfo* region, size_t residual, size_t residualFwd, size_t marked)
{
    if (!kCsetEmptyWho || region == nullptr) {
        return;
    }
    EnsureAtexit();
    const size_t n = g_keep.fetch_add(1, std::memory_order_relaxed) + 1;
    ++g_cycleKeep;
    (void)residualFwd;
    (void)marked;
    if (g_cycleSampled >= kMaxSamplePages || residual == 0) {
        return;
    }
    std::vector<BaseObject*> objs;
    objs.reserve(kMaxSampleObjs);
    const uintptr_t start = region->GetRegionStart();
    const uintptr_t alloc = region->GetRegionAllocPtr();
    uintptr_t pos = start;
    while (pos < alloc && objs.size() < kMaxSampleObjs) {
        BaseObject* o = from_region_addr(pos);
        if (!o->IsValidObject()) {
            break;
        }
        const size_t sz = o->GetSize();
        if (sz == 0) {
            break;
        }
        if (!o->IsForwarded()) {
            objs.push_back(o);
        }
        pos += sz;
    }
    if (objs.empty()) {
        return;
    }
    ++g_cycleSampled;
    g_sampledPages.fetch_add(1, std::memory_order_relaxed);
    g_sampledObjs.fetch_add(objs.size(), std::memory_order_relaxed);
    const uint64_t t0 = TimeUtil::NanoSeconds();
    ClassifyPage(region, objs);
    const uint64_t dt = TimeUtil::NanoSeconds() - t0;
    g_walkNs.fetch_add(static_cast<size_t>(dt), std::memory_order_relaxed);
    LOG(RTLOG_ERROR,
        "[OLDROOTS][cset-who] sampled nKeep=%zu cycleKeep=%zu walkNs=%llu residual=%zu marked=%zu",
        n, g_cycleKeep, static_cast<unsigned long long>(dt), residual, marked);
}

void Report(const char* tag)
{
    std::fprintf(stderr,
                 "[OLDROOTS][cset-who] %s cycles=%zu keep=%zu sampledPages=%zu sampledObjs=%zu "
                 "obj STATIC=%zu YOUNG=%zu OLD_UNMARKED=%zu OLD_MARKED=%zu NONE=%zu "
                 "page STATIC=%zu YOUNG=%zu OLD_UNMARKED=%zu OLD_MARKED=%zu NONE=%zu "
                 "walkNs=%zu STACK_SKIPPED=1 (PostTrace not STW)\n",
                 tag != nullptr ? tag : "?",
                 g_cycles.load(std::memory_order_relaxed),
                 g_keep.load(std::memory_order_relaxed),
                 g_sampledPages.load(std::memory_order_relaxed),
                 g_sampledObjs.load(std::memory_order_relaxed),
                 g_clsStatic.load(std::memory_order_relaxed),
                 g_clsYoung.load(std::memory_order_relaxed),
                 g_clsOldUnmarked.load(std::memory_order_relaxed),
                 g_clsOldMarked.load(std::memory_order_relaxed),
                 g_clsNone.load(std::memory_order_relaxed),
                 g_pageStatic.load(std::memory_order_relaxed),
                 g_pageYoung.load(std::memory_order_relaxed),
                 g_pageOldUnmarked.load(std::memory_order_relaxed),
                 g_pageOldMarked.load(std::memory_order_relaxed),
                 g_pageNone.load(std::memory_order_relaxed),
                 g_walkNs.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

} // namespace CsetEmptyWho
} // namespace MapleRuntime
