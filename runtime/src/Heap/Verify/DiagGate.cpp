// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/DiagGate.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"

namespace MapleRuntime {
namespace DiagGate {
namespace {
// MRT_GCV2_DIAG is read once. Every consumer already caches the answer in a
// function-local `static const bool`, so the product path pays one getenv per
// gate at first use and nothing afterwards.
const char* DiagEnv()
{
    static const char* const value = std::getenv("MRT_GCV2_DIAG");
    return value;
}

bool EnvIsOne(const char* name)
{
    if (name == nullptr) {
        return false;
    }
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// Case-sensitive CSV membership, comma or space separated, per the header contract.
bool CsvContains(const char* csv, const char* token)
{
    if (csv == nullptr || token == nullptr || token[0] == '\0') {
        return false;
    }
    const size_t tokenLen = std::strlen(token);
    const char* cursor = csv;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != ',' && *cursor != ' ' && *cursor != '\t') {
            ++cursor;
        }
        const size_t len = static_cast<size_t>(cursor - start);
        if (len == tokenLen && std::strncmp(start, token, tokenLen) == 0) {
            return true;
        }
    }
    return false;
}
} // namespace

bool TokenOn(const char* token)
{
    const char* csv = DiagEnv();
    if (csv == nullptr || csv[0] == '\0') {
        return false;
    }
    if (std::strcmp(csv, "all") == 0 || std::strcmp(csv, "1") == 0) {
        return true;
    }
    return CsvContains(csv, token);
}

bool LegacyOrToken(const char* legacyEnv, const char* token)
{
    return EnvIsOne(legacyEnv) || TokenOn(token);
}

bool SelfTestOn()
{
    return EnvIsOne("MRT_GCV2_DIAG_SELFTEST") || TokenOn("selftest");
}

void MaybeAnnounce()
{
    static std::atomic<bool> announced{ false };
    const bool wantHelp = EnvIsOne("MRT_GCV2_DIAG_HELP");
    const bool wantActive = EnvIsOne("MRT_GCV2_DIAG_ACTIVE");
    if ((!wantHelp && !wantActive) || announced.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    const char* csv = DiagEnv();
    if (wantHelp) {
        LOG(RTLOG_ERROR,
            "[GCV2][diag] tokens: promote promotegap nullslot stackref fromver oneseq rootgate reffixwalk "
            "markcomplete markcompletefatal intedge statheal selftest all | legacy: MRT_GCV2_<NAME>=1");
    }
    if (wantActive) {
        LOG(RTLOG_ERROR, "[GCV2][diag] MRT_GCV2_DIAG=%s selftest=%d", csv == nullptr ? "(unset)" : csv,
            static_cast<int>(SelfTestOn()));
    }
}

void EmitCounterLegend()
{
    static std::atomic<bool> emitted{ false };
    if (emitted.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    // Loud on purpose: a zero from a diagnostic must be readable as "the arm did
    // not fire", never as "the probe is dead".  DiagGate itself was hollowed once
    // (every accessor `return false;`), which turned nine gates into silent
    // false negatives; the legend is what makes that state visible in a log.
    LOG(RTLOG_ERROR,
        "[GCV2][diag] legend: gates live (env-read). A zero means the arm did not fire. "
        "Confirm the probe is armed with MRT_GCV2_DIAG_ACTIVE=1 before reading any zero.");
}

} // namespace DiagGate
} // namespace MapleRuntime
