// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include "EmitSiteCounters.h"

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"

namespace MapleRuntime {

std::atomic<uint64_t> EmitSiteCounters::storeAny[K] = {};
std::atomic<uint64_t> EmitSiteCounters::storeOldToYoung[K] = {};
std::atomic<uint64_t> EmitSiteCounters::stickyLogged[K] = {};
std::atomic<uint64_t> EmitSiteCounters::storeHolderYoungRefYoung[K] = {};
std::atomic<uint64_t> EmitSiteCounters::oldToYoungAge0[K] = {};
std::atomic<uint64_t> EmitSiteCounters::oldToYoungAgeN[K] = {};
std::atomic<uint64_t> EmitSiteCounters::stickyLineCalls{0};
std::atomic<uint64_t> EmitSiteCounters::stickyLineNew{0};
std::atomic<uint64_t> EmitSiteCounters::stickyLineOnYoung{0};
std::atomic<uint64_t> EmitSiteCounters::stickyLineOnOld{0};

void EmitSiteCounters::NoteWrite(EmitBarrierKind kind, BaseObject* holder, BaseObject* ref, bool didSticky)
{
    int k = static_cast<int>(kind);
    if (k < 0 || k >= K) {
        return;
    }
    if (holder == nullptr || !Heap::IsHeapAddress(holder)) {
        return;
    }
    storeAny[k].fetch_add(1, std::memory_order_relaxed);
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) {
        return;
    }
    RegionInfo* hReg = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(holder));
    RegionInfo* rReg = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(ref));
    if (hReg == nullptr || rReg == nullptr) {
        return;
    }
    bool holderYoung = hReg->IsYoungRegion();
    bool refYoung = rReg->IsYoungRegion();
    if (holderYoung && refYoung) {
        storeHolderYoungRefYoung[k].fetch_add(1, std::memory_order_relaxed);
    }
    if (!holderYoung && refYoung) {
        storeOldToYoung[k].fetch_add(1, std::memory_order_relaxed);
        if (hReg->GetYoungAge() == 0) {
            oldToYoungAge0[k].fetch_add(1, std::memory_order_relaxed);
        } else {
            oldToYoungAgeN[k].fetch_add(1, std::memory_order_relaxed);
        }
        if (didSticky) {
            stickyLogged[k].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void EmitSiteCounters::NoteStickyLogLine(BaseObject* object, bool newlyLogged)
{
    if (object == nullptr || !Heap::IsHeapAddress(object)) {
        return;
    }
    stickyLineCalls.fetch_add(1, std::memory_order_relaxed);
    if (newlyLogged) {
        stickyLineNew.fetch_add(1, std::memory_order_relaxed);
    }
    RegionInfo* reg = RegionInfo::GetRegionInfoAt(reinterpret_cast<uintptr_t>(object));
    if (reg == nullptr) {
        return;
    }
    if (reg->IsYoungRegion()) {
        stickyLineOnYoung.fetch_add(1, std::memory_order_relaxed);
    } else {
        stickyLineOnOld.fetch_add(1, std::memory_order_relaxed);
    }
}

void EmitSiteCounters::Reset()
{
    for (int i = 0; i < K; ++i) {
        storeAny[i].store(0, std::memory_order_relaxed);
        storeOldToYoung[i].store(0, std::memory_order_relaxed);
        stickyLogged[i].store(0, std::memory_order_relaxed);
        storeHolderYoungRefYoung[i].store(0, std::memory_order_relaxed);
        oldToYoungAge0[i].store(0, std::memory_order_relaxed);
        oldToYoungAgeN[i].store(0, std::memory_order_relaxed);
    }
    stickyLineCalls.store(0, std::memory_order_relaxed);
    stickyLineNew.store(0, std::memory_order_relaxed);
    stickyLineOnYoung.store(0, std::memory_order_relaxed);
    stickyLineOnOld.store(0, std::memory_order_relaxed);
}

static const char* KindName(int k)
{
    static const char* names[] = {"Idle", "IdleLog", "Enum", "Trace", "PostTrace", "Preforward", "Forward"};
    if (k < 0 || k >= EmitSiteCounters::K) {
        return "?";
    }
    return names[k];
}

void EmitSiteCounters::Dump(const char* tag)
{
    VLOG(REPORT, "[EmitSite] dump tag=%s", tag != nullptr ? tag : "");
    for (int i = 0; i < K; ++i) {
        uint64_t any = storeAny[i].load(std::memory_order_relaxed);
        uint64_t o2y = storeOldToYoung[i].load(std::memory_order_relaxed);
        uint64_t sticky = stickyLogged[i].load(std::memory_order_relaxed);
        uint64_t y2y = storeHolderYoungRefYoung[i].load(std::memory_order_relaxed);
        uint64_t gap = o2y > sticky ? o2y - sticky : 0;
        VLOG(REPORT,
             "[EmitSite] BARRIER=%s STORE_ANY=%llu STORE_OLD_TO_YOUNG=%llu STICKY_LOGGED=%llu "
             "GAP=%llu STORE_HOLDER_YOUNG_REF_YOUNG=%llu O2Y_AGE0=%llu O2Y_AGEN=%llu "
             "COUNTER_EXERCISED=%s",
             KindName(i), static_cast<unsigned long long>(any), static_cast<unsigned long long>(o2y),
             static_cast<unsigned long long>(sticky), static_cast<unsigned long long>(gap),
             static_cast<unsigned long long>(y2y),
             static_cast<unsigned long long>(oldToYoungAge0[i].load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(oldToYoungAgeN[i].load(std::memory_order_relaxed)),
             any != 0 ? "yes" : "no");
    }
    VLOG(REPORT,
         "[EmitSite] STICKY_LINE calls=%llu new=%llu onYoung=%llu onOld=%llu",
         static_cast<unsigned long long>(stickyLineCalls.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(stickyLineNew.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(stickyLineOnYoung.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(stickyLineOnOld.load(std::memory_order_relaxed)));
}

} // namespace MapleRuntime
