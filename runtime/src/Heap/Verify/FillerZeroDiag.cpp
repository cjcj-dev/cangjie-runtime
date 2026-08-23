#include "Heap/Verify/FillerZeroDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace FillerZeroDiag {

constexpr bool kArmed = true;
constexpr unsigned kSites = static_cast<unsigned>(Site::N);
constexpr unsigned kBuckets = 21;

static const char* kSiteName[kSites] = {
    "slot_extra", "clear_units", "take_garbage", "take_inactive", "dirty_take",
    "released_pre", "compact", "compact_partial", "route_reserve",
};

struct Row {
    std::atomic<uint64_t> n{ 0 };
    std::atomic<uint64_t> bytes{ 0 };
    std::atomic<uint64_t> denseN{ 0 };
    std::atomic<uint64_t> denseBytes{ 0 };
    std::atomic<uint64_t> bucket[kBuckets];
};

static Row g_row[kSites];
static std::atomic<bool> g_atexit{ false };

static unsigned BucketOf(size_t size)
{
    if (size == 0) {
        return 0;
    }
    unsigned lg = 63u - static_cast<unsigned>(__builtin_clzll(static_cast<unsigned long long>(size)));
    if (lg >= kBuckets) {
        return kBuckets - 1;
    }
    return lg;
}

static bool IsDenseWalkType(RegionInfo::RegionType t)
{
    return t == RegionInfo::RegionType::THREAD_LOCAL_REGION ||
        t == RegionInfo::RegionType::RECENT_FULL_REGION || t == RegionInfo::RegionType::FROM_REGION ||
        t == RegionInfo::RegionType::LONE_FROM_REGION;
}

void Dump(const char* point)
{
    if (!kArmed) {
        return;
    }
    std::fprintf(stderr, "[GCV2][fillerzero] %s\n", point != nullptr ? point : "?");
    for (unsigned s = 0; s < kSites; ++s) {
        const uint64_t n = g_row[s].n.load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[GCV2][fillerzero] site=%s n=%llu bytes=%llu dense_n=%llu dense_bytes=%llu buckets=",
                     kSiteName[s], static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(g_row[s].bytes.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_row[s].denseN.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_row[s].denseBytes.load(std::memory_order_relaxed)));
        for (unsigned b = 0; b < kBuckets; ++b) {
            const uint64_t c = g_row[s].bucket[b].load(std::memory_order_relaxed);
            if (c != 0) {
                std::fprintf(stderr, " 2^%u=%llu", b, static_cast<unsigned long long>(c));
            }
        }
        std::fprintf(stderr, "\n");
    }
}

static void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Dump("atexit"); });
    }
}

void Note(Site site, uintptr_t start, size_t size)
{
    if (!kArmed) {
        return;
    }
    EnsureAtexit();
    const unsigned s = static_cast<unsigned>(site);
    if (s >= kSites) {
        return;
    }
    g_row[s].n.fetch_add(1, std::memory_order_relaxed);
    g_row[s].bytes.fetch_add(size, std::memory_order_relaxed);
    g_row[s].bucket[BucketOf(size)].fetch_add(1, std::memory_order_relaxed);
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(start);
    if (region != nullptr && IsDenseWalkType(region->GetRegionType())) {
        g_row[s].denseN.fetch_add(1, std::memory_order_relaxed);
        g_row[s].denseBytes.fetch_add(size, std::memory_order_relaxed);
    }
}

} // namespace FillerZeroDiag
} // namespace MapleRuntime
