// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/O0HoleDiag.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace O0HoleDiag {
namespace {

constexpr size_t kSkipCap = 1u << 20;
constexpr size_t kSampleLimit = 8;

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return defaultValue;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) {
        return defaultValue;
    }
    return static_cast<size_t>(parsed);
}

size_t HashPtr(uintptr_t p)
{
    p ^= p >> 30;
    p *= 0xbf58476d1ce4e5b9ull;
    p ^= p >> 27;
    p *= 0x94d049bb133111ebull;
    p ^= p >> 31;
    return static_cast<size_t>(p);
}

struct SkipSlot {
    std::atomic<uintptr_t> field{ 0 };
    std::atomic<uint8_t> kind{ 0 };
};
SkipSlot g_skipSlots[kSkipCap];
std::atomic<uint64_t> g_skipSlotNotes{ 0 };
std::atomic<uint64_t> g_skipSlotCollisions{ 0 };

// 甲 counters
std::atomic<uint64_t> g_fwdEnterRegion{ 0 };
std::atomic<uint64_t> g_fwdEnterObj{ 0 };
std::atomic<uint64_t> g_fwdFieldSeen{ 0 };
std::atomic<uint64_t> g_fwdRecYoung{ 0 };
std::atomic<uint64_t> g_fwdSkipNull{ 0 };
std::atomic<uint64_t> g_fwdSkipOld{ 0 };
std::atomic<uint64_t> g_fwdSkipNoTo{ 0 };
std::atomic<uint64_t> g_fwdSkipNoRef{ 0 };

// 乙 counters
std::atomic<uint64_t> g_rebuildEnter{ 0 };
std::atomic<uint64_t> g_rebuildGateSkip{ 0 };
std::atomic<uint64_t> g_rebuildGateOpen{ 0 };
std::atomic<uint64_t> g_rebuildYoungBeforeSum{ 0 };
std::atomic<uint64_t> g_rebuildYoungAfterSum{ 0 };
std::atomic<uint64_t> g_rebuildYoungBeforeNz{ 0 };
std::atomic<uint64_t> g_rebuildYoungAfterNz{ 0 };
std::atomic<uint64_t> g_rebuildResidualDemoteSum{ 0 };
std::atomic<uint64_t> g_rebuildResidualPromoteSum{ 0 };
std::atomic<uint64_t> g_rebuildPromoteReplaySum{ 0 };
std::atomic<uint64_t> g_rebuildRebuiltSum{ 0 };
std::atomic<uint64_t> g_rebuildVirtualWouldSum{ 0 };
std::atomic<uint64_t> g_rebuildVirtualMissRemsetSum{ 0 };
std::atomic<uint64_t> g_rebuildVirtualWouldNz{ 0 };

// join
std::atomic<uint64_t> g_censusNeverSeen{ 0 };
std::atomic<uint64_t> g_censusJoinSkip{ 0 };
std::atomic<uint64_t> g_censusJoinNoSkip{ 0 };
std::atomic<uint64_t> g_censusJoinSkipNull{ 0 };
std::atomic<uint64_t> g_censusJoinSkipOld{ 0 };

std::atomic<uint64_t> g_sampleSkip{ 0 };
std::atomic<uint64_t> g_sampleJoin{ 0 };
std::atomic<uint64_t> g_sampleGate{ 0 };

bool StoreSkipSlot(MAddress fieldAddress, uint8_t kind)
{
    if (fieldAddress == 0) {
        return false;
    }
    uintptr_t key = static_cast<uintptr_t>(fieldAddress);
    size_t idx = HashPtr(key) & (kSkipCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kSkipCap - 1);
        uintptr_t cur = g_skipSlots[i].field.load(std::memory_order_acquire);
        if (cur == key) {
            g_skipSlots[i].kind.store(kind, std::memory_order_release);
            return true;
        }
        if (cur == 0) {
            uintptr_t exp = 0;
            if (g_skipSlots[i].field.compare_exchange_strong(exp, key, std::memory_order_acq_rel)) {
                g_skipSlots[i].kind.store(kind, std::memory_order_release);
                g_skipSlotNotes.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            if (exp == key) {
                g_skipSlots[i].kind.store(kind, std::memory_order_release);
                return true;
            }
        }
    }
    g_skipSlotCollisions.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool LookupSkipSlot(MAddress fieldAddress, uint8_t* outKind)
{
    if (fieldAddress == 0) {
        return false;
    }
    uintptr_t key = static_cast<uintptr_t>(fieldAddress);
    size_t idx = HashPtr(key) & (kSkipCap - 1);
    for (size_t p = 0; p < 32; ++p) {
        size_t i = (idx + p) & (kSkipCap - 1);
        uintptr_t cur = g_skipSlots[i].field.load(std::memory_order_acquire);
        if (cur == key) {
            if (outKind != nullptr) {
                *outKind = g_skipSlots[i].kind.load(std::memory_order_acquire);
            }
            return true;
        }
        if (cur == 0) {
            return false;
        }
    }
    return false;
}

} // namespace

bool Enabled()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return DiagGate::LegacyOrToken("MRT_GCV2_O0HOLE", "o0hole");
    }();
    return on;
}

void NoteFwdEnterRegion(RegionInfo* region)
{
    if (!Enabled()) {
        return;
    }
    g_fwdEnterRegion.fetch_add(1, std::memory_order_relaxed);
    (void)region;
}

void NoteFwdEnterObj(BaseObject* toObj)
{
    if (!Enabled()) {
        return;
    }
    g_fwdEnterObj.fetch_add(1, std::memory_order_relaxed);
    (void)toObj;
}

void NoteFwdSkipSlot(MAddress fieldAddress, uint8_t skipKind)
{
    if (!Enabled() || fieldAddress == 0) {
        return;
    }
    (void)StoreSkipSlot(fieldAddress, skipKind);
}

void NoteFwdField(MAddress fieldAddress, BaseObject* /*toObj*/, BaseObject* /*target*/, bool recorded,
                  uint8_t skipKind)
{
    if (!Enabled()) {
        return;
    }
    g_fwdFieldSeen.fetch_add(1, std::memory_order_relaxed);
    if (recorded || skipKind == 0) {
        g_fwdRecYoung.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (skipKind == 1) {
        g_fwdSkipNull.fetch_add(1, std::memory_order_relaxed);
        NoteFwdSkipSlot(fieldAddress, 1);
    } else if (skipKind == 2) {
        g_fwdSkipOld.fetch_add(1, std::memory_order_relaxed);
        NoteFwdSkipSlot(fieldAddress, 2);
    } else if (skipKind == 3) {
        g_fwdSkipNoTo.fetch_add(1, std::memory_order_relaxed);
    } else if (skipKind == 4) {
        g_fwdSkipNoRef.fetch_add(1, std::memory_order_relaxed);
    }
    size_t sampleCap = EnvSizeT("MRT_GCV2_O0HOLE_SAMPLES", kSampleLimit);
    uint64_t n = g_sampleSkip.fetch_add(1, std::memory_order_relaxed);
    if (n < sampleCap && fieldAddress != 0) {
        VLOG(REPORT, "[O0HOLE][fwd-skip] n=%llu slot=%p kind=%u",
             static_cast<unsigned long long>(n + 1), reinterpret_cast<void*>(fieldAddress),
             static_cast<unsigned>(skipKind));
    }
}

void NoteRebuildGate(size_t youngBeforeResidual, size_t youngAfterResidual, size_t residualDemoteN,
                     size_t residualPromoteRecords, size_t promoteReplay, size_t rebuiltRecords,
                     size_t virtualWouldRecord, size_t virtualWouldMissRemset, bool gateSkip)
{
    if (!Enabled()) {
        return;
    }
    g_rebuildEnter.fetch_add(1, std::memory_order_relaxed);
    g_rebuildYoungBeforeSum.fetch_add(youngBeforeResidual, std::memory_order_relaxed);
    g_rebuildYoungAfterSum.fetch_add(youngAfterResidual, std::memory_order_relaxed);
    g_rebuildResidualDemoteSum.fetch_add(residualDemoteN, std::memory_order_relaxed);
    g_rebuildResidualPromoteSum.fetch_add(residualPromoteRecords, std::memory_order_relaxed);
    g_rebuildPromoteReplaySum.fetch_add(promoteReplay, std::memory_order_relaxed);
    g_rebuildRebuiltSum.fetch_add(rebuiltRecords, std::memory_order_relaxed);
    g_rebuildVirtualWouldSum.fetch_add(virtualWouldRecord, std::memory_order_relaxed);
    g_rebuildVirtualMissRemsetSum.fetch_add(virtualWouldMissRemset, std::memory_order_relaxed);
    if (youngBeforeResidual != 0) {
        g_rebuildYoungBeforeNz.fetch_add(1, std::memory_order_relaxed);
    }
    if (youngAfterResidual != 0) {
        g_rebuildYoungAfterNz.fetch_add(1, std::memory_order_relaxed);
    }
    if (virtualWouldRecord != 0) {
        g_rebuildVirtualWouldNz.fetch_add(1, std::memory_order_relaxed);
    }
    if (gateSkip) {
        g_rebuildGateSkip.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_rebuildGateOpen.fetch_add(1, std::memory_order_relaxed);
    }
    size_t sampleCap = EnvSizeT("MRT_GCV2_O0HOLE_SAMPLES", kSampleLimit);
    uint64_t n = g_sampleGate.fetch_add(1, std::memory_order_relaxed);
    if (n < sampleCap) {
        VLOG(REPORT,
             "[O0HOLE][rebuild-gate] n=%llu youngBefore=%zu youngAfter=%zu residualDemote=%zu "
             "residualPromote=%zu promoteReplay=%zu rebuilt=%zu virtualWould=%zu virtualMissRemset=%zu "
             "gateSkip=%u",
             static_cast<unsigned long long>(n + 1), youngBeforeResidual, youngAfterResidual,
             residualDemoteN, residualPromoteRecords, promoteReplay, rebuiltRecords, virtualWouldRecord,
             virtualWouldMissRemset, gateSkip ? 1u : 0u);
    }
}

bool NoteCensusNeverSeen(MAddress fieldAddress)
{
    if (!Enabled() || fieldAddress == 0) {
        return false;
    }
    g_censusNeverSeen.fetch_add(1, std::memory_order_relaxed);
    uint8_t kind = 0;
    if (LookupSkipSlot(fieldAddress, &kind)) {
        g_censusJoinSkip.fetch_add(1, std::memory_order_relaxed);
        if (kind == 1) {
            g_censusJoinSkipNull.fetch_add(1, std::memory_order_relaxed);
        } else if (kind == 2) {
            g_censusJoinSkipOld.fetch_add(1, std::memory_order_relaxed);
        }
        size_t sampleCap = EnvSizeT("MRT_GCV2_O0HOLE_SAMPLES", kSampleLimit);
        uint64_t n = g_sampleJoin.fetch_add(1, std::memory_order_relaxed);
        if (n < sampleCap) {
            VLOG(REPORT, "[O0HOLE][causal-join] n=%llu slot=%p neverSeen∩fwdSkip=1 kind=%u",
                 static_cast<unsigned long long>(n + 1), reinterpret_cast<void*>(fieldAddress),
                 static_cast<unsigned>(kind));
        }
        return true;
    }
    g_censusJoinNoSkip.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void DumpProcessTotals(const char* tag)
{
    if (!Enabled()) {
        return;
    }
    VLOG(REPORT,
         "[O0HOLE][TOTAL] tag=%s | 甲 fwdEnterRegion=%llu fwdEnterObj=%llu fieldSeen=%llu "
         "recYoung=%llu skipNull=%llu skipOld=%llu skipNoTo=%llu skipNoRef=%llu skipSlotNotes=%llu | "
         "乙 rebuildEnter=%llu gateSkip=%llu gateOpen=%llu youngBeforeSum=%llu youngAfterSum=%llu "
         "youngBeforeNz=%llu youngAfterNz=%llu residualDemoteSum=%llu residualPromoteSum=%llu "
         "promoteReplaySum=%llu rebuiltSum=%llu virtualWouldSum=%llu virtualMissRemsetSum=%llu "
         "virtualWouldNz=%llu | censusNeverSeen=%llu joinSkip=%llu joinNoSkip=%llu "
         "joinSkipNull=%llu joinSkipOld=%llu skipColl=%llu",
         tag == nullptr ? "-" : tag,
         static_cast<unsigned long long>(g_fwdEnterRegion.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdEnterObj.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdFieldSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdRecYoung.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdSkipNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdSkipOld.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdSkipNoTo.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_fwdSkipNoRef.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipSlotNotes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildEnter.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildGateSkip.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildGateOpen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildYoungBeforeSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildYoungAfterSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildYoungBeforeNz.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildYoungAfterNz.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildResidualDemoteSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildResidualPromoteSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildPromoteReplaySum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildRebuiltSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildVirtualWouldSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildVirtualMissRemsetSum.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_rebuildVirtualWouldNz.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusNeverSeen.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusJoinSkip.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusJoinNoSkip.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusJoinSkipNull.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_censusJoinSkipOld.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(g_skipSlotCollisions.load(std::memory_order_relaxed)));
}

} // namespace O0HoleDiag
} // namespace MapleRuntime
