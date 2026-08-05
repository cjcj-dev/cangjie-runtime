// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/SatbGap.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace {

// barestore T0: 8192 filled mid-package (satbgap B2 not_in_scan_set noise).
// Unbounded would thrash; 1<<20 holds major TRACE holders for selfhost pkgs.
constexpr size_t kScanCap = 1u << 20;
constexpr size_t kWriteCap = 4096;

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

std::mutex gMu;
void* gScanSet[kScanCap];
size_t gScanN = 0;

struct WriteRec {
    void* holder;
    void* field;
    void* oldRef;
    void* newRef;
    intptr_t slotOff;
    int satbLoggedNew;
    int posSkip;
    uint32_t phase;
    char site[24];
};
WriteRec gWrites[kWriteCap];
size_t gWriteN = 0;
size_t gWriteTotal = 0;
size_t gPostScanWriteTotal = 0;
size_t gPostScanNoSatb = 0;

std::atomic<int> gPosFired{ 0 };
void* gPosHolder = nullptr;
void* gPosField = nullptr;
void* gPosNew = nullptr;
intptr_t gPosOff = -1;

bool ScanContainsUnlocked(void* holder)
{
    for (size_t i = 0; i < gScanN; ++i) {
        if (gScanSet[i] == holder) {
            return true;
        }
    }
    return false;
}

} // namespace

bool SatbGap::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_SATB_GAP") || EnvIsOne("MRT_GCV2_SATB_POSCTRL");
    return on;
}

bool SatbGap::PosCtrlEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_SATB_POSCTRL");
    return on;
}

void SatbGap::NoteScanDone(void* holder)
{
    if (!Enabled() || holder == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(gMu);
    if (ScanContainsUnlocked(holder)) {
        return;
    }
    if (gScanN < kScanCap) {
        gScanSet[gScanN++] = holder;
    }
}

bool SatbGap::IsScanned(void* holder)
{
    if (!Enabled() || holder == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(gMu);
    return ScanContainsUnlocked(holder);
}

bool SatbGap::ShouldSkipSatbNew(void* holder, void* field, void* newRef, const char* site)
{
    if (!PosCtrlEnabled() || holder == nullptr || newRef == nullptr) {
        return false;
    }
    if (gPosFired.load(std::memory_order_acquire) != 0) {
        return false;
    }
    if (!IsScanned(holder)) {
        return false;
    }
    // Prefer non-null new edge on a scanned holder — edge-level inject.
    int expected = 0;
    if (!gPosFired.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return false;
    }
    gPosHolder = holder;
    gPosField = field;
    gPosNew = newRef;
    gPosOff = (holder != nullptr && field != nullptr)
                  ? BaseObject::FieldOffset(reinterpret_cast<BaseObject*>(holder), field)
                  : -1;
    BaseObject* h = reinterpret_cast<BaseObject*>(holder);
    const char* tname = "?";
    if (h != nullptr && h->IsValidObject()) {
        TypeInfo* ti = h->GetTypeInfo();
        if (ti != nullptr && ti->GetName() != nullptr) {
            tname = ti->GetName();
        }
    }
    VLOG(REPORT,
         "[GCV2][SATB][POSCTRL] skip_satb_new holder=%p field=%p new=%p off=%zd type=%s site=%s "
         "(post-scan write without SATB enqueue)",
         holder, field, newRef, gPosOff, tname, site != nullptr ? site : "?");
    return true;
}

bool SatbGap::NoteWrite(void* holder, void* field, void* oldRef, void* newRef, int satbLoggedNew, const char* site)
{
    if (!Enabled()) {
        return false;
    }
    const bool scanned = IsScanned(holder);
    if (!scanned && !PosCtrlEnabled()) {
        // Hot path: only record post-scan writes when gap probe is on.
        // Always allow POSCTRL path to observe.
    }
    bool posSkip = false;
    if (scanned && newRef != nullptr && satbLoggedNew == 0) {
        // Caller already decided not to log; still record.
    }
    if (scanned && newRef != nullptr && PosCtrlEnabled() && satbLoggedNew == 1) {
        // POSCTRL decided before enqueue: handled via ShouldSkipSatbNew + satbLoggedNew=0.
    }
    if (!scanned) {
        return false;
    }

    std::lock_guard<std::mutex> lk(gMu);
    ++gPostScanWriteTotal;
    if (satbLoggedNew == 0) {
        ++gPostScanNoSatb;
    }
    if (gWriteN < kWriteCap) {
        WriteRec& r = gWrites[gWriteN++];
        r.holder = holder;
        r.field = field;
        r.oldRef = oldRef;
        r.newRef = newRef;
        r.slotOff = (holder != nullptr && field != nullptr)
                        ? BaseObject::FieldOffset(reinterpret_cast<BaseObject*>(holder), field)
                        : -1;
        r.satbLoggedNew = satbLoggedNew;
        r.posSkip = (gPosFired.load(std::memory_order_acquire) != 0 && holder == gPosHolder && field == gPosField &&
                     newRef == gPosNew)
                        ? 1
                        : 0;
        r.phase = 0;
        r.site[0] = '\0';
        if (site != nullptr) {
            std::snprintf(r.site, sizeof(r.site), "%s", site);
        }
    }
    ++gWriteTotal;

    // Sample first few post-scan no-satb for live log (cap spam).
    static std::atomic<int> sample{ 0 };
    if (satbLoggedNew == 0 && sample.fetch_add(1, std::memory_order_relaxed) < 8) {
        VLOG(REPORT,
             "[GCV2][SATB][postscan-write] holder=%p field=%p old=%p new=%p satbNew=%d off=%zd site=%s",
             holder, field, oldRef, newRef, satbLoggedNew,
             (holder != nullptr && field != nullptr)
                 ? BaseObject::FieldOffset(reinterpret_cast<BaseObject*>(holder), field)
                 : static_cast<intptr_t>(-1),
             site != nullptr ? site : "?");
    }
    (void)posSkip;
    return false;
}

bool SatbGap::PosCtrlMatch(void* holder, void* target)
{
    return gPosFired.load(std::memory_order_acquire) != 0 && holder == gPosHolder && target == gPosNew;
}

void SatbGap::DumpAtAbort(void* holder, void* field, void* target, char* verdictBuf, size_t verdictBufSize)
{
    if (verdictBuf != nullptr && verdictBufSize > 0) {
        verdictBuf[0] = '\0';
    }
    if (!Enabled() && !EnvIsOne("MRT_GCV2_F3_DEATH")) {
        if (verdictBuf != nullptr && verdictBufSize > 0) {
            std::snprintf(verdictBuf, verdictBufSize, "SATB_OFF");
        }
        return;
    }

    int scanned = 0;
    int matchWrite = 0;
    int matchNoSatb = 0;
    int matchPos = PosCtrlMatch(holder, target) ? 1 : 0;
    intptr_t slotOff = -1;
    if (holder != nullptr && field != nullptr) {
        slotOff = BaseObject::FieldOffset(reinterpret_cast<BaseObject*>(holder), field);
    }

    {
        std::lock_guard<std::mutex> lk(gMu);
        scanned = ScanContainsUnlocked(holder) ? 1 : 0;
        for (size_t i = 0; i < gWriteN; ++i) {
            const WriteRec& r = gWrites[i];
            if (r.holder != holder) {
                continue;
            }
            if (r.newRef == target || r.field == field) {
                matchWrite = 1;
                if (r.satbLoggedNew == 0) {
                    matchNoSatb = 1;
                }
            }
        }
    }

    const char* sClass = "SATB_NO_POSTSCAN_WRITE_MATCH";
    if (matchPos) {
        sClass = "SATB_POSCTRL_MATCH_injected_gap";
    } else if (scanned && matchNoSatb) {
        sClass = "SATBGAP_CONFIRMED_postscan_nosatb";
    } else if (scanned && matchWrite) {
        sClass = "SATB_POSTSCAN_WRITE_LOGGED";
    } else if (scanned) {
        sClass = "SATB_SCANNED_NO_WRITE_RECORD";
    } else if (!scanned) {
        sClass = "SATB_HOLDER_NOT_IN_SCAN_SET";
    }

    VLOG(REPORT,
         "[GCV2][SATB] holder=%p field=%p target=%p slotOff=%zd scanned=%d matchWrite=%d matchNoSatb=%d "
         "posMatch=%d posFired=%d scanN=%zu writeN=%zu postScanW=%zu postScanNoSatb=%zu class=%s",
         holder, field, target, slotOff, scanned, matchWrite, matchNoSatb, matchPos,
         gPosFired.load(std::memory_order_acquire), gScanN, gWriteN, gPostScanWriteTotal, gPostScanNoSatb, sClass);

    if (verdictBuf != nullptr && verdictBufSize > 0) {
        std::snprintf(verdictBuf, verdictBufSize, "%s_sc%d_mw%d_ns%d_pos%d", sClass, scanned, matchWrite, matchNoSatb,
                      matchPos);
    }
}

} // namespace MapleRuntime
