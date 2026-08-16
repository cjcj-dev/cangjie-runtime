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

// Per call site, because only kSiteBarrier and kSiteMakeLoadGood can diverge: the mark/store
// sites reach the predicate having already required (value & g_cjMarkBadMask) == 0.
std::atomic<uint64_t> g_reads[kSiteCount];
std::atomic<uint64_t> g_diverge[kSiteCount];
// Population of the construct that can diverge, counted on every read, not only on divergence:
// a zero divergence means something different when the tagged population is zero than when it
// is large.
std::atomic<uint64_t> g_taggedSeen[kSiteCount];

std::atomic<uint64_t> g_legacyGood{ 0 };
std::atomic<uint64_t> g_zgcGood{ 0 };
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

#define GP_LD(c) static_cast<unsigned long long>((c).load(std::memory_order_relaxed))

void ReportRaw(const char* why)
{
    unsigned long long readsAll = 0;
    unsigned long long divAll = 0;
    unsigned long long taggedAll = 0;
    for (unsigned s = 0; s < kSiteCount; ++s) {
        readsAll += GP_LD(g_reads[s]);
        divAll += GP_LD(g_diverge[s]);
        taggedAll += GP_LD(g_taggedSeen[s]);
    }
    const unsigned long long riskReads = GP_LD(g_reads[kSiteBarrier]) + GP_LD(g_reads[kSiteMakeLoadGood]);
    std::fprintf(stderr,
                 "[GCV2][goodpred][census] why=%s mode=%u apply=%u "
                 "reads=%llu reads_at_risk=%llu legacy_good=%llu zgc_good=%llu "
                 "diverge=%llu div_legacy_good_zgc_bad=%llu div_zgc_good_legacy_bad=%llu "
                 "tagged_seen=%llu "
                 "reads_barrier=%llu reads_mlg=%llu reads_markgood=%llu reads_storegood=%llu "
                 "div_barrier=%llu div_mlg=%llu div_markgood=%llu div_storegood=%llu "
                 "tagged_barrier=%llu tagged_mlg=%llu tagged_markgood=%llu tagged_storegood=%llu "
                 "cause_tagged=%llu cause_stale_remap=%llu cause_multi_remap=%llu cause_no_remap=%llu "
                 "applied=%llu load_bad_mask=%#lx\n",
                 why == nullptr ? "?" : why, static_cast<unsigned>(g_mode),
                 static_cast<unsigned>(g_applyZgc ? 1 : 0), readsAll, riskReads, GP_LD(g_legacyGood),
                 GP_LD(g_zgcGood), divAll, GP_LD(g_divLegacyGood), GP_LD(g_divZgcGood), taggedAll,
                 GP_LD(g_reads[kSiteBarrier]), GP_LD(g_reads[kSiteMakeLoadGood]),
                 GP_LD(g_reads[kSiteMarkGood]), GP_LD(g_reads[kSiteStoreGood]),
                 GP_LD(g_diverge[kSiteBarrier]), GP_LD(g_diverge[kSiteMakeLoadGood]),
                 GP_LD(g_diverge[kSiteMarkGood]), GP_LD(g_diverge[kSiteStoreGood]),
                 GP_LD(g_taggedSeen[kSiteBarrier]), GP_LD(g_taggedSeen[kSiteMakeLoadGood]),
                 GP_LD(g_taggedSeen[kSiteMarkGood]), GP_LD(g_taggedSeen[kSiteStoreGood]),
                 GP_LD(g_causeTagged), GP_LD(g_causeStaleRemap), GP_LD(g_causeMultiRemap),
                 GP_LD(g_causeNoRemap), GP_LD(g_applied), static_cast<unsigned long>(::g_cjLoadBadMask));
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

bool NoteAudit(uintptr_t value, bool legacy, bool zgc, uint8_t site)
{
    InstallOnce();
    const unsigned s = site < kSiteCount ? site : 0u;
    g_reads[s].fetch_add(1, std::memory_order_relaxed);
    if ((value & TAGGED_BITS_MASK) != 0) {
        g_taggedSeen[s].fetch_add(1, std::memory_order_relaxed);
    }
    if (legacy) {
        g_legacyGood.fetch_add(1, std::memory_order_relaxed);
    }
    if (zgc) {
        g_zgcGood.fetch_add(1, std::memory_order_relaxed);
    }
    if (legacy != zgc) {
        g_diverge[s].fetch_add(1, std::memory_order_relaxed);
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
                         "[GCV2][goodpred][sample] value=%#lx site=%u legacy=%u zgc=%u "
                         "load_bad_mask=%#lx tagged=%u remap_bits=%#lx current_remap=%#lx\n",
                         static_cast<unsigned long>(value), s, static_cast<unsigned>(legacy ? 1 : 0),
                         static_cast<unsigned>(zgc ? 1 : 0), static_cast<unsigned long>(mask),
                         static_cast<unsigned>((value & TAGGED_BITS_MASK) != 0 ? 1 : 0),
                         static_cast<unsigned long>(remapBits), static_cast<unsigned long>(current));
            std::fflush(stderr);
        }
    }
    return g_applyZgc ? zgc : legacy;
}

bool SelfTestPending()
{
    static const bool armed = EnvIsOne("MRT_GCV2_LOADGOOD_SELFTEST");
    if (!armed) {
        return false;
    }
    static std::atomic<bool> done{ false };
    bool expected = false;
    return done.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

void ReportSelfTest(uintptr_t taggedValue, bool taggedLegacy, bool taggedZgc, uintptr_t plainValue,
                    bool plainLegacy, bool plainZgc)
{
    // Expected: the tagged probe splits the two definitions, the plain probe does not.
    const bool pass = taggedLegacy && !taggedZgc && plainLegacy && plainZgc;
    std::fprintf(stderr,
                 "[GCV2][goodpred][selftest] verdict=%s "
                 "tagged_value=%#lx tagged_legacy=%u tagged_zgc=%u "
                 "plain_value=%#lx plain_legacy=%u plain_zgc=%u load_bad_mask=%#lx\n",
                 pass ? "SPLIT" : "NO_SPLIT", static_cast<unsigned long>(taggedValue),
                 static_cast<unsigned>(taggedLegacy ? 1 : 0), static_cast<unsigned>(taggedZgc ? 1 : 0),
                 static_cast<unsigned long>(plainValue), static_cast<unsigned>(plainLegacy ? 1 : 0),
                 static_cast<unsigned>(plainZgc ? 1 : 0), static_cast<unsigned long>(::g_cjLoadBadMask));
    std::fflush(stderr);
}

void Report(const char* why) { ReportRaw(why); }

} // namespace GoodPredDiag
} // namespace MapleRuntime
