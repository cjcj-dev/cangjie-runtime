#include "Heap/Verify/NoTracedDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "securec.h"

namespace MapleRuntime {
namespace NoTracedDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_NOTRACED")) {
            return true;
        }
        return DiagGate::TokenOn("notraced");
    }();
    return on;
}

// Cap: young-only still high volume; 256k rows × ~24B ≈ 6MB.
constexpr size_t kCap = 1u << 18;

struct Row {
    uintptr_t obj;   // current (possibly remapped) object address
    uintptr_t start; // region start at record time (secondary)
    uint32_t gc;
    uint16_t phase;
    uint16_t pad0;
    uint32_t seq;
};

Row g_rows[kCap];
std::atomic<uint32_t> g_next{ 0 };
std::atomic<size_t> g_total{ 0 };
std::atomic<size_t> g_wrap{ 0 };
std::atomic<size_t> g_youngSkip{ 0 };
std::atomic<size_t> g_remap{ 0 };
std::atomic<size_t> g_copySkip{ 0 }; // copy ranges with no page hit
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

// Page filter (4KiB): only scan ring when from-object page was recorded.
constexpr size_t kPageBits = 1u << 20;
constexpr uintptr_t kPageShift = 12;
uint8_t g_pageBits[kPageBits / 8] = {};

inline void MarkPage(uintptr_t addr)
{
    if (addr == 0) {
        return;
    }
    size_t bit = (addr >> kPageShift) & (kPageBits - 1);
    g_pageBits[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
}

inline bool PageMarked(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    size_t bit = (addr >> kPageShift) & (kPageBits - 1);
    return (g_pageBits[bit / 8] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
}

inline bool RangeMaybeMarked(uintptr_t from, uintptr_t size)
{
    if (from == 0 || size == 0) {
        return false;
    }
    uintptr_t end = from + size - 1;
    uintptr_t p = from & ~((uintptr_t(1) << kPageShift) - 1);
    uintptr_t pEnd = end & ~((uintptr_t(1) << kPageShift) - 1);
    for (;;) {
        if (PageMarked(p)) {
            return true;
        }
        if (p >= pEnd) {
            return false;
        }
        p += (uintptr_t(1) << kPageShift);
    }
}

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
        LOG(RTLOG_ERROR, "[GCV2][notraced] health probe_live=1 env=MRT_GCV2_NOTRACED=1 young_only=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

} // namespace

bool Enabled() { return GateOn(); }

void NoteTrace(BaseObject* obj)
{
    if (LIKELY(!GateOn())) {
        return;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return;
    }
    HealthOnce();
    EnsureAtexit();

    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    if (region == nullptr || !region->IsYoungRegion()) {
        g_youngSkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kCap) {
        g_wrap.fetch_add(1, std::memory_order_relaxed);
    }
    size_t total = g_total.fetch_add(1, std::memory_order_relaxed) + 1;
    Row& row = g_rows[idx % kCap];
    uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    row.obj = addr;
    row.start = region->GetRegionStart();
    row.gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    row.phase = static_cast<uint16_t>(phase);
    row.pad0 = 0;
    row.seq = static_cast<uint32_t>(total);
    MarkPage(addr);
}

void NoteCopy(const void* fromAddr, const void* toAddr, size_t size, uint32_t done)
{
    if (!GateOn() || done == 0 || size == 0 || fromAddr == nullptr || toAddr == nullptr) {
        return;
    }
    uintptr_t from = reinterpret_cast<uintptr_t>(fromAddr);
    uintptr_t to = reinterpret_cast<uintptr_t>(toAddr);
    uintptr_t sz = static_cast<uintptr_t>(size);

    size_t total = g_total.load(std::memory_order_acquire);
    if (total == 0) {
        return;
    }
    // Fast path: most copies never hit a recorded young-trace page.
    if (!RangeMaybeMarked(from, sz)) {
        g_copySkip.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint32_t n = static_cast<uint32_t>(total < kCap ? total : kCap);
    uint32_t next = g_next.load(std::memory_order_acquire);
    uint32_t base = (total < kCap) ? 0 : (next % kCap);
    for (uint32_t i = 0; i < n; ++i) {
        Row& row = g_rows[(base + i) % kCap];
        uintptr_t o = row.obj;
        if (o >= from && o < from + sz) {
            uintptr_t ns = to + (o - from);
            row.obj = ns;
            MarkPage(ns);
            g_remap.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void NoteCrashJoin(uintptr_t holderCrash, uintptr_t holderCas)
{
    if (!GateOn()) {
        return;
    }
    size_t total = g_total.load(std::memory_order_acquire);
    uint32_t n = static_cast<uint32_t>(total < kCap ? total : kCap);
    uint32_t next = g_next.load(std::memory_order_acquire);
    uint32_t base = (total < kCap) ? 0 : (next % kCap);
    size_t wrap = g_wrap.load(std::memory_order_relaxed);
    size_t youngSkip = g_youngSkip.load(std::memory_order_relaxed);
    size_t remap = g_remap.load(std::memory_order_relaxed);
    size_t copySkip = g_copySkip.load(std::memory_order_relaxed);

    auto matchOne = [&](uintptr_t h, uint32_t& hitObj, uint32_t& hitStart, uint32_t& lastGc,
                        uint16_t& lastPhase, uint32_t& lastSeq) {
        if (h == 0) {
            return;
        }
        RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(h);
        uintptr_t hStart = (hr != nullptr) ? hr->GetRegionStart() : 0;
        for (uint32_t i = 0; i < n; ++i) {
            const Row& row = g_rows[(base + i) % kCap];
            if (row.obj == h) {
                ++hitObj;
                lastGc = row.gc;
                lastPhase = row.phase;
                lastSeq = row.seq;
            } else if (hStart != 0 && row.start == hStart && row.obj != 0) {
                ++hitStart;
            }
        }
    };

    uint32_t hitCrash = 0;
    uint32_t hitCrashStart = 0;
    uint32_t hitCas = 0;
    uint32_t hitCasStart = 0;
    uint32_t lastGc = 0;
    uint16_t lastPhase = 0;
    uint32_t lastSeq = 0;
    matchOne(holderCrash, hitCrash, hitCrashStart, lastGc, lastPhase, lastSeq);
    matchOne(holderCas, hitCas, hitCasStart, lastGc, lastPhase, lastSeq);

    uint32_t hitObj = hitCrash + hitCas;
    const char* verdict = "丙_no_record";
    if (total == 0) {
        verdict = "丙_trace_empty";
    } else if (hitObj > 0) {
        verdict = "乙_traced";
    } else {
        verdict = "甲_not_traced";
    }

    char line[704];
    int ln = sprintf_s(line, sizeof(line),
                       "[GCV2][notraced] crash_join holderCrash=%#zx holderCas=%#zx "
                       "traceN=%zu wrap=%zu youngSkip=%zu remap=%zu copySkip=%zu "
                       "hitCrash=%u hitCas=%u hitObj=%u hitStartC=%u hitStartA=%u "
                       "lastGc=%u lastPhase=%u lastSeq=%u verdict=%s\n",
                       holderCrash, holderCas, total, wrap, youngSkip, remap, copySkip, hitCrash,
                       hitCas, hitObj, hitCrashStart, hitCasStart, lastGc,
                       static_cast<unsigned>(lastPhase), lastSeq, verdict);
    if (ln > 0) {
        WriteLine(line, static_cast<size_t>(ln));
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][notraced] point=%s traceN=%zu wrap=%zu youngSkip=%zu remap=%zu "
                      "copySkip=%zu cap=%zu env=MRT_GCV2_NOTRACED=1\n",
                      point == nullptr ? "?" : point, g_total.load(std::memory_order_relaxed),
                      g_wrap.load(std::memory_order_relaxed),
                      g_youngSkip.load(std::memory_order_relaxed),
                      g_remap.load(std::memory_order_relaxed),
                      g_copySkip.load(std::memory_order_relaxed), kCap);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

} // namespace NoTracedDiag
} // namespace MapleRuntime
