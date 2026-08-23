#include "Heap/Verify/HoleWhoDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Common/BaseObject.h"
#include "Common/ColourTypes.h"
#include "Heap/Allocator/HeapFiller.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/ManagedObjectGate.h"

namespace MapleRuntime {
namespace HoleWhoDiag {

constexpr bool kArmed = true;
constexpr unsigned kSampleCap = 128;
constexpr unsigned kBuckets = static_cast<unsigned>(Bucket::N);

static std::atomic<uint64_t> g_n{ 0 };
static std::atomic<uint64_t> g_bytes{ 0 };
static std::atomic<uint64_t> g_bucket[kBuckets];
static std::atomic<uint64_t> g_nextPlausible{ 0 };
static std::atomic<uint64_t> g_tl{ 0 };
static std::atomic<uint64_t> g_rf{ 0 };
static std::atomic<bool> g_atexit{ false };

static void EnsureAtexit();

static unsigned Classify(uintptr_t holeStart, uintptr_t holeEnd, uintptr_t regionStart, uint64_t nextWord)
{
    (void)regionStart;
    if (nextWord == 0) {
        return static_cast<unsigned>(Bucket::NEVER_WRITTEN);
    }
    if ((holeStart & 7u) == 0 && holeEnd > holeStart) {
        return static_cast<unsigned>(Bucket::AFTER_NEIGHBOR);
    }
    return static_cast<unsigned>(Bucket::HEADER_CLEARED);
}

void Dump(const char* point)
{
    if (!kArmed) {
        return;
    }
    std::fprintf(stderr,
                 "[GCV2][holewho] %s n=%llu bytes=%llu never=%llu after_neighbor=%llu header_cleared=%llu "
                 "next_plausible=%llu tl=%llu rf=%llu\n",
                 point != nullptr ? point : "?",
                 static_cast<unsigned long long>(g_n.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_bytes.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_bucket[0].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_bucket[1].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_bucket[2].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_nextPlausible.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_tl.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_rf.load(std::memory_order_relaxed)));
}

static void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Dump("atexit"); });
    }
}

void NoteWalkBreak(RegionInfo* region, uintptr_t holeStart, uintptr_t allocPtr, BaseObject* prevObj, size_t prevSize)
{
    if (!kArmed || region == nullptr || holeStart >= allocPtr) {
        return;
    }
    EnsureAtexit();
    uintptr_t pos = holeStart;
    while (pos + 8 <= allocPtr) {
        uint64_t w = *reinterpret_cast<uint64_t*>(pos);
        if (w != 0) {
            break;
        }
        pos += 8;
    }
    size_t len = pos - holeStart;
    if (len < 8) {
        return;
    }
    uint64_t n = g_n.fetch_add(1, std::memory_order_relaxed) + 1;
    g_bytes.fetch_add(len, std::memory_order_relaxed);
    RegionInfo::RegionType t = region->GetRegionType();
    if (t == RegionInfo::RegionType::THREAD_LOCAL_REGION) {
        g_tl.fetch_add(1, std::memory_order_relaxed);
    } else if (t == RegionInfo::RegionType::RECENT_FULL_REGION) {
        g_rf.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t nextWord = (pos + 8 <= allocPtr) ? *reinterpret_cast<uint64_t*>(pos) : 0;
    unsigned b = Classify(holeStart, pos, region->GetRegionStart(), nextWord);
    g_bucket[b].fetch_add(1, std::memory_order_relaxed);
    bool nextOk = false;
    if (pos < allocPtr) {
        nextOk = PlausibleManagedObjectGate("holewho-next", from_region_addr(pos));
        if (nextOk) {
            g_nextPlausible.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (n > kSampleCap) {
        return;
    }
    uintptr_t off = holeStart - region->GetRegionStart();
    const char* prevName = "?";
    if (prevObj != nullptr && PlausibleManagedObjectGate("holewho-prev", prevObj)) {
        TypeInfo* ti = prevObj->GetTypeInfo();
        if (ti != nullptr && ti->GetName() != nullptr) {
            prevName = ti->GetName();
        }
    }
    (void)prevSize;
    std::fprintf(stderr,
                 "[GCV2][holewho] sample n=%llu off=%zu len=%zu type=%u next_plausible=%u bucket=%u prev=%s "
                 "filler=%u\n",
                 static_cast<unsigned long long>(n), static_cast<size_t>(off), len,
                 static_cast<unsigned>(t), static_cast<unsigned>(nextOk), b, prevName,
                 static_cast<unsigned>(HeapFiller::Enabled()));
}

} // namespace HoleWhoDiag
} // namespace MapleRuntime
