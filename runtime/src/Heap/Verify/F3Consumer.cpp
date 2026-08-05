// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/F3Consumer.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"

namespace MapleRuntime {
namespace {

constexpr size_t kCap = 4096;

struct Entry {
    void* obj = nullptr;
    void* field = nullptr;
    void* target = nullptr;
    uint64_t ns = 0;
    uint32_t young : 1;
    uint32_t major : 1;
    uint32_t marked : 1;
    uint32_t scanned : 1;
    uint32_t edgeFollowed : 1;
    uint32_t edgeSkipped : 1;
    char site[24];
};

std::mutex gMu;
size_t gNext = 0;
Entry gTab[kCap];

std::atomic<int> gPosCtrlFired{ 0 };
void* gPosHolder = nullptr;
void* gPosField = nullptr;
void* gPosTarget = nullptr;

// Bucket-1 mark skip (F3M_POSCTRL): independent of F3_CONSUMER ledger.
std::atomic<int> gMarkPosCtrlFired{ 0 };
void* gMarkPosObj = nullptr;

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// Caller must hold gMu.
Entry* FindOrAllocLocked(void* obj)
{
    if (obj == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < gNext && i < kCap; ++i) {
        if (gTab[i].obj == obj) {
            return &gTab[i];
        }
    }
    if (gNext >= kCap) {
        return nullptr;
    }
    Entry& e = gTab[gNext++];
    e.obj = obj;
    e.field = nullptr;
    e.target = nullptr;
    e.ns = TimeUtil::NanoSeconds();
    e.young = 0;
    e.major = 0;
    e.marked = 0;
    e.scanned = 0;
    e.edgeFollowed = 0;
    e.edgeSkipped = 0;
    e.site[0] = '\0';
    return &e;
}

// Caller must hold gMu.
Entry* FindLocked(void* obj)
{
    if (obj == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < gNext && i < kCap; ++i) {
        if (gTab[i].obj == obj) {
            return &gTab[i];
        }
    }
    return nullptr;
}

void CopySite(Entry& e, const char* site)
{
    if (site == nullptr) {
        return;
    }
    std::snprintf(e.site, sizeof(e.site), "%s", site);
}

} // namespace

bool F3Consumer::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_F3_CONSUMER") || EnvIsOne("MRT_GCV2_F3C_POSCTRL");
    return on;
}

bool F3Consumer::PosCtrlEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_F3C_POSCTRL");
    return on;
}

bool F3Consumer::MarkPosCtrlEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_F3M_POSCTRL");
    return on;
}

void F3Consumer::NoteMark(void* obj, const char* site, unsigned int young, unsigned int major)
{
    if (!Enabled() || obj == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lg(gMu);
    Entry* e = FindOrAllocLocked(obj);
    if (e == nullptr) {
        return;
    }
    e->marked = 1;
    e->young = young ? 1u : 0u;
    e->major = major ? 1u : 0u;
    e->ns = TimeUtil::NanoSeconds();
    CopySite(*e, site);
}

void F3Consumer::NoteScan(void* obj, const char* site, unsigned int young, unsigned int major)
{
    if (!Enabled() || obj == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lg(gMu);
    Entry* e = FindOrAllocLocked(obj);
    if (e == nullptr) {
        return;
    }
    e->scanned = 1;
    if (young) {
        e->young = 1;
    }
    if (major) {
        e->major = 1;
    }
    e->ns = TimeUtil::NanoSeconds();
    CopySite(*e, site);
}

void F3Consumer::NoteEdgeFollow(void* holder, void* field, void* target, const char* site)
{
    // Hot path: only record if holder already in ledger (was marked). Avoids O(heap) table growth.
    if (!Enabled() || holder == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lg(gMu);
    Entry* e = FindLocked(holder);
    if (e == nullptr) {
        return;
    }
    e->edgeFollowed = 1;
    e->field = field;
    e->target = target;
    e->ns = TimeUtil::NanoSeconds();
    CopySite(*e, site);
}

bool F3Consumer::ShouldSkipEdge(void* holder, void* field, void* target, const char* site)
{
    if (!PosCtrlEnabled() || holder == nullptr || target == nullptr) {
        return false;
    }
    // Fire once, only on major TraceRefField paths (not young scan): skip following
    // an edge from a marked+scanned holder so target can die while holder stays live.
    // site must be TraceRefField_* so we do not poison minor closure.
    if (site == nullptr || std::strncmp(site, "TraceRefField", 13) != 0) {
        return false;
    }
    if (gPosCtrlFired.load(std::memory_order_acquire) != 0) {
        return false;
    }
    std::lock_guard<std::mutex> lg(gMu);
    Entry* e = FindLocked(holder);
    if (e == nullptr || e->marked == 0 || e->scanned == 0 || e->major == 0) {
        return false;
    }
    int expected = 0;
    if (!gPosCtrlFired.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return false;
    }
    e->edgeSkipped = 1;
    e->field = field;
    e->target = target;
    CopySite(*e, site != nullptr ? site : "posctrl_skip");
    gPosHolder = holder;
    gPosField = field;
    gPosTarget = target;
    VLOG(REPORT,
         "[GCV2][F3C][POSCTRL] skip_edge holder=%p field=%p target=%p site=%s "
         "holderMarked=1 scanned=1 major=1 (intentional miss for bucket2 positive control)",
         holder, field, target, site != nullptr ? site : "?");
    return true;
}

bool F3Consumer::ShouldSkipMark(void* obj, unsigned int major)
{
    if (!MarkPosCtrlEnabled() || obj == nullptr || major == 0) {
        return false;
    }
    if (gMarkPosCtrlFired.load(std::memory_order_acquire) != 0) {
        return false;
    }
    int expected = 0;
    if (!gMarkPosCtrlFired.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return false;
    }
    gMarkPosObj = obj;
    VLOG(REPORT,
         "[GCV2][F3M][POSCTRL] skip_mark obj=%p major=1 "
         "(intentional unmarked holder for bucket1 positive control; no F3_CONSUMER ledger)",
         obj);
    return true;
}

void F3Consumer::NotePosCtrlFired(void* holder, void* field, void* target)
{
    gPosHolder = holder;
    gPosField = field;
    gPosTarget = target;
}

bool F3Consumer::MarkPosCtrlMatch(void* holder)
{
    return gMarkPosCtrlFired.load(std::memory_order_acquire) != 0 && holder == gMarkPosObj;
}

void F3Consumer::DumpAtAbort(void* holder, void* field, void* target, char* verdictBuf, size_t verdictBufSize)
{
    if (verdictBuf != nullptr && verdictBufSize > 0) {
        verdictBuf[0] = '\0';
    }
    if (!Enabled() && !MarkPosCtrlEnabled()) {
        if (verdictBuf != nullptr && verdictBufSize > 0) {
            std::snprintf(verdictBuf, verdictBufSize, "F3C_OFF");
        }
        return;
    }

    const int markPosMatch = MarkPosCtrlMatch(holder) ? 1 : 0;
    if (MarkPosCtrlEnabled() && !Enabled()) {
        // Bucket-1 POSCTRL path: no ledger; classify from mark-skip match alone.
        VLOG(REPORT,
             "[GCV2][F3M] holder=%p field=%p target=%p markPosMatch=%d markPosObj=%p "
             "bucket=%s",
             holder, field, target, markPosMatch, gMarkPosObj,
             markPosMatch ? "F3M_BUCKET1_POSCTRL_skip_mark" : "F3M_BUCKET1_natural_or_unrelated");
        if (verdictBuf != nullptr && verdictBufSize > 0) {
            std::snprintf(verdictBuf, verdictBufSize, "%s_mp%d",
                          markPosMatch ? "F3M_BUCKET1_POSCTRL" : "F3M_BUCKET1_NATURAL", markPosMatch);
        }
        return;
    }

    std::lock_guard<std::mutex> lg(gMu);
    Entry* e = FindLocked(holder);
    const int marked = e != nullptr ? static_cast<int>(e->marked) : 0;
    const int scanned = e != nullptr ? static_cast<int>(e->scanned) : 0;
    const int followed = e != nullptr ? static_cast<int>(e->edgeFollowed) : 0;
    const int skipped = e != nullptr ? static_cast<int>(e->edgeSkipped) : 0;
    char siteBuf[24] = "none";
    if (e != nullptr && e->site[0] != '\0') {
        std::snprintf(siteBuf, sizeof(siteBuf), "%s", e->site);
    }
    const char* site = siteBuf;
    const unsigned young = e != nullptr ? e->young : 0u;
    const unsigned major = e != nullptr ? e->major : 0u;

    // Classify (a)/(b)/(c) from ledger alone.
    // (a) marked+scanned but this edge not followed (gctib/slot miss or posctrl skip)
    // (b) marked but never scanned (mark set without field walk)
    // (c) marked+scanned+edge followed yet target still dead (ordering / post-follow kill)
    const char* bucket = "UNCLASSIFIED";
    if (marked == 1 && scanned == 0) {
        bucket = "F3C_BUCKET2_b_marked_not_scanned";
    } else if (marked == 1 && scanned == 1 && followed == 0) {
        bucket = skipped ? "F3C_BUCKET2_a_scan_missed_slot_POSCTRL" : "F3C_BUCKET2_a_scan_missed_slot";
    } else if (marked == 1 && scanned == 1 && followed == 1) {
        bucket = "F3C_BUCKET2_c_followed_then_killed";
    } else if (marked == 0) {
        bucket = markPosMatch ? "F3M_BUCKET1_POSCTRL_skip_mark" : "F3C_BUCKET1_holder_never_marked";
    }

    const int posMatch =
        (gPosCtrlFired.load(std::memory_order_acquire) != 0 && holder == gPosHolder && target == gPosTarget) ? 1 : 0;

    VLOG(REPORT,
         "[GCV2][F3C] holder=%p field=%p target=%p ledgerHit=%u marked=%d scanned=%d edgeFollowed=%d "
         "edgeSkipped=%d young=%u major=%u site=%s posCtrlMatch=%d posFired=%d posHolder=%p posTarget=%p "
         "markPosMatch=%d bucket=%s",
         holder, field, target, static_cast<unsigned>(e != nullptr), marked, scanned, followed, skipped, young, major,
         site, posMatch, gPosCtrlFired.load(std::memory_order_acquire), gPosHolder, gPosTarget, markPosMatch, bucket);

    if (verdictBuf != nullptr && verdictBufSize > 0) {
        std::snprintf(verdictBuf, verdictBufSize, "%s_m%d_s%d_f%d_skip%d_pos%d_mp%d", bucket, marked, scanned, followed,
                      skipped, posMatch, markPosMatch);
    }
}

} // namespace MapleRuntime
