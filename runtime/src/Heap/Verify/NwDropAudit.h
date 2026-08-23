#ifndef MRT_NWDROP_AUDIT_H
#define MRT_NWDROP_AUDIT_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/BaseObject.h"

namespace MapleRuntime {
namespace NwDropAudit {

enum Drop : uint8_t {
    kNotHeap = 0,
    kWeak,
    kDeadHolder,
    kRetained,
    kStaleOldTag,
    kResolveNull,
    kFindToMiss,
    kNoOrigin,
    kBadTarget,
    kAdmit,
    kCount,
};

inline bool Enabled()
{
    static const bool on = []() {
        const char* v = std::getenv("NWDROP_AUDIT");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

inline std::atomic<uint64_t>* Counters()
{
    static std::atomic<uint64_t> c[kCount] = {};
    return c;
}

inline void Note(Drop d)
{
    if (Enabled()) {
        Counters()[d].fetch_add(1, std::memory_order_relaxed);
    }
}

inline std::atomic<uint64_t>& SatbNull()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<uint64_t>& SatbHdr0()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<uint64_t>& SatbHdrNz()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}
inline std::atomic<uint64_t>& SatbSeen()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

struct HdrSample {
    uintptr_t obj;
    uint64_t word0;
};
inline HdrSample* Ring()
{
    static HdrSample r[5] = {};
    return r;
}
inline std::atomic<uint32_t>& RingI()
{
    static std::atomic<uint32_t> i{ 0 };
    return i;
}

inline void NoteSatbObj(BaseObject* obj)
{
    if (!Enabled()) {
        return;
    }
    SatbSeen().fetch_add(1, std::memory_order_relaxed);
    if (obj == nullptr) {
        SatbNull().fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t word0 = 0;
    std::memcpy(&word0, obj, sizeof(word0));
    if (word0 == 0) {
        SatbHdr0().fetch_add(1, std::memory_order_relaxed);
    } else {
        SatbHdrNz().fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t i = RingI().fetch_add(1, std::memory_order_relaxed) % 5u;
    Ring()[i] = { reinterpret_cast<uintptr_t>(obj), word0 };
}

inline void Report(const char* point)
{
    if (!Enabled()) {
        return;
    }
    auto* c = Counters();
    std::fprintf(stderr,
                 "[NWDROP] point=%s notHeap=%llu weak=%llu deadHolder=%llu retained=%llu "
                 "staleOldTag=%llu resolveNull=%llu findToMiss=%llu noOrigin=%llu badTarget=%llu "
                 "admit=%llu satb_seen=%llu satb_null=%llu satb_hdr0=%llu satb_hdr_nz=%llu\n",
                 point == nullptr ? "?" : point,
                 static_cast<unsigned long long>(c[kNotHeap].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kWeak].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kDeadHolder].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kRetained].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kStaleOldTag].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kResolveNull].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kFindToMiss].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kNoOrigin].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kBadTarget].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(c[kAdmit].load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(SatbSeen().load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(SatbNull().load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(SatbHdr0().load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(SatbHdrNz().load(std::memory_order_relaxed)));
    for (uint32_t i = 0; i < 5; ++i) {
        HdrSample s = Ring()[i];
        if (s.obj == 0 && s.word0 == 0) {
            continue;
        }
        std::fprintf(stderr, "[NWDROP][satb-hdr] i=%u obj=%#llx word0=%#llx\n", i,
                     static_cast<unsigned long long>(s.obj), static_cast<unsigned long long>(s.word0));
    }
    std::fflush(stderr);
}

inline void EnsureAtexit()
{
    if (!Enabled()) {
        return;
    }
    static std::atomic<bool> once{ false };
    bool expected = false;
    if (once.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

} // namespace NwDropAudit
} // namespace MapleRuntime

#endif
