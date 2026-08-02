// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

#include "EmitSiteCounters.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/TypeInfo.h"

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

namespace {
constexpr size_t ATTRIBUTION_SHARD_COUNT = 64;

struct AttributionEntry {
    BaseObject* target;
    TypeInfo* holderType;
    void* pc;
    EmitBarrierKind barrierKind;
    bool fastPath;
    uint64_t sequence;
};

struct AttributionShard {
    std::mutex mutex;
    std::unordered_map<MAddress, AttributionEntry> entries;
};

std::array<AttributionShard, ATTRIBUTION_SHARD_COUNT>& AttributionShards()
{
    static std::array<AttributionShard, ATTRIBUTION_SHARD_COUNT> shards;
    return shards;
}

std::atomic<uint64_t> attributionRecords{0};
std::atomic<uint64_t> attributionQueries{0};
std::atomic<uint64_t> attributionHits{0};
std::atomic<uint64_t> attributionNoSlot{0};
std::atomic<uint64_t> attributionTargetMismatch{0};
std::atomic<uint64_t> attributionTypeMismatch{0};
std::atomic<uint64_t> attributionPositiveControl{0};

int AttributionMode()
{
    static int mode = []() {
        const char* value = std::getenv("MRT_EMIT_ATTRIBUTION");
        if (value == nullptr) {
            return -1;
        }
        if (std::strcmp(value, "0") == 0) {
            return 0;
        }
        if (std::strcmp(value, "1") == 0) {
            return 1;
        }
        return -1;
    }();
    return mode;
}

bool AttributionPositiveControlEnabled()
{
    static bool enabled = []() {
        const char* value = std::getenv("MRT_EMIT_ATTRIBUTION_POSCTRL");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

const char* KindName(int kind)
{
    static const char* names[] = {"Idle", "IdleLog", "Enum", "Trace", "PostTrace", "Preforward", "Forward"};
    if (kind < 0 || kind >= EmitSiteCounters::K) {
        return "?";
    }
    return names[kind];
}
} // namespace

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

bool EmitSiteCounters::IsAttributionConfigured()
{
    return AttributionMode() >= 0;
}

bool EmitSiteCounters::IsAttributionEnabled()
{
    return AttributionMode() == 1;
}

void EmitSiteCounters::NoteAttribution(BaseObject* holder, void* slot, BaseObject* ref, bool fastPath, void* pc)
{
    if (!IsAttributionEnabled() || holder == nullptr || slot == nullptr || ref == nullptr ||
        !Heap::IsHeapAddress(holder) || !Heap::IsHeapAddress(ref)) {
        return;
    }
    MAddress slotAddress = reinterpret_cast<MAddress>(slot);
    MAddress holderAddress = reinterpret_cast<MAddress>(holder);
    size_t holderSize = holder->GetSize();
    if (slotAddress < holderAddress || slotAddress + sizeof(MAddress) > holderAddress + holderSize) {
        return;
    }
    uint64_t sequence = attributionRecords.fetch_add(1, std::memory_order_relaxed) + 1;
    auto kind = static_cast<EmitBarrierKind>(EmitSiteActiveKind().load(std::memory_order_relaxed));
    AttributionShard& shard = AttributionShards()[std::hash<MAddress>{}(slotAddress) % ATTRIBUTION_SHARD_COUNT];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.entries[slotAddress] = AttributionEntry{ ref, holder->GetTypeInfo(), pc, kind, fastPath, sequence };
    }
    if (AttributionPositiveControlEnabled() &&
        attributionPositiveControl.fetch_add(1, std::memory_order_relaxed) == 0) {
        TypeInfo* type = holder->GetTypeInfo();
        VLOG(REPORT,
             "[EmitAttr] POSCTRL n=1 writeAddress=%p holderType=%s target=%p barrier=%s fastpath=%u pc=%p",
             slot, type == nullptr ? "<unknown>" : type->GetName(), ref, KindName(static_cast<int>(kind)),
             static_cast<unsigned>(fastPath), pc);
    }
}

void EmitSiteCounters::ReportAttribution(BaseObject* holder, MAddress slot, BaseObject* ref)
{
    if (!IsAttributionEnabled()) {
        return;
    }
    attributionQueries.fetch_add(1, std::memory_order_relaxed);
    AttributionEntry entry{};
    bool found = false;
    AttributionShard& shard = AttributionShards()[std::hash<MAddress>{}(slot) % ATTRIBUTION_SHARD_COUNT];
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.entries.find(slot);
        if (it != shard.entries.end()) {
            entry = it->second;
            found = true;
        }
    }
    TypeInfo* holderType = holder == nullptr ? nullptr : holder->GetTypeInfo();
    const char* holderTypeName = holderType == nullptr ? "<unknown>" : holderType->GetName();
    if (!found) {
        attributionNoSlot.fetch_add(1, std::memory_order_relaxed);
        VLOG(REPORT, "[EmitAttr] MISS status=no_slot writeAddress=%p holderType=%s target=%p",
             reinterpret_cast<void*>(slot), holderTypeName, ref);
        return;
    }
    if (entry.target != ref) {
        attributionTargetMismatch.fetch_add(1, std::memory_order_relaxed);
        VLOG(REPORT,
             "[EmitAttr] MISS status=target_mismatch writeAddress=%p holderType=%s target=%p "
             "recordTarget=%p pc=%p sequence=%llu",
             reinterpret_cast<void*>(slot), holderTypeName, ref, entry.target, entry.pc,
             static_cast<unsigned long long>(entry.sequence));
        return;
    }
    if (entry.holderType != holderType) {
        attributionTypeMismatch.fetch_add(1, std::memory_order_relaxed);
        VLOG(REPORT,
             "[EmitAttr] MISS status=type_mismatch writeAddress=%p holderType=%s target=%p pc=%p sequence=%llu",
             reinterpret_cast<void*>(slot), holderTypeName, ref, entry.pc,
             static_cast<unsigned long long>(entry.sequence));
        return;
    }

    const char* module = "<unavailable>";
    const char* symbol = "<unknown>";
    void* moduleBase = nullptr;
    void* symbolAddress = nullptr;
#if !defined(_WIN32)
    Dl_info info{};
    if (entry.pc != nullptr && dladdr(entry.pc, &info) != 0) {
        module = info.dli_fname == nullptr ? "<unknown>" : info.dli_fname;
        symbol = info.dli_sname == nullptr ? "<unknown>" : info.dli_sname;
        moduleBase = info.dli_fbase;
        symbolAddress = info.dli_saddr;
    }
#endif
    uintptr_t pcValue = reinterpret_cast<uintptr_t>(entry.pc);
    uintptr_t moduleOffset = moduleBase == nullptr ? 0 : pcValue - reinterpret_cast<uintptr_t>(moduleBase);
    uintptr_t symbolOffset = symbolAddress == nullptr ? 0 : pcValue - reinterpret_cast<uintptr_t>(symbolAddress);
    attributionHits.fetch_add(1, std::memory_order_relaxed);
    VLOG(REPORT,
         "[EmitAttr] MISS status=attributed writeAddress=%p holderType=%s target=%p barrier=%s fastpath=%u "
         "pc=%p module=%s moduleBase=%p moduleOffset=%#zx symbol=%s symbolOffset=%#zx sequence=%llu",
         reinterpret_cast<void*>(slot), holderTypeName, ref, KindName(static_cast<int>(entry.barrierKind)),
         static_cast<unsigned>(entry.fastPath), entry.pc, module, moduleBase, moduleOffset, symbol, symbolOffset,
         static_cast<unsigned long long>(entry.sequence));
}

void EmitSiteCounters::DumpAttribution()
{
    if (!IsAttributionEnabled()) {
        return;
    }
    VLOG(REPORT,
         "[EmitAttr] SUMMARY records=%llu queries=%llu attributed=%llu no_slot=%llu target_mismatch=%llu "
         "type_mismatch=%llu posctrl=%llu",
         static_cast<unsigned long long>(attributionRecords.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(attributionQueries.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(attributionHits.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(attributionNoSlot.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(attributionTargetMismatch.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(attributionTypeMismatch.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(attributionPositiveControl.load(std::memory_order_relaxed)));
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
    DumpAttribution();
}

} // namespace MapleRuntime
