// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/TlRawDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "securec.h"
#include "Common/BaseObject.h"
#include "Common/Runtime.h"
#include "Heap/Allocator/AllocBuffer.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionList.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
namespace TlRawDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_TLRAW")) {
            return true;
        }
        return DiagGate::TokenOn("tlraw");
    }();
    return on;
}

constexpr size_t kInitCap = 1u << 16;

struct InitRow {
    std::atomic<uintptr_t> start{ 0 };
    std::atomic<uint32_t> inits{ 0 };
    std::atomic<uint32_t> lastType{ 0 };
};

InitRow g_initTab[kInitCap];
std::atomic<size_t> g_enter{ 0 };
std::atomic<size_t> g_initNotes{ 0 };
std::atomic<size_t> g_initSat{ 0 };
std::atomic<size_t> g_tlSmallMax{ 0 };
std::atomic<size_t> g_tlLargeMax{ 0 };
std::atomic<size_t> g_o2yMax{ 0 };
std::atomic<size_t> g_o2yTotal{ 0 };
std::atomic<bool> g_atexit{ false };
std::atomic<bool> g_healthOnce{ false };

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

void HealthOnce()
{
    bool expected = false;
    if (!g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }
    LOG(RTLOG_ERROR, "[GCV2][tlraw] health probe_live=1 env=MRT_GCV2_TLRAW=1");
}

size_t TabIdx(uintptr_t start)
{
    return (start >> 12) & (kInitCap - 1);
}

InitRow* FindOrInsert(uintptr_t start)
{
    if (start == 0) {
        return nullptr;
    }
    size_t idx = TabIdx(start);
    for (size_t probe = 0; probe < 64; ++probe) {
        size_t i = (idx + probe) & (kInitCap - 1);
        uintptr_t cur = g_initTab[i].start.load(std::memory_order_acquire);
        if (cur == start) {
            return &g_initTab[i];
        }
        if (cur == 0) {
            uintptr_t expected = 0;
            if (g_initTab[i].start.compare_exchange_strong(expected, start, std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
                return &g_initTab[i];
            }
            if (expected == start) {
                return &g_initTab[i];
            }
        }
    }
    g_initSat.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

InitRow* FindRow(uintptr_t start)
{
    if (start == 0) {
        return nullptr;
    }
    size_t idx = TabIdx(start);
    for (size_t probe = 0; probe < 64; ++probe) {
        size_t i = (idx + probe) & (kInitCap - 1);
        uintptr_t cur = g_initTab[i].start.load(std::memory_order_acquire);
        if (cur == start) {
            return &g_initTab[i];
        }
        if (cur == 0) {
            return nullptr;
        }
    }
    return nullptr;
}

bool RegionHasOldToYoung(RegionInfo* region, size_t& walkAbort)
{
    if (region == nullptr || region->IsYoungRegion() || region->IsGarbageRegion()) {
        return false;
    }
    bool hit = false;
    region->VisitAllObjects([&hit, &walkAbort](BaseObject* object) {
        if (object == nullptr) {
            return;
        }
        if (!Collector::PlausibleManagedObjectGate("tlraw-o2y", object)) {
            ++walkAbort;
            return;
        }
        if (!object->HasRefField()) {
            return;
        }
        object->ForEachRefField([&hit](RefField<>& field) {
            if (hit) {
                return;
            }
            BaseObject* target = to_object(field.GetTargetObject());
            if (target == nullptr || !Heap::IsHeapAddress(target)) {
                return;
            }
            RegionInfo* targetRegion = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(target));
            if (targetRegion != nullptr && targetRegion->IsYoungRegion()) {
                hit = true;
            }
        });
    });
    return hit;
}

void CensusList(RegionList& list, size_t& regions, size_t& o2y, size_t& walkAbort, size_t& youngTl)
{
    list.VisitAllRegions([&regions, &o2y, &walkAbort, &youngTl](RegionInfo* region) {
        if (region == nullptr) {
            return;
        }
        ++regions;
        if (region->IsYoungRegion()) {
            ++youngTl;
        }
        if (RegionHasOldToYoung(region, walkAbort)) {
            ++o2y;
        }
    });
}

void WriteCrashLine(const char* buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return;
    }
    (void)write(STDERR_FILENO, buf, len);
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteMinorEnter(size_t minorRun)
{
    if (!GateOn()) {
        return;
    }
    EnsureAtexit();
    HealthOnce();
    size_t n = g_enter.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t buffers = 0;
    size_t tlSmall = 0;
    size_t tlLarge = 0;
    size_t o2ySmall = 0;
    size_t o2yLarge = 0;
    size_t walkAbort = 0;
    size_t youngTl = 0;
    Heap::GetHeap().GetAllocator().VisitAllocBuffers(
        [&buffers, &tlSmall, &tlLarge, &o2ySmall, &o2yLarge, &walkAbort, &youngTl](AllocBuffer& buffer) {
            ++buffers;
            CensusList(buffer.GetTlRawPointerRegions(), tlSmall, o2ySmall, walkAbort, youngTl);
            CensusList(buffer.GetTlLargeRawPointerRegions(), tlLarge, o2yLarge, walkAbort, youngTl);
        });
    size_t o2y = o2ySmall + o2yLarge;
    g_o2yTotal.fetch_add(o2y, std::memory_order_relaxed);
    size_t prevSmall = g_tlSmallMax.load(std::memory_order_relaxed);
    while (tlSmall > prevSmall &&
           !g_tlSmallMax.compare_exchange_weak(prevSmall, tlSmall, std::memory_order_relaxed)) {
    }
    size_t prevLarge = g_tlLargeMax.load(std::memory_order_relaxed);
    while (tlLarge > prevLarge &&
           !g_tlLargeMax.compare_exchange_weak(prevLarge, tlLarge, std::memory_order_relaxed)) {
    }
    size_t prevO2y = g_o2yMax.load(std::memory_order_relaxed);
    while (o2y > prevO2y && !g_o2yMax.compare_exchange_weak(prevO2y, o2y, std::memory_order_relaxed)) {
    }
    LOG(RTLOG_ERROR,
        "[GCV2][tlraw] enter=%zu minor=%zu buffers=%zu tlSmall=%zu tlLarge=%zu "
        "o2ySmall=%zu o2yLarge=%zu walkAbort=%zu youngTl=%zu initNotes=%zu initSat=%zu "
        "env=MRT_GCV2_TLRAW=1",
        n, minorRun, buffers, tlSmall, tlLarge, o2ySmall, o2yLarge, walkAbort, youngTl,
        g_initNotes.load(std::memory_order_relaxed), g_initSat.load(std::memory_order_relaxed));
}

void NoteInitRegion(RegionInfo* region)
{
    if (!GateOn() || region == nullptr) {
        return;
    }
    EnsureAtexit();
    HealthOnce();
    uintptr_t start = region->GetRegionStart();
    InitRow* row = FindOrInsert(start);
    if (row == nullptr) {
        return;
    }
    row->inits.fetch_add(1, std::memory_order_relaxed);
    row->lastType.store(static_cast<uint32_t>(region->GetRegionType()), std::memory_order_relaxed);
    g_initNotes.fetch_add(1, std::memory_order_relaxed);
}

void NoteCrashRdi(uintptr_t rdi)
{
    if (!GateOn()) {
        return;
    }
    unsigned heap = 0;
    unsigned rtype = 255;
    unsigned young = 0;
    unsigned garbage = 0;
    unsigned freeReg = 0;
    uintptr_t rstart = 0;
    uintptr_t rend = 0;
    uintptr_t alloc = 0;
    uintptr_t off = 0;
    uint32_t inits = 0;
    uint64_t epoch = 0;
    unsigned atStart = 0;
    unsigned inAlloc = 0;
    if (Runtime::CurrentRef() != nullptr && rdi != 0 && Heap::IsHeapAddress(rdi)) {
        heap = 1;
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(rdi);
        if (region != nullptr) {
            rtype = static_cast<unsigned>(region->GetRegionType());
            young = region->IsYoungRegion() ? 1 : 0;
            garbage = region->IsGarbageRegion() ? 1 : 0;
            freeReg = region->IsFreeRegion() ? 1 : 0;
            rstart = region->GetRegionStart();
            rend = region->GetRegionEnd();
            alloc = region->GetRegionAllocPtr();
            off = rdi >= rstart ? rdi - rstart : 0;
            atStart = (rdi == rstart) ? 1 : 0;
            inAlloc = (rdi >= rstart && rdi < alloc) ? 1 : 0;
            epoch = region->GetSnapshotEpoch();
            InitRow* row = FindRow(rstart);
            if (row != nullptr) {
                inits = row->inits.load(std::memory_order_relaxed);
            }
        }
    }
    char line[512];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][tlraw] crash rdi=%#zx heap=%u regionType=%u young=%u garbage=%u "
                      "free=%u start=%#zx end=%#zx alloc=%#zx off=%#zx atStart=%u inAlloc=%u "
                      "inits=%u epoch=%llu env=MRT_GCV2_TLRAW=1\n",
                      rdi, heap, rtype, young, garbage, freeReg, rstart, rend, alloc, off, atStart,
                      inAlloc, inits, static_cast<unsigned long long>(epoch));
    if (n > 0) {
        WriteCrashLine(line, static_cast<size_t>(n));
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][tlraw] report point=%s enter=%zu initNotes=%zu initSat=%zu "
        "tlSmallMax=%zu tlLargeMax=%zu o2yMax=%zu o2yTotal=%zu env=MRT_GCV2_TLRAW=1",
        point != nullptr ? point : "none", g_enter.load(std::memory_order_relaxed),
        g_initNotes.load(std::memory_order_relaxed), g_initSat.load(std::memory_order_relaxed),
        g_tlSmallMax.load(std::memory_order_relaxed), g_tlLargeMax.load(std::memory_order_relaxed),
        g_o2yMax.load(std::memory_order_relaxed), g_o2yTotal.load(std::memory_order_relaxed));
}

} // namespace TlRawDiag
} // namespace MapleRuntime
