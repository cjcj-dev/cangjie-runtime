#include "Heap/Verify/ArrayWalkDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
namespace ArrayWalkDiag {
namespace {

struct WalkFrame {
    BaseObject* holder = nullptr;
    uint64_t declared = 0;
    uint64_t visits = 0;
    uint64_t push = 0;
    uint64_t skipMarked = 0;
    uint64_t skipGate = 0;
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
std::atomic<uint64_t> g_skipGateSum{ 0 };
std::atomic<uint64_t> g_skipNullSum{ 0 };
std::atomic<uint64_t> g_maxDeclared{ 0 };
std::atomic<uintptr_t> g_lastLargeHolder{ 0 };
std::atomic<uint64_t> g_lastLargeDeclared{ 0 };
std::atomic<uint64_t> g_lastLargeVisits{ 0 };

} // namespace

bool Enabled()
{
    static const bool on = DiagGate::LegacyOrToken("MRT_GCV2_ARRAYWALK", "arraywalk");
    return on;
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

void NoteSkipGate()
{
    if (t_active) {
        ++t_frame.skipGate;
    }
}

void NoteSkipNull()
{
    if (t_active) {
        ++t_frame.skipNull;
    }
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
    g_skipGateSum.fetch_add(t_frame.skipGate, std::memory_order_relaxed);
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
                     "push=%lu skipMarked=%lu skipGate=%lu skipNull=%lu incomplete=%d gc=%u\n",
                     static_cast<void*>(t_frame.holder), static_cast<unsigned>(t_frame.largeRegion),
                     static_cast<unsigned>(t_frame.componentKind),
                     static_cast<unsigned long>(t_frame.declared),
                     static_cast<unsigned long>(t_frame.visits),
                     static_cast<unsigned long>(t_frame.push),
                     static_cast<unsigned long>(t_frame.skipMarked),
                     static_cast<unsigned long>(t_frame.skipGate),
                     static_cast<unsigned long>(t_frame.skipNull),                      incomplete ? 1 : 0,
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
                 "visitsSum=%lu push=%lu skipMarked=%lu skipGate=%lu skipNull=%lu maxDeclared=%lu "
                 "lastLargeHolder=%p lastLargeDeclared=%lu lastLargeVisits=%lu\n",
                 point != nullptr ? point : "?",
                 static_cast<unsigned long>(g_walks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_largeWalks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_incomplete.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_declaredSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_visitsSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_pushSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipMarkedSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipGateSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_skipNullSum.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_maxDeclared.load(std::memory_order_relaxed)),
                 reinterpret_cast<void*>(g_lastLargeHolder.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_lastLargeDeclared.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(g_lastLargeVisits.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

} // namespace ArrayWalkDiag
} // namespace MapleRuntime
