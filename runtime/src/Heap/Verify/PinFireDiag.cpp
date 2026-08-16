#include "Heap/Verify/PinFireDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "securec.h"

namespace MapleRuntime {
namespace PinFireDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool GateOn()
{
    static const bool on = EnvIsOne("MRT_GCV2_PINFIRE");
    return on;
}

std::atomic<uint64_t> g_addRaw{ 0 };
std::atomic<uint64_t> g_collectPinned{ 0 };
std::atomic<uint64_t> g_skipFreeSlots{ 0 };
std::atomic<uint64_t> g_skipRegion{ 0 };
std::atomic<uint64_t> g_skipBytes{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR, "[GCV2][pinfire] health probe_live=1 env=MRT_GCV2_PINFIRE=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

// Best-effort: unmarked live-looking slots while pin holds (cost of pinroot hold).
uint64_t UnmarkedBytes(RegionInfo* region)
{
    if (region == nullptr) {
        return 0;
    }
    uint64_t bytes = 0;
    size_t start = region->GetRegionStart();
    MarkView<Generation::Old> view = region->GetMarkView<Generation::Old>();
    region->VisitAllObjects([region, view, start, &bytes](BaseObject* object) {
        size_t offset = reinterpret_cast<MAddress>(object) - start;
        if (!region->IsSurvivedObject(view, offset)) {
            bytes += object->GetSize();
        }
    });
    return bytes;
}

} // namespace

bool Enabled() { return GateOn(); }

void NoteAddRawPointer()
{
    if (LIKELY(!GateOn())) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_addRaw.fetch_add(1, std::memory_order_relaxed);
}

void NoteCollectPinnedGarbage()
{
    if (LIKELY(!GateOn())) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_collectPinned.fetch_add(1, std::memory_order_relaxed);
}

void NoteSkipFreeSlots(RegionInfo* region)
{
    if (LIKELY(!GateOn())) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_skipFreeSlots.fetch_add(1, std::memory_order_relaxed);
    g_skipBytes.fetch_add(UnmarkedBytes(region), std::memory_order_relaxed);
}

void NoteSkipRegion(RegionInfo* region)
{
    if (LIKELY(!GateOn())) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_skipRegion.fetch_add(1, std::memory_order_relaxed);
    g_skipBytes.fetch_add(UnmarkedBytes(region), std::memory_order_relaxed);
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    uint64_t add = g_addRaw.load(std::memory_order_relaxed);
    uint64_t col = g_collectPinned.load(std::memory_order_relaxed);
    uint64_t skipSlot = g_skipFreeSlots.load(std::memory_order_relaxed);
    uint64_t skipReg = g_skipRegion.load(std::memory_order_relaxed);
    uint64_t skip = skipSlot + skipReg;
    uint64_t bytes = g_skipBytes.load(std::memory_order_relaxed);
    char buf[512];
    int n = sprintf_s(buf, sizeof(buf),
                      "[GCV2][pinfire] point=%s add_raw=%llu collect_pinned=%llu "
                      "skip_total=%llu (free_slots=%llu region=%llu) skip_bytes=%llu\n",
                      point != nullptr ? point : "?", static_cast<unsigned long long>(add),
                      static_cast<unsigned long long>(col), static_cast<unsigned long long>(skip),
                      static_cast<unsigned long long>(skipSlot), static_cast<unsigned long long>(skipReg),
                      static_cast<unsigned long long>(bytes));
    if (n > 0) {
        WriteLine(buf, static_cast<size_t>(n));
        LOG(RTLOG_ERROR,
            "[GCV2][pinfire] point=%s add_raw=%llu collect_pinned=%llu skip_total=%llu "
            "(free_slots=%llu region=%llu) skip_bytes=%llu",
            point != nullptr ? point : "?", static_cast<unsigned long long>(add),
            static_cast<unsigned long long>(col), static_cast<unsigned long long>(skip),
            static_cast<unsigned long long>(skipSlot), static_cast<unsigned long long>(skipReg),
            static_cast<unsigned long long>(bytes));
    }
}

} // namespace PinFireDiag
} // namespace MapleRuntime
