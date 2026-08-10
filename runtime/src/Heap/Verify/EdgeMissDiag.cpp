// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/EdgeMissDiag.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Base/TimeUtils.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"

namespace MapleRuntime {
namespace EdgeMissDiag {
namespace {

std::atomic<int> g_enabled{ -1 };
std::atomic<size_t> g_markN{ 0 };
std::atomic<size_t> g_writeN{ 0 };
std::atomic<size_t> g_lookupN{ 0 };

struct MarkRec {
    uintptr_t obj;
    uint64_t ns;
    size_t gcCount;
    unsigned phase;
};

struct WriteRec {
    uintptr_t field;
    uintptr_t holder;
    uintptr_t oldRef;
    uintptr_t newRef;
    uint64_t ns;
    size_t gcCount;
    unsigned phase;
    unsigned holderMarkedAtWrite; // 1/0/0xff
    unsigned satbOld;
    unsigned satbNew;
    char site[16];
};

// Cover first major under CI cjpm: many marks, fewer Enum/Trace ref writes of interest.
constexpr size_t kMarkRing = 65536;
constexpr size_t kWriteRing = 65536;
MarkRec g_markRing[kMarkRing];
WriteRec g_writeRing[kWriteRing];
std::atomic<size_t> g_markSeq{ 0 };
std::atomic<size_t> g_writeSeq{ 0 };

bool GateOn()
{
    int e = g_enabled.load(std::memory_order_relaxed);
    if (e >= 0) {
        return e != 0;
    }
    int on = DiagGate::LegacyOrToken("MRT_GCV2_EDGEMISS", "edgemiss") ? 1 : 0;
    g_enabled.store(on, std::memory_order_relaxed);
    return on != 0;
}

unsigned HolderMarkedNow(BaseObject* holder)
{
    if (holder == nullptr || !Heap::IsHeapAddress(holder)) {
        return 0xffu;
    }
    RegionInfo* hr = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(holder));
    if (hr == nullptr || hr->IsFreeRegion() || hr->GetMarkBitmap() == nullptr) {
        return 0xffu;
    }
    return hr->IsMarkedObject(holder) ? 1u : 0u;
}

// Latest mark stamp for obj with matching gcCount (or any if gcCount==0 match-all last).
bool FindMark(uintptr_t obj, size_t preferGc, uint64_t& outNs, size_t& outGc, unsigned& outPhase)
{
    size_t seq = g_markSeq.load(std::memory_order_acquire);
    size_t n = seq < kMarkRing ? seq : kMarkRing;
    bool any = false;
    uint64_t bestNs = 0;
    size_t bestGc = 0;
    unsigned bestPh = 0xffu;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (seq - 1 - i) % kMarkRing;
        const MarkRec& rec = g_markRing[idx];
        if (rec.obj != obj) {
            continue;
        }
        if (preferGc != 0 && rec.gcCount != preferGc) {
            // Prefer same GC; still accept if nothing else found later.
            if (!any) {
                bestNs = rec.ns;
                bestGc = rec.gcCount;
                bestPh = rec.phase;
                any = true;
            }
            continue;
        }
        // First match walking newest→oldest with preferGc match wins.
        outNs = rec.ns;
        outGc = rec.gcCount;
        outPhase = rec.phase;
        return true;
    }
    if (any) {
        outNs = bestNs;
        outGc = bestGc;
        outPhase = bestPh;
        return true;
    }
    return false;
}

// Newest write of field (optionally matching newRef/from) in preferGc.
bool FindWrite(uintptr_t field, uintptr_t preferNew, size_t preferGc, WriteRec& out)
{
    size_t seq = g_writeSeq.load(std::memory_order_acquire);
    size_t n = seq < kWriteRing ? seq : kWriteRing;
    bool anyField = false;
    WriteRec anyRec{};
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (seq - 1 - i) % kWriteRing;
        const WriteRec& rec = g_writeRing[idx];
        if (rec.field != field) {
            continue;
        }
        if (preferGc != 0 && rec.gcCount != preferGc && rec.gcCount + 1 != preferGc) {
            // Accept current or previous GC (edge may be written just before flip).
            if (!anyField) {
                anyRec = rec;
                anyField = true;
            }
            continue;
        }
        if (preferNew != 0 && rec.newRef != preferNew && rec.oldRef != preferNew) {
            if (!anyField) {
                anyRec = rec;
                anyField = true;
            }
            continue;
        }
        out = rec;
        return true;
    }
    if (anyField) {
        out = anyRec;
        return true;
    }
    return false;
}

} // namespace

bool Enabled() { return GateOn(); }

void NoteMark(BaseObject* obj)
{
    if (!GateOn() || obj == nullptr) {
        return;
    }
    size_t seq = g_markSeq.fetch_add(1, std::memory_order_relaxed);
    MarkRec& rec = g_markRing[seq % kMarkRing];
    rec.obj = reinterpret_cast<uintptr_t>(obj);
    rec.ns = TimeUtil::NanoSeconds();
    rec.gcCount = g_gcCount;
    rec.phase = static_cast<unsigned>(Heap::GetHeap().GetGCPhase());
    g_markN.fetch_add(1, std::memory_order_relaxed);
}

void NoteWrite(BaseObject* holder, void* field, BaseObject* oldRef, BaseObject* newRef, const char* site,
               unsigned satbOld, unsigned satbNew)
{
    if (!GateOn() || field == nullptr) {
        return;
    }
    size_t seq = g_writeSeq.fetch_add(1, std::memory_order_relaxed);
    WriteRec& rec = g_writeRing[seq % kWriteRing];
    rec.field = reinterpret_cast<uintptr_t>(field);
    rec.holder = reinterpret_cast<uintptr_t>(holder);
    rec.oldRef = reinterpret_cast<uintptr_t>(oldRef);
    rec.newRef = reinterpret_cast<uintptr_t>(newRef);
    rec.ns = TimeUtil::NanoSeconds();
    rec.gcCount = g_gcCount;
    rec.phase = static_cast<unsigned>(Heap::GetHeap().GetGCPhase());
    rec.holderMarkedAtWrite = HolderMarkedNow(holder);
    rec.satbOld = satbOld;
    rec.satbNew = satbNew;
    if (site != nullptr) {
        size_t i = 0;
        for (; i + 1 < sizeof(rec.site) && site[i] != '\0'; ++i) {
            rec.site[i] = site[i];
        }
        rec.site[i] = '\0';
    } else {
        rec.site[0] = '\0';
    }
    g_writeN.fetch_add(1, std::memory_order_relaxed);
}

const char* LookupAtF3(BaseObject* holder, void* field, BaseObject* from, int holderMarked, int fromMarked)
{
    if (!GateOn()) {
        return "off";
    }
    size_t n = g_lookupN.fetch_add(1, std::memory_order_relaxed);
    if (n >= 64) {
        return "cap";
    }

    uintptr_t h = reinterpret_cast<uintptr_t>(holder);
    uintptr_t f = reinterpret_cast<uintptr_t>(field);
    uintptr_t fromU = reinterpret_cast<uintptr_t>(from);
    size_t gcNow = g_gcCount;
    unsigned phase = static_cast<unsigned>(Heap::GetHeap().GetGCPhase());

    uint64_t markNs = 0;
    size_t markGc = 0;
    unsigned markPh = 0xffu;
    bool haveMark = (holder != nullptr) && FindMark(h, gcNow, markNs, markGc, markPh);

    WriteRec w{};
    bool haveWrite = (field != nullptr) && FindWrite(f, fromU, gcNow, w);

    // remset Contains at F3 (postflip may already drain; still informative).
    unsigned remsetHit = 0xffu;
    if (field != nullptr) {
        remsetHit = Heap::GetHeap().GetRememberedSet().Contains(reinterpret_cast<MAddress>(field)) ? 1u : 0u;
    }

    const char* verdict = "unknown";
    // hM=0 group: holder never first-marked this cycle (or stamp missed).
    if (holderMarked == 0) {
        verdict = haveMark ? "hm0_but_stamp" : "hm0_no_mark";
    } else if (holderMarked == 1) {
        if (!haveWrite) {
            // No Enum/Trace write of this slot in ring ⇒ edge almost certainly
            // pre-existed before concurrent mark barriers ⇒ miss-follow if fM=0.
            verdict = "miss_follow_no_write";
        } else if (haveMark && w.ns > markNs && w.gcCount >= markGc) {
            // Write after holder first-mark in same/later GC ⇒ 甲-2 post-mark write.
            verdict = "post_mark_write";
        } else if (w.holderMarkedAtWrite == 1) {
            // Write saw holder already marked (even if mark stamp missed) ⇒ 甲-2.
            verdict = "post_mark_write";
        } else if (haveMark && w.ns <= markNs) {
            // Write before holder mark ⇒ edge present when scanned ⇒ 甲-1 if fM=0.
            verdict = "miss_follow_pre_mark_write";
        } else {
            verdict = "miss_follow_write_before_or_unknown";
        }
    } else {
        verdict = "hm_unknown";
    }

    LOG(RTLOG_ERROR,
        "[GCV2][nullslot][edgemiss] n=%zu verdict=%s hM=%d fM=%d holder=%p field=%p from=%p "
        "haveMark=%u markNs=%llu markGc=%zu markPh=%u "
        "haveWrite=%u wNs=%llu wGc=%zu wPh=%u wHMark=%u satbOld=%u satbNew=%u site=%s "
        "wOld=%p wNew=%p remset=%u phase=%u gc=%zu markN=%zu writeN=%zu",
        n, verdict, holderMarked, fromMarked, holder, field, from,
        haveMark ? 1u : 0u, static_cast<unsigned long long>(markNs), markGc, markPh,
        haveWrite ? 1u : 0u, static_cast<unsigned long long>(haveWrite ? w.ns : 0),
        haveWrite ? w.gcCount : 0, haveWrite ? w.phase : 0xffu,
        haveWrite ? w.holderMarkedAtWrite : 0xffu, haveWrite ? w.satbOld : 0xffu, haveWrite ? w.satbNew : 0xffu,
        haveWrite ? w.site : "-",
        haveWrite ? reinterpret_cast<void*>(w.oldRef) : nullptr,
        haveWrite ? reinterpret_cast<void*>(w.newRef) : nullptr, remsetHit, phase, gcNow,
        g_markN.load(std::memory_order_relaxed), g_writeN.load(std::memory_order_relaxed));

    return verdict;
}

} // namespace EdgeMissDiag
} // namespace MapleRuntime
