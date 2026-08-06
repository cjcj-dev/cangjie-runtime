// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B4BenignProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MClass.inline.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
namespace {

#define B4B_LOG(fmt, ...)                                                                                              \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][b4benign] " fmt "\n", ##__VA_ARGS__);                                              \
        std::fflush(stderr);                                                                                           \
    } while (0)

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

bool PageMapped(uintptr_t addr)
{
    if (addr == 0) {
        return false;
    }
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        pageSize = 4096;
    }
    uintptr_t page = addr & ~(static_cast<uintptr_t>(pageSize) - 1);
    unsigned char vec = 0;
    if (mincore(reinterpret_cast<void*>(page), static_cast<size_t>(pageSize), &vec) == 0) {
        return true;
    }
    return false;
}

TypeInfo* PeekTypeInfoAt(uintptr_t addr)
{
    if (addr == 0 || (addr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return nullptr;
    }
    if (!Heap::IsHeapAddress(addr)) {
        return nullptr;
    }
#ifdef __arm__
    uint32_t raw = 0;
    std::memcpy(&raw, reinterpret_cast<const void*>(addr), sizeof(raw));
    return reinterpret_cast<TypeInfo*>(static_cast<uintptr_t>(raw));
#else
    uint32_t low = 0;
    uint16_t high = 0;
    std::memcpy(&low, reinterpret_cast<const void*>(addr), sizeof(low));
    std::memcpy(&high, reinterpret_cast<const void*>(addr + 4), sizeof(high));
    uintptr_t tipAddr = (static_cast<uintptr_t>(high) << 32) | static_cast<uintptr_t>(low);
    return reinterpret_cast<TypeInfo*>(tipAddr);
#endif
}

bool TipLooksValid(TypeInfo* tip)
{
    if (tip == nullptr) {
        return false;
    }
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    bool inTim = TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipAddr);
    if (!inTim && !PageMapped(tipAddr)) {
        return false;
    }
    if (!tip->IsVaildType()) {
        return false;
    }
    MSize isz = tip->GetInstanceSize();
    if (isz == 0 || isz > (1u << 20)) {
        return false;
    }
    return true;
}

size_t SaneObjectSize(TypeInfo* tip, RegionInfo* region)
{
    if (tip == nullptr || region == nullptr) {
        return 0;
    }
    MSize isz = tip->GetInstanceSize();
    size_t size = (static_cast<size_t>(isz) + 8u + 7u) & ~static_cast<size_t>(7u);
    size_t regionBytes = region->GetRegionEnd() - region->GetRegionStart();
    if (size < 16 || size > regionBytes || size > (1u << 20)) {
        return 0;
    }
    return size;
}

// Classify value as base vs interior@16 (base-first; only -16 for this probe's gold set).
enum class Kind : uint8_t { Base = 0, Interior16 = 1, InteriorOther = 2, Unknown = 3, NotHeap = 4 };

Kind ClassifyInterior16(uintptr_t value, uintptr_t& baseOut, TypeInfo*& tipAtBase)
{
    baseOut = 0;
    tipAtBase = nullptr;
    if (!Heap::IsHeapAddress(value)) {
        return Kind::NotHeap;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(value);
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        return Kind::Unknown;
    }
    TypeInfo* tipVal = PeekTypeInfoAt(value);
    if (TipLooksValid(tipVal)) {
        baseOut = value;
        tipAtBase = tipVal;
        return Kind::Base;
    }
    // only check -16 for B-4 gold (100% of remset interiors in dumps)
    if (value >= 16) {
        uintptr_t cand = value - 16;
        if (Heap::IsHeapAddress(cand)) {
            RegionInfo* cr = RegionInfo::TryGetRegionInfoAt(cand);
            if (cr == region) {
                TypeInfo* tip = PeekTypeInfoAt(cand);
                if (TipLooksValid(tip)) {
                    size_t size = SaneObjectSize(tip, region);
                    bool sizeOk = (size != 0 && value >= cand && value < cand + size);
                    if (!sizeOk && size == 0) {
                        sizeOk = true; // allow tip-valid base even if size probe soft-fails
                    }
                    if (sizeOk) {
                        baseOut = cand;
                        tipAtBase = tip;
                        return Kind::Interior16;
                    }
                }
            }
        }
    }
    return Kind::Unknown;
}

// Read raw instanceSize@TI+12 without requiring valid TypeInfo methods (code-as-TI path).
bool RawInstSize(uintptr_t tipAddr, uint32_t& instOut, int8_t& typeOut)
{
    instOut = 0;
    typeOut = 0;
    if (tipAddr == 0 || !PageMapped(tipAddr)) {
        return false;
    }
    std::memcpy(&typeOut, reinterpret_cast<const void*>(tipAddr + 8), 1);
    std::memcpy(&instOut, reinterpret_cast<const void*>(tipAddr + 12), 4);
    return true;
}

// Mirror RegionInfo::CheckObjectSize (RegionInfo.h:1270-1277).
const char* SizeGuardName(size_t objSize, uintptr_t objAddr, RegionInfo* region)
{
    if (region == nullptr) {
        return "NO_REGION";
    }
    uintptr_t rStart = region->GetRegionStart();
    uintptr_t rEnd = region->GetRegionEnd();
    if (objSize == 0) {
        return "FAIL_ZERO";
    }
    if ((objSize % 8) != 0) {
        return "FAIL_UNALIGNED";
    }
    if (objSize > (rEnd - objAddr)) {
        return "FAIL_OVERSIZE";
    }
    return "PASS";
}

std::atomic<uint64_t> gConsumeTotal{0};
std::atomic<uint64_t> gBase{0};
std::atomic<uint64_t> gInterior16{0};
std::atomic<uint64_t> gUnknown{0};
std::atomic<uint64_t> gNotHeap{0};

// Interior16 fate buckets
std::atomic<uint64_t> gI16ValidNullTip{0};   // IsValidObject false (tip==0)
std::atomic<uint64_t> gI16ValidNonNull{0};   // IsValidObject true
std::atomic<uint64_t> gI16TipHeap{0};        // tip in managed heap (not a TypeInfo)
std::atomic<uint64_t> gI16TipCode{0};        // tip in executable mapping
std::atomic<uint64_t> gI16TipStaticOk{0};    // tip looks like real TypeInfo
std::atomic<uint64_t> gI16TipOther{0};

std::atomic<uint64_t> gI16GuardPass{0};
std::atomic<uint64_t> gI16GuardFailOversize{0};
std::atomic<uint64_t> gI16GuardFailZero{0};
std::atomic<uint64_t> gI16GuardFailUnaligned{0};
std::atomic<uint64_t> gI16GuardNoSize{0}; // could not read tip as size
std::atomic<uint64_t> gI16MagicAE{0};     // recon == 1200310576
std::atomic<uint64_t> gI16Recon8{0};
std::atomic<uint64_t> gI16Recon2p32{0};

std::atomic<uint64_t> gI16Young{0};
std::atomic<uint64_t> gI16Old{0};
std::atomic<uint64_t> gI16WouldMark{0}; // young + valid + would reach MarkObject

std::atomic<uint64_t> gDumpLeft{0};
std::atomic<bool> gArmed{false};

std::mutex gLastMu;
uintptr_t gLastP = 0;
uintptr_t gLastBase = 0;
size_t gLastRecon = 0;
char gLastGuard[32] = {};
char gLastTipClass[32] = {};

} // namespace

bool B4BenignProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B4BENIGN");
    return on;
}

void B4BenignProbe::NoteConsumeAsBase(void* object, const char* site)
{
    if (!Enabled()) {
        return;
    }
    if (!gArmed.exchange(true, std::memory_order_relaxed)) {
        gDumpLeft.store(EnvSizeT("MRT_GCV2_B4BENIGN_DUMP_MAX", 32), std::memory_order_relaxed);
        B4B_LOG("ARMED site=%s dump_max=%zu", site != nullptr ? site : "?",
                static_cast<size_t>(gDumpLeft.load(std::memory_order_relaxed)));
    }
    gConsumeTotal.fetch_add(1, std::memory_order_relaxed);
    uintptr_t value = reinterpret_cast<uintptr_t>(object);
    uintptr_t base = 0;
    TypeInfo* tipBase = nullptr;
    Kind k = ClassifyInterior16(value, base, tipBase);
    if (k == Kind::Base) {
        gBase.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (k == Kind::NotHeap) {
        gNotHeap.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (k != Kind::Interior16) {
        gUnknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    gInterior16.fetch_add(1, std::memory_order_relaxed);

    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(value);
    bool young = region != nullptr && region->IsYoungRegion();
    if (young) {
        gI16Young.fetch_add(1, std::memory_order_relaxed);
    } else {
        gI16Old.fetch_add(1, std::memory_order_relaxed);
    }

    // What TraceYoungClosure sees at P as object header
    TypeInfo* tipAtP = PeekTypeInfoAt(value);
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tipAtP);
    const char* tipClass = "null";
    if (tipAtP == nullptr || tipAddr == 0) {
        gI16ValidNullTip.fetch_add(1, std::memory_order_relaxed);
        tipClass = "null_tip";
    } else {
        gI16ValidNonNull.fetch_add(1, std::memory_order_relaxed);
        if (Heap::IsHeapAddress(tipAddr)) {
            gI16TipHeap.fetch_add(1, std::memory_order_relaxed);
            tipClass = "heap_ptr";
        } else if (TipLooksValid(tipAtP)) {
            gI16TipStaticOk.fetch_add(1, std::memory_order_relaxed);
            tipClass = "static_ti";
        } else {
            // code or garbage: try raw size
            Dl_info info;
            if (dladdr(reinterpret_cast<void*>(tipAddr), &info) != 0 && info.dli_fname != nullptr) {
                gI16TipCode.fetch_add(1, std::memory_order_relaxed);
                tipClass = "code";
            } else {
                gI16TipOther.fetch_add(1, std::memory_order_relaxed);
                tipClass = "other";
            }
        }
    }

    // Reconstruct GetSize the way MarkObject would if IsValidObject passed
    size_t recon = 0;
    bool haveSize = false;
    if (tipAddr != 0) {
        uint32_t inst = 0;
        int8_t ty = 0;
        if (RawInstSize(tipAddr, inst, ty)) {
            recon = (static_cast<size_t>(inst) + 8u + 7u) & ~static_cast<size_t>(7u);
            haveSize = true;
            if (recon == 1200310576u) {
                gI16MagicAE.fetch_add(1, std::memory_order_relaxed);
            }
            if (recon == 8) {
                gI16Recon8.fetch_add(1, std::memory_order_relaxed);
            }
            if (recon == 4294967304ull) {
                gI16Recon2p32.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const char* guard = "NO_SIZE";
    if (!haveSize) {
        gI16GuardNoSize.fetch_add(1, std::memory_order_relaxed);
    } else {
        guard = SizeGuardName(recon, value, region);
        if (std::strcmp(guard, "PASS") == 0) {
            gI16GuardPass.fetch_add(1, std::memory_order_relaxed);
        } else if (std::strcmp(guard, "FAIL_OVERSIZE") == 0) {
            gI16GuardFailOversize.fetch_add(1, std::memory_order_relaxed);
        } else if (std::strcmp(guard, "FAIL_ZERO") == 0) {
            gI16GuardFailZero.fetch_add(1, std::memory_order_relaxed);
        } else if (std::strcmp(guard, "FAIL_UNALIGNED") == 0) {
            gI16GuardFailUnaligned.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Would reach MarkObject: young && IsValidObject (non-null tip). sizeguard may still abort.
    if (young && tipAddr != 0) {
        gI16WouldMark.fetch_add(1, std::memory_order_relaxed);
    }

    {
        std::lock_guard<std::mutex> lock(gLastMu);
        gLastP = value;
        gLastBase = base;
        gLastRecon = recon;
        std::snprintf(gLastGuard, sizeof(gLastGuard), "%s", guard);
        std::snprintf(gLastTipClass, sizeof(gLastTipClass), "%s", tipClass);
    }

    uint64_t left = gDumpLeft.load(std::memory_order_relaxed);
    while (left > 0) {
        if (gDumpLeft.compare_exchange_weak(left, left - 1, std::memory_order_relaxed)) {
            const char* tipName = "?";
            if (tipBase != nullptr) {
                tipName = tipBase->GetName();
                if (tipName == nullptr) {
                    tipName = "?";
                }
            }
            B4B_LOG("CONSUME_I16 site=%s P=%p base=%p tipBaseName=%s tipClass=%s recon=%zu guard=%s young=%u "
                    "truebase_ti=%p",
                    site != nullptr ? site : "?", reinterpret_cast<void*>(value), reinterpret_cast<void*>(base),
                    tipName, tipClass, recon, guard, young ? 1u : 0u, static_cast<void*>(tipBase));
            break;
        }
    }
}

void B4BenignProbe::FlushSummary(const char* site)
{
    if (!Enabled()) {
        return;
    }
    uint64_t total = gConsumeTotal.load(std::memory_order_relaxed);
    uint64_t i16 = gInterior16.load(std::memory_order_relaxed);
    uint64_t pass = gI16GuardPass.load(std::memory_order_relaxed);
    uint64_t failOs = gI16GuardFailOversize.load(std::memory_order_relaxed);
    uint64_t nullTip = gI16ValidNullTip.load(std::memory_order_relaxed);
    uint64_t nonNull = gI16ValidNonNull.load(std::memory_order_relaxed);
    uint64_t wouldMark = gI16WouldMark.load(std::memory_order_relaxed);
    uint64_t magic = gI16MagicAE.load(std::memory_order_relaxed);
    B4B_LOG("SUMMARY site=%s consume=%llu base=%llu i16=%llu unknown=%llu not_heap=%llu",
            site != nullptr ? site : "?", static_cast<unsigned long long>(total),
            static_cast<unsigned long long>(gBase.load()), static_cast<unsigned long long>(i16),
            static_cast<unsigned long long>(gUnknown.load()), static_cast<unsigned long long>(gNotHeap.load()));
    B4B_LOG("I16_FATE valid_null_tip=%llu valid_nonnull=%llu tip_heap=%llu tip_code=%llu tip_static=%llu tip_other=%llu",
            static_cast<unsigned long long>(nullTip), static_cast<unsigned long long>(nonNull),
            static_cast<unsigned long long>(gI16TipHeap.load()), static_cast<unsigned long long>(gI16TipCode.load()),
            static_cast<unsigned long long>(gI16TipStaticOk.load()),
            static_cast<unsigned long long>(gI16TipOther.load()));
    B4B_LOG("I16_GUARD pass=%llu fail_oversize=%llu fail_zero=%llu fail_unaligned=%llu no_size=%llu magicAE=%llu "
            "recon8=%llu recon2p32=%llu",
            static_cast<unsigned long long>(pass), static_cast<unsigned long long>(failOs),
            static_cast<unsigned long long>(gI16GuardFailZero.load()),
            static_cast<unsigned long long>(gI16GuardFailUnaligned.load()),
            static_cast<unsigned long long>(gI16GuardNoSize.load()), static_cast<unsigned long long>(magic),
            static_cast<unsigned long long>(gI16Recon8.load()),
            static_cast<unsigned long long>(gI16Recon2p32.load()));
    B4B_LOG("I16_REGION young=%llu old=%llu would_mark=%llu",
            static_cast<unsigned long long>(gI16Young.load()), static_cast<unsigned long long>(gI16Old.load()),
            static_cast<unsigned long long>(wouldMark));
    // Machine-readable verdict fragments
    B4B_LOG("VERDICT_FRAG i16=%llu guard_pass=%llu guard_fail_os=%llu null_tip=%llu magic=%llu would_mark=%llu",
            static_cast<unsigned long long>(i16), static_cast<unsigned long long>(pass),
            static_cast<unsigned long long>(failOs), static_cast<unsigned long long>(nullTip),
            static_cast<unsigned long long>(magic), static_cast<unsigned long long>(wouldMark));
    {
        std::lock_guard<std::mutex> lock(gLastMu);
        if (gLastP != 0) {
            B4B_LOG("LAST_I16 P=%p base=%p recon=%zu guard=%s tipClass=%s", reinterpret_cast<void*>(gLastP),
                    reinterpret_cast<void*>(gLastBase), gLastRecon, gLastGuard, gLastTipClass);
        }
    }
}

} // namespace MapleRuntime
