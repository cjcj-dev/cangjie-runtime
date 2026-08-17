#include "Heap/Verify/LostWriteProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"

namespace MapleRuntime {
namespace LostWriteProbe {
namespace {

struct Sample {
    uintptr_t obj = 0;
    uintptr_t field = 0;
    uint32_t off = 0;
    uint32_t phase = 0;
};

constexpr size_t kSampleCap = 8;
constexpr uint32_t kTlsFlush = 4096;

std::atomic<uint64_t> g_a{ 0 };
std::atomic<uint64_t> g_b{ 0 };
std::atomic<uint64_t> g_c{ 0 };
std::atomic<uint64_t> g_wr{ 0 };
std::atomic<uint64_t> g_resolveNull{ 0 };
std::atomic<uint64_t> g_tryfwdNullGhost{ 0 };
std::atomic<uint64_t> g_tryfwdNullOther{ 0 };

std::atomic<uint32_t> g_aN{ 0 };
std::atomic<uint32_t> g_bN{ 0 };
std::atomic<uint32_t> g_cN{ 0 };
Sample g_aS[kSampleCap];
Sample g_bS[kSampleCap];
Sample g_cS[kSampleCap];

std::atomic<bool> g_atexit{ false };

thread_local uint32_t t_wr = 0;

void FlushWr()
{
    if (t_wr != 0) {
        g_wr.fetch_add(t_wr, std::memory_order_relaxed);
        t_wr = 0;
    }
}

void PushSample(std::atomic<uint32_t>& n, Sample* ring, uintptr_t obj, uintptr_t field, uint32_t phase)
{
    uint32_t i = n.fetch_add(1, std::memory_order_relaxed);
    if (i >= kSampleCap) {
        return;
    }
    uint32_t off = 0;
    if (obj != 0 && field >= obj) {
        const uint64_t d = field - obj;
        if (d <= 0xffffffffull) {
            off = static_cast<uint32_t>(d);
        }
    }
    ring[i] = Sample{ obj, field, off, phase };
}

void DumpSamples(const char* kind, const Sample* ring, uint32_t n)
{
    const uint32_t lim = n < kSampleCap ? n : static_cast<uint32_t>(kSampleCap);
    for (uint32_t i = 0; i < lim; ++i) {
        std::fprintf(stderr, "[LOSTWRITE] sample kind=%s i=%u obj=%p field=%p off=%u phase=%u\n", kind, i,
                     reinterpret_cast<void*>(ring[i].obj), reinterpret_cast<void*>(ring[i].field), ring[i].off,
                     ring[i].phase);
    }
}

void Dump(const char* point)
{
    FlushWr();
    std::fprintf(stderr,
                 "[LOSTWRITE] point=%s a=%llu b=%llu c=%llu wr=%llu resolve_null=%llu "
                 "tryfwd_null=%llu tryfwd_null_other=%llu\n",
                 point == nullptr ? "?" : point,
                 static_cast<unsigned long long>(g_a.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_b.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_c.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_wr.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_resolveNull.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_tryfwdNullGhost.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_tryfwdNullOther.load(std::memory_order_relaxed)));
    DumpSamples("A", g_aS, g_aN.load(std::memory_order_relaxed));
    DumpSamples("B", g_bS, g_bN.load(std::memory_order_relaxed));
    DumpSamples("C", g_cS, g_cN.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Dump("atexit"); });
    }
}

} // namespace

void NoteWrite(BaseObject* obj, const void* field, const Collector& collector)
{
    if (++t_wr >= kTlsFlush) {
        FlushWr();
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    const GCPhase phase = collector.GetGCPhase();
    if (phase != GCPhase::GC_PHASE_PREFORWARD && phase != GCPhase::GC_PHASE_FORWARD) {
        return;
    }
    EnsureAtexit();
    const uint32_t phaseU = static_cast<uint32_t>(phase);
    const uintptr_t objU = reinterpret_cast<uintptr_t>(obj);
    const uintptr_t fieldU = reinterpret_cast<uintptr_t>(field);
    if (collector.IsGhostFromObject(obj)) {
        if (obj->IsForwarded()) {
            g_a.fetch_add(1, std::memory_order_relaxed);
            PushSample(g_aN, g_aS, objU, fieldU, phaseU);
        } else {
            g_b.fetch_add(1, std::memory_order_relaxed);
            PushSample(g_bN, g_bS, objU, fieldU, phaseU);
        }
        return;
    }
    if (!obj->IsValidObject()) {
        RegionInfo* reg = RegionInfo::TryGetRegionInfoAt(objU);
        if (reg != nullptr && reg->IsRouteDestHeld()) {
            g_c.fetch_add(1, std::memory_order_relaxed);
            PushSample(g_cN, g_cS, objU, fieldU, phaseU);
        }
    }
}

void NoteResolveNull(BaseObject* from, bool movableGhost)
{
    if (movableGhost && from != nullptr) {
        g_resolveNull.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteTryForwardNull(BaseObject* from, bool movableGhost)
{
    if (movableGhost && from != nullptr) {
        g_tryfwdNullGhost.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_tryfwdNullOther.fetch_add(1, std::memory_order_relaxed);
    }
}

void Report(const char* point) { Dump(point == nullptr ? "report" : point); }

} // namespace LostWriteProbe
} // namespace MapleRuntime
