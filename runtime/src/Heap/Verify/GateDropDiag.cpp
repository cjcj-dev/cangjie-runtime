#include "Heap/Verify/GateDropDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/RefField.h"
#include "TypeInfoManager.h"
#include "securec.h"

namespace MapleRuntime {
namespace GateDropDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        if (EnvIsOne("MRT_GCV2_GATEDROP")) {
            return true;
        }
        return DiagGate::TokenOn("gatedrop");
    }();
    return on;
}

// Mirror product tip floors (Collector.cpp) for reason tags only — never used to change admit.
constexpr uintptr_t kMinPlausibleTypeInfoAddr = 0x100000000ULL;

inline bool TipLow32IsZero(uintptr_t tipAddr)
{
    return (tipAddr & 0xffffffffULL) == 0;
}

// Classify why PlausibleManagedObjectGate would reject (same predicate order as product).
// Returns static string; "admit" if would pass (should not happen on reject arm).
const char* ClassifyPlausible(BaseObject* obj)
{
    if (obj == nullptr) {
        return "null";
    }
    if (!Heap::IsHeapAddress(obj)) {
        return "non-heap";
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion() ||
        region->GetRegionType() == RegionInfo::RegionType::FREE_REGION) {
        return "dead-region";
    }
    TypeInfo* tip = obj->GetTypeInfo();
    uintptr_t tipAddr = reinterpret_cast<uintptr_t>(tip);
    if (tipAddr == 0) {
        return "null-tip";
    }
    if (tipAddr < kMinPlausibleTypeInfoAddr) {
        return "tip-small-int";
    }
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return "tip-misaligned";
    }
    if (TipLow32IsZero(tipAddr)) {
        return "tip-4g-aligned";
    }
    if (Heap::IsHeapAddress(tipAddr)) {
        return "tip-in-heap";
    }
    if (!TypeInfoManager::GetTypeInfoManager().IsResidentTypeInfoAddress(tipAddr)) {
        return "tip-not-resident";
    }
    return "admit";
}

const char* ArmName(uint8_t arm)
{
    switch (arm) {
        case ARM_MARKGOOD:
            return "markgood";
        case ARM_PLAUSIBLE_GOOD:
            return "plausible.good";
        case ARM_PLAUSIBLE_SLOW:
            return "plausible.slow";
        default:
            return "unknown";
    }
}

constexpr size_t kCap = 1u << 16; // 64k rows

struct Row {
    uintptr_t target;
    uintptr_t holder;
    uint32_t fieldOff;
    uint32_t gc;
    uint16_t phase;
    uint8_t arm;
    uint8_t pad0;
    // reason packed as first 15 bytes + NUL of a short tag
    char reason[16];
    uint32_t seq;
};

Row g_rows[kCap];
std::atomic<uint32_t> g_next{ 0 };
std::atomic<size_t> g_total{ 0 };
std::atomic<size_t> g_wrap{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

// reason tallies (coarse)
std::atomic<uint64_t> g_byArm[4]{};
std::atomic<uint64_t> g_rNonHeap{ 0 };
std::atomic<uint64_t> g_rDeadRegion{ 0 };
std::atomic<uint64_t> g_rNullTip{ 0 };
std::atomic<uint64_t> g_rTipSmall{ 0 };
std::atomic<uint64_t> g_rTipMis{ 0 };
std::atomic<uint64_t> g_rTip4g{ 0 };
std::atomic<uint64_t> g_rTipHeap{ 0 };
std::atomic<uint64_t> g_rTipNotRes{ 0 };
std::atomic<uint64_t> g_rNull{ 0 };
std::atomic<uint64_t> g_rOther{ 0 };
std::atomic<uint64_t> g_sampleLog{ 0 };

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR, "[GCV2][gatedrop] health probe_live=1 env=MRT_GCV2_GATEDROP=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

void TallyReason(const char* reason)
{
    if (reason == nullptr) {
        g_rOther.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (std::strcmp(reason, "markgood-non-heap") == 0 || std::strcmp(reason, "non-heap") == 0) {
        g_rNonHeap.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "dead-region") == 0) {
        g_rDeadRegion.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "null-tip") == 0) {
        g_rNullTip.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "tip-small-int") == 0) {
        g_rTipSmall.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "tip-misaligned") == 0) {
        g_rTipMis.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "tip-4g-aligned") == 0) {
        g_rTip4g.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "tip-in-heap") == 0) {
        g_rTipHeap.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "tip-not-resident") == 0) {
        g_rTipNotRes.fetch_add(1, std::memory_order_relaxed);
    } else if (std::strcmp(reason, "null") == 0) {
        g_rNull.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_rOther.fetch_add(1, std::memory_order_relaxed);
    }
}

void DumpRow(const char* tag, const Row& row)
{
    char line[512];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][gatedrop] %s target=%#zx holder=%#zx off=%u arm=%s(%u) reason=%s "
                      "gc=%u phase=%u seq=%u\n",
                      tag, row.target, row.holder, row.fieldOff, ArmName(row.arm),
                      static_cast<unsigned>(row.arm), row.reason, row.gc,
                      static_cast<unsigned>(row.phase), row.seq);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

} // namespace

bool Enabled() { return GateOn(); }

void NoteReject(BaseObject* holder, void* fieldPtr, BaseObject* target, uint8_t arm)
{
    if (LIKELY(!GateOn())) {
        return;
    }
    HealthOnce();
    EnsureAtexit();

    const char* reason = "unknown";
    if (arm == ARM_MARKGOOD) {
        reason = "markgood-non-heap";
    } else {
        reason = ClassifyPlausible(target);
    }

    if (arm < 4) {
        g_byArm[arm].fetch_add(1, std::memory_order_relaxed);
    }
    TallyReason(reason);

    uint32_t idx = g_next.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kCap) {
        g_wrap.fetch_add(1, std::memory_order_relaxed);
    }
    size_t total = g_total.fetch_add(1, std::memory_order_relaxed) + 1;
    Row& row = g_rows[idx % kCap];
    row.target = reinterpret_cast<uintptr_t>(target);
    row.holder = reinterpret_cast<uintptr_t>(holder);
    uint32_t off = 0xffffffffu;
    if (holder != nullptr && fieldPtr != nullptr && Heap::IsHeapAddress(holder)) {
        off = static_cast<uint32_t>(
            BaseObject::FieldOffset(holder, reinterpret_cast<RefField<>*>(fieldPtr)));
    }
    row.fieldOff = off;
    row.gc = static_cast<uint32_t>(g_gcCount.load(std::memory_order_relaxed));
    GCPhase phase = Heap::GetHeap().GetGCPhase();
    row.phase = static_cast<uint16_t>(phase);
    row.arm = arm;
    row.pad0 = 0;
    std::memset(row.reason, 0, sizeof(row.reason));
    if (reason != nullptr) {
        std::strncpy(row.reason, reason, sizeof(row.reason) - 1);
    }
    row.seq = static_cast<uint32_t>(total);

    // Rare sample logs (reject path only). Cap keeps stderr bounded.
    uint64_t s = g_sampleLog.fetch_add(1, std::memory_order_relaxed);
    if (s < 32) {
        char line[640];
        int n = sprintf_s(line, sizeof(line),
                          "[GCV2][gatedrop] REJECT n=%zu target=%p holder=%p off=%u arm=%s reason=%s "
                          "gc=%u phase=%s(%u)\n",
                          total, target, holder, off, ArmName(arm), reason, row.gc,
                          Collector::GetGCPhaseName(phase), static_cast<unsigned>(phase));
        if (n > 0) {
            WriteLine(line, static_cast<size_t>(n));
        }
    }
}

void NoteCrashJoin(uintptr_t holder, uintptr_t slotBytes, uintptr_t tgtPeeled)
{
    if (!GateOn()) {
        return;
    }
    size_t total = g_total.load(std::memory_order_acquire);
    uint32_t n = static_cast<uint32_t>(total < kCap ? total : kCap);
    uint32_t next = g_next.load(std::memory_order_acquire);
    uint32_t base = (total < kCap) ? 0 : (next % kCap);

    // Peel address bits (same mask as whozero tgt).
    uintptr_t tgt = tgtPeeled & 0x0000ffffffffffffULL;

    uint32_t hitTgt = 0;
    uint32_t hitHolder = 0;
    uint32_t hitHolderOff = 0;
    uint32_t lastIdx = 0;
    bool have = false;
    // Prefer exact target match; also count holder / holder+off for context.
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = (base + i) % kCap;
        const Row& row = g_rows[idx];
        uintptr_t rt = row.target & 0x0000ffffffffffffULL;
        if (tgt != 0 && rt == tgt) {
            ++hitTgt;
            lastIdx = idx;
            have = true;
        }
        if (holder != 0 && row.holder == holder) {
            ++hitHolder;
            if (slotBytes != 0 && holder != 0) {
                uintptr_t wantOff = slotBytes - holder;
                if (row.fieldOff == wantOff) {
                    ++hitHolderOff;
                    if (!have) {
                        lastIdx = idx;
                        have = true;
                    }
                }
            }
        }
    }

    char line[512];
    int ln = sprintf_s(line, sizeof(line),
                       "[GCV2][gatedrop] crash_join holder=%#zx slot=%#zx tgt=%#zx "
                       "rejectTotal=%zu hitTgt=%u hitHolder=%u hitHolderOff=%u "
                       "verdict=%s\n",
                       holder, slotBytes, tgt, total, hitTgt, hitHolder, hitHolderOff,
                       (hitTgt > 0) ? "甲_tgt_in_reject" :
                       (total == 0) ? "丙_reject_empty" :
                       (hitHolder > 0 || hitHolderOff > 0) ? "乙_other_reject" : "乙_no_tgt_match");
    if (ln > 0) {
        WriteLine(line, static_cast<size_t>(ln));
    }
    if (have) {
        DumpRow("crash_hit", g_rows[lastIdx]);
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][gatedrop] point=%s total=%zu wrap=%zu "
                      "arm_markgood=%llu arm_plaus_good=%llu arm_plaus_slow=%llu "
                      "r_null=%llu r_nonheap=%llu r_dead=%llu r_nulltip=%llu "
                      "r_tipsmall=%llu r_tipmis=%llu r_tip4g=%llu r_tipheap=%llu "
                      "r_tipnotres=%llu r_other=%llu env=MRT_GCV2_GATEDROP=1\n",
                      point == nullptr ? "?" : point, g_total.load(std::memory_order_relaxed),
                      g_wrap.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(g_byArm[1].load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_byArm[2].load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_byArm[3].load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rNull.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rNonHeap.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rDeadRegion.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rNullTip.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rTipSmall.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rTipMis.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rTip4g.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rTipHeap.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rTipNotRes.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_rOther.load(std::memory_order_relaxed)));
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

} // namespace GateDropDiag
} // namespace MapleRuntime
