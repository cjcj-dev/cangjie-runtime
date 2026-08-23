// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_REGION_LIFE_CLOCK_H
#define MRT_REGION_LIFE_CLOCK_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace MapleRuntime {

// ZGC binds page identity at reset (zPage.cpp:90-93), compares it before
// consuming page state (zPage.inline.hpp:176-186), and advances the generation
// clock at mark start (zGeneration.cpp:870-872). Ours is per-region rather than
// per-generation: zero is reserved for an unstamped carrier, InitRegionInfo
// publishes the first usable value, and wraparound is forbidden.
using RegionLifeId = uint64_t;

class RegionLifeClock {
public:
    enum class Carrier : uint8_t {
        ROUTE_INFO = 0,
        ROUTE_STATE,
        GHOST,
        MARK_SNAPSHOT,
        RETAINED_COPY,
        ARMED_ENTRY,
        RETIRED_ENTRY,
        RECEIPT,
        COUNT,
    };

    struct Snapshot {
        uint64_t published;
        uint64_t stamped;
        uint64_t reads;
        uint64_t current;
        uint64_t stale;
        uint64_t missing;
        uint64_t untracked;
        uint64_t zeroAcrossBoundary;
        uint64_t capWouldOverflow;
    };

    static bool AuditEnabled()
    {
        static const bool enabled = EnvIsOne("LIFECLOCK_AUDIT");
        if (enabled) {
            EnsureAtexit();
        }
        return enabled;
    }

    static bool EnforceEnabled()
    {
        static const bool enabled = EnvIsOne("CJRT_LIFECLOCK_ENFORCE");
        return enabled;
    }

    static bool Active() { return AuditEnabled() || EnforceEnabled(); }

    static void Publish(Carrier carrier, RegionLifeId stamp)
    {
        if (!Active()) {
            return;
        }
        Counter& c = At(carrier);
        c.published.fetch_add(1, std::memory_order_relaxed);
        if (stamp != 0) {
            c.stamped.fetch_add(1, std::memory_order_relaxed);
        } else {
            c.missing.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Audit-only runs preserve the legacy decision. Enforcement rejects a
    // missing or stale carrier before its payload is consumed.
    static bool Validate(Carrier carrier, RegionLifeId stamp, RegionLifeId current)
    {
        if (!Active()) {
            return true;
        }
        Counter& c = At(carrier);
        c.reads.fetch_add(1, std::memory_order_relaxed);
        if (stamp == 0) {
            c.missing.fetch_add(1, std::memory_order_relaxed);
            return !EnforceEnabled();
        }
        if (stamp == current) {
            c.current.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        c.stale.fetch_add(1, std::memory_order_relaxed);
        return !EnforceEnabled();
    }

    static void NoteUntracked(Carrier carrier)
    {
        if (Active()) {
            At(carrier).untracked.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void NoteZeroAcrossBoundary(Carrier carrier, bool carrierAlive, RegionLifeId stamp)
    {
        if (Active() && carrierAlive && stamp == 0) {
            At(carrier).zeroAcrossBoundary.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void NoteCapWouldOverflow(Carrier carrier)
    {
        if (Active()) {
            At(carrier).capWouldOverflow.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static Snapshot GetSnapshot(Carrier carrier)
    {
        Counter& c = At(carrier);
        return { c.published.load(std::memory_order_relaxed), c.stamped.load(std::memory_order_relaxed),
                 c.reads.load(std::memory_order_relaxed), c.current.load(std::memory_order_relaxed),
                 c.stale.load(std::memory_order_relaxed), c.missing.load(std::memory_order_relaxed),
                 c.untracked.load(std::memory_order_relaxed), c.zeroAcrossBoundary.load(std::memory_order_relaxed),
                 c.capWouldOverflow.load(std::memory_order_relaxed) };
    }

    static void Report(const char* point)
    {
        if (!AuditEnabled()) {
            return;
        }
        std::fprintf(stderr, "[LIFECLOCK][matrix] point=%s enforce=%d\n", point == nullptr ? "?" : point,
                     EnforceEnabled() ? 1 : 0);
        for (size_t i = 0; i < static_cast<size_t>(Carrier::COUNT); ++i) {
            const Carrier carrier = static_cast<Carrier>(i);
            const Snapshot s = GetSnapshot(carrier);
            std::fprintf(stderr,
                         "[LIFECLOCK][carrier] name=%s published=%llu stamped=%llu reads=%llu "
                         "current=%llu stale=%llu missing=%llu untracked=%llu "
                         "zero_across_boundary=%llu cap_would_overflow=%llu\n",
                         Name(carrier), static_cast<unsigned long long>(s.published),
                         static_cast<unsigned long long>(s.stamped), static_cast<unsigned long long>(s.reads),
                         static_cast<unsigned long long>(s.current), static_cast<unsigned long long>(s.stale),
                         static_cast<unsigned long long>(s.missing), static_cast<unsigned long long>(s.untracked),
                         static_cast<unsigned long long>(s.zeroAcrossBoundary),
                         static_cast<unsigned long long>(s.capWouldOverflow));
        }
        std::fflush(stderr);
    }

private:
    struct Counter {
        std::atomic<uint64_t> published{ 0 };
        std::atomic<uint64_t> stamped{ 0 };
        std::atomic<uint64_t> reads{ 0 };
        std::atomic<uint64_t> current{ 0 };
        std::atomic<uint64_t> stale{ 0 };
        std::atomic<uint64_t> missing{ 0 };
        std::atomic<uint64_t> untracked{ 0 };
        std::atomic<uint64_t> zeroAcrossBoundary{ 0 };
        std::atomic<uint64_t> capWouldOverflow{ 0 };
    };

    static bool EnvIsOne(const char* name)
    {
        const char* value = std::getenv(name);
        return value != nullptr && std::strcmp(value, "1") == 0;
    }

    static std::array<Counter, static_cast<size_t>(Carrier::COUNT)>& Counters()
    {
        static std::array<Counter, static_cast<size_t>(Carrier::COUNT)> counters;
        return counters;
    }

    static std::atomic<bool>& AtexitInstalled()
    {
        static std::atomic<bool> installed{ false };
        return installed;
    }

    static Counter& At(Carrier carrier) { return Counters()[static_cast<size_t>(carrier)]; }

    static const char* Name(Carrier carrier)
    {
        static constexpr const char* names[] = { "route_info", "route_state", "ghost", "mark_snapshot",
                                                  "retained_copy", "armed_entry", "retired_entry", "receipt" };
        const size_t idx = static_cast<size_t>(carrier);
        return idx < static_cast<size_t>(Carrier::COUNT) ? names[idx] : "unknown";
    }

    static void EnsureAtexit()
    {
        bool expected = false;
        if (AtexitInstalled().compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::atexit([]() { Report("atexit"); });
        }
    }
};

} // namespace MapleRuntime

#endif // MRT_REGION_LIFE_CLOCK_H
