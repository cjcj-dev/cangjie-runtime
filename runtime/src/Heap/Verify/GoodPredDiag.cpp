// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/GoodPredDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/ColourPredicates.h"

namespace MapleRuntime {
namespace GoodPredDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

uint8_t ReadMode()
{
    const bool audit = EnvIsOne("MRT_GCV2_LOADGOOD_AUDIT");
    const bool zgc = EnvIsOne("MRT_GCV2_ZGC_LOADGOOD");
    if (audit) {
        return kAudit;
    }
    return zgc ? kZgc : kLegacy;
}

std::atomic<uint64_t> g_reads{ 0 };
std::atomic<uint64_t> g_legacyGood{ 0 };
std::atomic<uint64_t> g_zgcGood{ 0 };
std::atomic<uint64_t> g_diverge{ 0 };
// legacy said good, ZGC said bad -- the direction the tagged term produces.
std::atomic<uint64_t> g_divLegacyGood{ 0 };
// ZGC said good, legacy said bad -- must stay 0; ZGC is the strictly stronger predicate.
std::atomic<uint64_t> g_divZgcGood{ 0 };
// Why they disagreed, on the divergent values only.
std::atomic<uint64_t> g_causeTagged{ 0 };
std::atomic<uint64_t> g_causeStaleRemap{ 0 };
std::atomic<uint64_t> g_causeMultiRemap{ 0 };
std::atomic<uint64_t> g_causeNoRemap{ 0 };
// Answers this process actually changed: divergent AND the ZGC answer was the one returned.
std::atomic<uint64_t> g_applied{ 0 };
std::atomic<uint64_t> g_samples{ 0 };

constexpr uint64_t kSampleCap = 8;

void ReportRaw(const char* why)
{
    const unsigned long long reads = g_reads.load(std::memory_order_relaxed);
    const unsigned long long diverge = g_diverge.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[GCV2][goodpred][census] why=%s mode=%u apply=%u "
                 "reads=%llu legacy_good=%llu zgc_good=%llu "
                 "diverge=%llu div_legacy_good_zgc_bad=%llu div_zgc_good_legacy_bad=%llu "
                 "cause_tagged=%llu cause_stale_remap=%llu cause_multi_remap=%llu cause_no_remap=%llu "
                 "applied=%llu load_bad_mask=%#lx diverge_ppm=%llu\n",
                 why == nullptr ? "?" : why, static_cast<unsigned>(g_mode),
                 static_cast<unsigned>(g_applyZgc ? 1 : 0), reads,
                 static_cast<unsigned long long>(g_legacyGood.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_zgcGood.load(std::memory_order_relaxed)), diverge,
                 static_cast<unsigned long long>(g_divLegacyGood.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_divZgcGood.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_causeTagged.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_causeStaleRemap.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_causeMultiRemap.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_causeNoRemap.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_applied.load(std::memory_order_relaxed)),
                 static_cast<unsigned long>(::g_cjLoadBadMask),
                 reads == 0 ? 0ull : (diverge * 1000000ull) / reads);
    std::fflush(stderr);
}

void InstallOnce()
{
    static std::atomic<bool> installed{ false };
    bool expected = false;
    if (installed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { ReportRaw("atexit"); });
    }
}

} // namespace

uint8_t g_mode = ReadMode();
bool g_applyZgc = EnvIsOne("MRT_GCV2_ZGC_LOADGOOD");

bool NoteAudit(uintptr_t value, bool legacy, bool zgc)
{
    InstallOnce();
    g_reads.fetch_add(1, std::memory_order_relaxed);
    if (legacy) {
        g_legacyGood.fetch_add(1, std::memory_order_relaxed);
    }
    if (zgc) {
        g_zgcGood.fetch_add(1, std::memory_order_relaxed);
    }
    if (legacy != zgc) {
        g_diverge.fetch_add(1, std::memory_order_relaxed);
        if (legacy) {
            g_divLegacyGood.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_divZgcGood.fetch_add(1, std::memory_order_relaxed);
        }
        const uintptr_t mask = static_cast<uintptr_t>(::g_cjLoadBadMask);
        const uintptr_t remapBits = value & REMAP_COLOUR_MASK;
        const uintptr_t current = ColourPredicates::current_remapped(mask);
        if ((value & TAGGED_BITS_MASK) != 0) {
            g_causeTagged.fetch_add(1, std::memory_order_relaxed);
        }
        if ((remapBits & ~current) != 0) {
            g_causeStaleRemap.fetch_add(1, std::memory_order_relaxed);
        }
        if ((remapBits & (remapBits - 1)) != 0) {
            g_causeMultiRemap.fetch_add(1, std::memory_order_relaxed);
        }
        if (remapBits == 0) {
            g_causeNoRemap.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_applyZgc) {
            g_applied.fetch_add(1, std::memory_order_relaxed);
        }
        if (g_samples.fetch_add(1, std::memory_order_relaxed) < kSampleCap) {
            std::fprintf(stderr,
                         "[GCV2][goodpred][sample] value=%#lx legacy=%u zgc=%u load_bad_mask=%#lx "
                         "tagged=%u remap_bits=%#lx current_remap=%#lx\n",
                         static_cast<unsigned long>(value), static_cast<unsigned>(legacy ? 1 : 0),
                         static_cast<unsigned>(zgc ? 1 : 0), static_cast<unsigned long>(mask),
                         static_cast<unsigned>((value & TAGGED_BITS_MASK) != 0 ? 1 : 0),
                         static_cast<unsigned long>(remapBits), static_cast<unsigned long>(current));
            std::fflush(stderr);
        }
    }
    return g_applyZgc ? zgc : legacy;
}

void Report(const char* why) { ReportRaw(why); }

} // namespace GoodPredDiag
} // namespace MapleRuntime
