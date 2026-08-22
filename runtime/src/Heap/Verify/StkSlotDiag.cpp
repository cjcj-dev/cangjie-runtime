#include "Heap/Verify/StkSlotDiag.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "Base/Log.h"

namespace MapleRuntime {
namespace StkSlotDiag {

static constexpr size_t kMagicLen = 24;
static constexpr size_t kWatchCap = 65536;
static constexpr size_t kIdentCap = 32;
static constexpr size_t kMapCap = 256;
static constexpr size_t kScanWords = 512;
static constexpr uintptr_t kAddrMask = (static_cast<uintptr_t>(1) << 48) - 1u;
static const char kMagic[kMagicLen + 1] = "x86_64-unknown-linux-gnu";

struct Watch {
    uintptr_t addr;
    uint32_t nElems;
    uint32_t pad;
    uint64_t seq;
};

static Watch g_watch[kWatchCap];
static std::atomic<uint64_t> g_watchN{ 0 };
static std::atomic<uint64_t> g_identN{ 0 };
static uintptr_t g_ident[kIdentCap];
static std::atomic<uint64_t> g_zeroHits{ 0 };
static std::atomic<uint64_t> g_rawHits{ 0 };
static std::atomic<uint64_t> g_mapHits{ 0 };
static std::atomic<uint64_t> g_h2Hits{ 0 };
static std::atomic<uint64_t> g_frames{ 0 };
static std::atomic<uint64_t> g_logs{ 0 };
static std::atomic<bool> g_atexit{ false };

thread_local intptr_t t_bias[kMapCap];
thread_local uintptr_t t_slotVal[kMapCap];
thread_local int t_nMap = 0;
thread_local int t_nReg = 0;
thread_local int t_reg[kMapCap];
thread_local uintptr_t t_regVal[kMapCap];

static void EnsureAtexit();

static uintptr_t Plain(uintptr_t w) { return w & kAddrMask; }

static bool MatchesIdent(uintptr_t plain)
{
    const uint64_t n = g_identN.load(std::memory_order_acquire);
    const uint64_t lim = n < kIdentCap ? n : kIdentCap;
    for (uint64_t i = 0; i < lim; ++i) {
        const uintptr_t t = g_ident[i];
        if (t == 0) {
            continue;
        }
        if (plain == t || (plain > t && plain < t + 40)) {
            return true;
        }
    }
    return false;
}

static bool PayloadIsMagic(uintptr_t obj, uint32_t nElems)
{
    const char* p = reinterpret_cast<const char*>(obj + 16);
    const size_t n = nElems;
    if (n < kMagicLen) {
        return false;
    }
    const size_t lim = n - kMagicLen;
    for (size_t i = 0; i <= lim && i < 2048; ++i) {
        if (std::memcmp(p + i, kMagic, kMagicLen) == 0) {
            return true;
        }
    }
    return false;
}

static void Identify()
{
    static std::atomic<bool> sampled{ false };
    const uint64_t n = g_watchN.load(std::memory_order_acquire);
    const uint64_t lim = n < kWatchCap ? n : kWatchCap;
    bool expect = false;
    if (lim > 0 && sampled.compare_exchange_strong(expect, true, std::memory_order_relaxed)) {
        const uint64_t show = lim < 8 ? lim : 8;
        for (uint64_t i = 0; i < show; ++i) {
            const uintptr_t a = g_watch[i].addr;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(a);
            LOG(RTLOG_ERROR,
                "[STKSLOT] sample i=%llu obj=%p b16=%02x%02x%02x%02x%02x%02x%02x%02x "
                "b0=%02x%02x%02x%02x ascii16=%.24s",
                static_cast<unsigned long long>(i), reinterpret_cast<void*>(a),
                p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
                p[0], p[1], p[2], p[3], reinterpret_cast<const char*>(a + 16));
        }
    }
    for (uint64_t i = 0; i < lim; ++i) {
        const uintptr_t a = g_watch[i].addr;
        if (a == 0) {
            continue;
        }
        if (!PayloadIsMagic(a, g_watch[i].nElems)) {
            continue;
        }
        uint64_t cur = g_identN.load(std::memory_order_relaxed);
        bool seen = false;
        const uint64_t have = cur < kIdentCap ? cur : kIdentCap;
        for (uint64_t j = 0; j < have; ++j) {
            if (g_ident[j] == a) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (cur >= kIdentCap) {
            continue;
        }
        g_ident[cur] = a;
        g_identN.store(cur + 1, std::memory_order_release);
        LOG(RTLOG_ERROR, "[STKSLOT] ident obj=%p seq=%llu payload=triple", reinterpret_cast<void*>(a),
            static_cast<unsigned long long>(g_watch[i].seq));
    }
}

void Dump(const char* point)
{
    if (!kArmed) {
        return;
    }
    std::fprintf(stderr,
                 "[STKSLOT] %s watch=%llu ident=%llu zero=%llu rawHits=%llu mapHits=%llu h2=%llu frames=%llu\n",
                 point != nullptr ? point : "?",
                 static_cast<unsigned long long>(g_watchN.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_identN.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_zeroHits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_rawHits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_mapHits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_h2Hits.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed)));
    const uint64_t n = g_identN.load(std::memory_order_relaxed);
    const uint64_t lim = n < kIdentCap ? n : kIdentCap;
    for (uint64_t i = 0; i < lim; ++i) {
        std::fprintf(stderr, "[STKSLOT] ident[%llu]=%p\n", static_cast<unsigned long long>(i),
                     reinterpret_cast<void*>(g_ident[i]));
    }
}

static void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Dump("atexit"); });
        LOG(RTLOG_ERROR, "[STKSLOT] armed watch_len24+content_match");
    }
}

void NoteAlloc(void* obj, size_t nElems)
{
    if (!kArmed || obj == nullptr || nElems < 24 || nElems > 4096) {
        return;
    }
    EnsureAtexit();
    const uint64_t seq = g_watchN.fetch_add(1, std::memory_order_relaxed);
    g_watch[seq % kWatchCap] = Watch{ reinterpret_cast<uintptr_t>(obj), static_cast<uint32_t>(nElems), 0, seq };
}

void NoteZero(uintptr_t start, size_t size)
{
    if (!kArmed || size == 0) {
        return;
    }
    const uint64_t n = g_identN.load(std::memory_order_acquire);
    if (n == 0) {
        return;
    }
    const uint64_t lim = n < kIdentCap ? n : kIdentCap;
    for (uint64_t i = 0; i < lim; ++i) {
        const uintptr_t t = g_ident[i];
        if (t >= start && t < start + size) {
            const uint64_t z = g_zeroHits.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG(RTLOG_ERROR, "[STKSLOT] filler-zero obj=%p range=[%p,+%zu) n=%llu",
                reinterpret_cast<void*>(t), reinterpret_cast<void*>(start), size,
                static_cast<unsigned long long>(z));
        }
    }
}

void NoteMapSlot(intptr_t bias, BaseObject* root)
{
    if (!kArmed || t_nMap >= static_cast<int>(kMapCap)) {
        return;
    }
    t_bias[t_nMap] = bias;
    t_slotVal[t_nMap] = Plain(reinterpret_cast<uintptr_t>(root));
    ++t_nMap;
}

void NoteMapReg(int reg, BaseObject* root)
{
    if (!kArmed || t_nReg >= static_cast<int>(kMapCap)) {
        return;
    }
    t_reg[t_nReg] = reg;
    t_regVal[t_nReg] = Plain(reinterpret_cast<uintptr_t>(root));
    ++t_nReg;
}

static bool BiasInMap(intptr_t bias)
{
    for (int i = 0; i < t_nMap; ++i) {
        if (t_bias[i] == bias) {
            return true;
        }
    }
    return false;
}

void AfterFrame(uintptr_t startIP, uintptr_t frameIP, uintptr_t fa, bool mapValid, const char* funcName)
{
    if (!kArmed) {
        t_nMap = 0;
        t_nReg = 0;
        return;
    }
    EnsureAtexit();
    Identify();
    g_frames.fetch_add(1, std::memory_order_relaxed);
    const uint64_t nIdent = g_identN.load(std::memory_order_acquire);
    if (nIdent == 0 || fa == 0) {
        t_nMap = 0;
        t_nReg = 0;
        return;
    }

    for (int i = 0; i < t_nMap; ++i) {
        if (MatchesIdent(t_slotVal[i])) {
            g_mapHits.fetch_add(1, std::memory_order_relaxed);
            const uint64_t lg = g_logs.fetch_add(1, std::memory_order_relaxed);
            if (lg < 64) {
                LOG(RTLOG_ERROR,
                    "[STKSLOT] IN-MAP slot bias=%ld val=%p func=%s fa=%p mapValid=%d nMap=%d nReg=%d",
                    static_cast<long>(t_bias[i]), reinterpret_cast<void*>(t_slotVal[i]),
                    funcName != nullptr ? funcName : "?", reinterpret_cast<void*>(fa), static_cast<int>(mapValid),
                    t_nMap, t_nReg);
            }
        }
    }
    for (int i = 0; i < t_nReg; ++i) {
        if (MatchesIdent(t_regVal[i])) {
            g_mapHits.fetch_add(1, std::memory_order_relaxed);
            const uint64_t lg = g_logs.fetch_add(1, std::memory_order_relaxed);
            if (lg < 64) {
                LOG(RTLOG_ERROR, "[STKSLOT] IN-MAP reg=%d val=%p func=%s", t_reg[i],
                    reinterpret_cast<void*>(t_regVal[i]), funcName != nullptr ? funcName : "?");
            }
        }
    }

    uintptr_t lo = fa - kScanWords * sizeof(uintptr_t);
    uintptr_t hi = fa + 16 * sizeof(uintptr_t);
    int rawOnFrame = 0;
    for (uintptr_t p = lo; p < hi; p += sizeof(uintptr_t)) {
        uintptr_t w = *reinterpret_cast<uintptr_t*>(p);
        uintptr_t plain = Plain(w);
        if (!MatchesIdent(plain)) {
            continue;
        }
        ++rawOnFrame;
        g_rawHits.fetch_add(1, std::memory_order_relaxed);
        intptr_t bias = static_cast<intptr_t>(p) - static_cast<intptr_t>(fa);
        const bool inMap = BiasInMap(bias);
        if (!inMap) {
            g_h2Hits.fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t lg = g_logs.fetch_add(1, std::memory_order_relaxed);
        if (lg < 96) {
            LOG(RTLOG_ERROR,
                "[STKSLOT] RAW word=%p plain=%p bias=%ld inMap=%d mapValid=%d nMap=%d nReg=%d "
                "func=%s startIP=%p frameIP=%p fa=%p %s",
                reinterpret_cast<void*>(w), reinterpret_cast<void*>(plain), static_cast<long>(bias),
                static_cast<int>(inMap), static_cast<int>(mapValid), t_nMap, t_nReg,
                funcName != nullptr ? funcName : "?", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP), reinterpret_cast<void*>(fa),
                inMap ? "LISTED" : "H2-UNLISTED");
            if (!inMap && t_nMap > 0 && t_nMap <= 16) {
                for (int i = 0; i < t_nMap; ++i) {
                    LOG(RTLOG_ERROR, "[STKSLOT]   RECORD slot[%d] bias=%ld val=%p", i,
                        static_cast<long>(t_bias[i]), reinterpret_cast<void*>(t_slotVal[i]));
                }
            }
        }
    }
    (void)rawOnFrame;
    t_nMap = 0;
    t_nReg = 0;
}

} // namespace StkSlotDiag
} // namespace MapleRuntime
