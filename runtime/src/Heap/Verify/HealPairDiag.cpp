#include "Heap/Verify/HealPairDiag.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"

namespace MapleRuntime {
namespace HealPairDiag {
// cjpmnull2: live NoteZeroWrite / NoteCrashWhoZero arm. Compile-time gate, no
// new MRT_GCV2_* reader.
constexpr bool kWhoZero = true;
constexpr size_t kZeroRing = 256;

struct ZeroRec {
    const void* slot;
    uintptr_t oldRaw;
    uint16_t site;
    uint16_t seq;
};

static std::atomic<size_t> g_zeroN{ 0 };
static std::atomic<size_t> g_zeroBySite[128];
static ZeroRec g_ring[kZeroRing];
static std::atomic<bool> g_atexit{ false };

void Report(const char* point)
{
    if (!kWhoZero) {
        return;
    }
    std::fprintf(stderr, "[GCV2][whozero] point=%s n=%zu",
                 point != nullptr ? point : "?", g_zeroN.load(std::memory_order_relaxed));
    for (size_t i = 0; i < 128; ++i) {
        size_t c = g_zeroBySite[i].load(std::memory_order_relaxed);
        if (c != 0) {
            std::fprintf(stderr, " site%zu=%zu", i, c);
        }
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

void NoteZeroWrite(const void* slot, uintptr_t oldRaw, uintptr_t newRaw, uint16_t site)
{
    if (!kWhoZero || newRaw != 0 || oldRaw == 0) {
        return;
    }
    size_t n = g_zeroN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (site < 128) {
        g_zeroBySite[site].fetch_add(1, std::memory_order_relaxed);
    }
    ZeroRec rec{ slot, oldRaw, site, static_cast<uint16_t>(n) };
    g_ring[(n - 1) % kZeroRing] = rec;
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
    if (n <= 8 || (n & (n - 1)) == 0) {
        char line[256];
        int k = std::snprintf(line, sizeof(line),
                              "[GCV2][whozero] n=%zu slot=%p old=%#zx site=%u\n",
                              n, slot, oldRaw, static_cast<unsigned>(site));
        if (k > 0) {
            (void)write(STDERR_FILENO, line, static_cast<size_t>(k));
        }
    }
}

void NoteCrashWhoZero(uintptr_t r13, uintptr_t rcx, uintptr_t rsi, uintptr_t rbx, uintptr_t r12)
{
    if (!kWhoZero) {
        return;
    }
    const uintptr_t cands[] = { r13, rcx, rsi, rbx, r12 };
    const char* names[] = { "r13", "rcx", "rsi", "rbx", "r12" };
    size_t n = g_zeroN.load(std::memory_order_relaxed);
    size_t scan = n < kZeroRing ? n : kZeroRing;
    int hits = 0;
    for (size_t i = 0; i < 5; ++i) {
        if (cands[i] == 0) {
            continue;
        }
        for (size_t k = 0; k < scan; ++k) {
            const ZeroRec& rec = g_ring[(n - 1 - k) % kZeroRing];
            if (rec.slot == nullptr) {
                continue;
            }
            uintptr_t slot = reinterpret_cast<uintptr_t>(rec.slot);
            if (slot == cands[i] || (cands[i] >= slot && cands[i] < slot + 8)) {
                char line[320];
                int m = std::snprintf(line, sizeof(line),
                                      "[GCV2][whozero] CRASH-HIT reg=%s val=%#zx slot=%p old=%#zx "
                                      "site=%u seq=%u n=%zu\n",
                                      names[i], cands[i], rec.slot, rec.oldRaw,
                                      static_cast<unsigned>(rec.site),
                                      static_cast<unsigned>(rec.seq), n);
                if (m > 0) {
                    (void)write(STDERR_FILENO, line, static_cast<size_t>(m));
                }
                ++hits;
            }
        }
    }
    char line[192];
    int m = std::snprintf(line, sizeof(line),
                          "[GCV2][whozero] CRASH-SCAN hits=%d ring=%zu n=%zu "
                          "r13=%#zx rcx=%#zx rsi=%#zx rbx=%#zx r12=%#zx\n",
                          hits, scan, n, r13, rcx, rsi, rbx, r12);
    if (m > 0) {
        (void)write(STDERR_FILENO, line, static_cast<size_t>(m));
    }
}

} // namespace HealPairDiag
} // namespace MapleRuntime
