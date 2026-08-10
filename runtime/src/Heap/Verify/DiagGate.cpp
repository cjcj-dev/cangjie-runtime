// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/DiagGate.h"

#include <atomic>
#include <cstring>
#include <cstdlib>

#include "Base/Log.h"

namespace MapleRuntime {
namespace DiagGate {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// MRT_GCV2_DIAG is a CSV of tokens. "all" / "1" enables every token.
// Matching is substring-at-token-boundary (comma/space/semicolon separators).
bool DiagListHas(const char* list, const char* token)
{
    if (list == nullptr || list[0] == '\0' || token == nullptr || token[0] == '\0') {
        return false;
    }
    if (std::strcmp(list, "all") == 0 || std::strcmp(list, "1") == 0 || std::strcmp(list, "*") == 0) {
        return true;
    }
    const size_t tlen = std::strlen(token);
    const char* p = list;
    while (*p != '\0') {
        while (*p == ',' || *p == ' ' || *p == ';' || *p == ':') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const char* start = p;
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != ';' && *p != ':') {
            ++p;
        }
        size_t n = static_cast<size_t>(p - start);
        if (n == tlen && std::strncmp(start, token, tlen) == 0) {
            return true;
        }
    }
    return false;
}

const char* DiagList()
{
    return std::getenv("MRT_GCV2_DIAG");
}

} // namespace

bool TokenOn(const char* token)
{
    return DiagListHas(DiagList(), token);
}

bool LegacyOrToken(const char* legacyEnv, const char* token)
{
    if (legacyEnv != nullptr && EnvIsOne(legacyEnv)) {
        return true;
    }
    return TokenOn(token);
}

bool SelfTestOn()
{
    if (EnvIsOne("MRT_GCV2_DIAG_SELFTEST") || EnvIsOne("MRT_GCV2_IDLEEDGE_SELFTEST")) {
        return true;
    }
    return TokenOn("selftest");
}

void MaybeAnnounce()
{
    static std::atomic<uint32_t> once{ 0 };
    if (once.exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    if (EnvIsOne("MRT_GCV2_DIAG_HELP")) {
        LOG(RTLOG_ERROR,
            "[GCV2][diag][HELP] master=MRT_GCV2_DIAG=<csv|all> tokens=idleedge,promote,promotegap,"
            "fullclear,nullslot,selftest | legacy aliases still work: "
            "MRT_GCV2_IDLEEDGE MRT_GCV2_IDLEEDGE_STAMP_BITS MRT_GCV2_PROMOTEGAP_PROBE "
            "MRT_GCV2_FULLCLEAR_PROBE MRT_GCV2_NULLSLOT MRT_GCV2_IDLEEDGE_SELFTEST "
            "MRT_GCV2_DIAG_SELFTEST | discovery: MRT_GCV2_DIAG_ACTIVE=1");
    }
    if (EnvIsOne("MRT_GCV2_DIAG_ACTIVE") || DiagList() != nullptr) {
        LOG(RTLOG_ERROR,
            "[GCV2][diag][ACTIVE] MRT_GCV2_DIAG=%s idleedge=%d promote=%d fullclear=%d "
            "nullslot=%d selftest=%d stamp_bits_env=%s",
            DiagList() == nullptr ? "(unset)" : DiagList(),
            LegacyOrToken("MRT_GCV2_IDLEEDGE", "idleedge") ? 1 : 0,
            (LegacyOrToken("MRT_GCV2_PROMOTEGAP_PROBE", "promote") ||
             LegacyOrToken("MRT_GCV2_PROMOTEGAP_PROBE", "promotegap"))
                ? 1
                : 0,
            LegacyOrToken("MRT_GCV2_FULLCLEAR_PROBE", "fullclear") ? 1 : 0,
            LegacyOrToken("MRT_GCV2_NULLSLOT", "nullslot") ? 1 : 0, SelfTestOn() ? 1 : 0,
            std::getenv("MRT_GCV2_IDLEEDGE_STAMP_BITS") == nullptr
                ? "(default 18)"
                : std::getenv("MRT_GCV2_IDLEEDGE_STAMP_BITS"));
    }
}

void EmitCounterLegend()
{
    static std::atomic<uint32_t> once{ 0 };
    if (once.exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }
    // Format: name · meaning · healthy · falseHigh · falseLow
    // Loud (RTLOG_ERROR): must not hide next to quiet progress-only volume.
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] format=name · meaning · healthy · falseHigh · falseLow");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] missBare · old→young edge with no write stamp · "
        "healthy=load-dep (same order as oldToYoungEdges only if write side closed) · "
        "falseHigh=stamp collision reclass + promOld→censusYoung spur (fullclear) · "
        "falseLow=over-stamping");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] missRecordedLost · stamp recorded=1 but slot not in remset · "
        "healthy=often 0; spikes on young-after-drain windows · "
        "falseHigh=stamp never cleared across minor (remset drained, stamp lives) · "
        "falseLow=collision overwrote recorded bit");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] missPhaseLe8 · miss whose write phase ≤ INIT · "
        "healthy=load-dep (Idle bare window) · falseHigh=phase mis-stamp · falseLow=n/a");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] missPhaseGt8 · miss whose write phase > INIT · "
        "healthy=0 when only Idle/Init writers miss · falseHigh=stale FORWARD stamp · "
        "falseLow=n/a");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] remsetSize · Snapshot() cardinality at census · "
        "healthy≈oldToYoungEdges when remset closed · falseHigh=stale slots · "
        "falseLow=DrainForMinor/fullclear without rebuild (fwdlost empty window)");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] oldToYoungEdges · heap walk non-young holder → young target · "
        "healthy=load-dep · falseHigh=n/a · falseLow=invalid holder skip");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] stampProbeFail · open-address store exhausted kProbeMax · "
        "healthy=0 · falseHigh=table too small (raise STAMP_BITS) · falseLow=n/a");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] stampWraps · probes that hit a different field key · "
        "healthy=low relative to stampNotes · falseHigh=table saturation · falseLow=n/a");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] stampOccPct · occupied stamp slots / cap · "
        "healthy=<50%%; >50%% ⇒ INSTRUMENT_SATURATED · falseHigh=n/a · falseLow=n/a");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] promotegap.* · promote re-reg field walks (seen/rec/node10*) · "
        "healthy=rec≤seen; node10rec when target still young · falseHigh=n/a · "
        "falseLow=skipOldT counted as miss elsewhere");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] fullclear.bareReal · promYoung×censusYoung bare · "
        "healthy=0 if remset kept promote records · falseHigh=n/a · "
        "falseLow=matrix off so spur folds into bare");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] fullclear.bareSpur · promOld×censusYoung bare · "
        "healthy=load-dep (not a write-side bug) · falseHigh=if read as bareReal · falseLow=n/a");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] grant · Ensure newly painted liveInfo0 · "
        "healthy=often 0  ★ 0 = already in domain, NOT failure · "
        "falseHigh=n/a · falseLow=face miss (target never scanned)");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] already · Ensure found survived bit already set · "
        "healthy=≫0 when FYS/closure closed · falseHigh=n/a · falseLow=mark face not shared");
    LOG(RTLOG_ERROR,
        "[GCV2][diag][LEGEND] tooLate · Ensure after region ROUTED · "
        "healthy=0 · falseHigh=n/a · falseLow=n/a");
}

} // namespace DiagGate
} // namespace MapleRuntime
