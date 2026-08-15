#include "Heap/Verify/HeldFreeDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Common/Runtime.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "Heap/Verify/TraceClear.h"
#include "securec.h"

namespace MapleRuntime {
namespace HeldFreeDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return EnvIsOne("MRT_GCV2_HELDFREE") || DiagGate::TokenOn("heldfree");
    }();
    return on;
}

constexpr size_t kRingCap = 1u << 16;
constexpr size_t kSlotCap = 1u << 18;

enum Kind : uint16_t {
    KIND_ENUM = 1,
    KIND_PUSH = 2,
    KIND_MARK = 3,
    KIND_FIX = 4,
};

struct Rec {
    uintptr_t slot = 0;
    uintptr_t target = 0;
    uint32_t seq = 0;
    uint16_t kind = 0;
    uint16_t wrote = 0;
    const char* site = nullptr;
};

struct SlotFace {
    Rec lastEnum{};
    Rec lastFix{};
};

Rec g_ring[kRingCap];
std::atomic<uint32_t> g_ringNext{ 0 };
SlotFace g_slots[kSlotCap];
std::atomic<uint32_t> g_seq{ 0 };
std::atomic<size_t> g_enumN{ 0 };
std::atomic<size_t> g_pushN{ 0 };
std::atomic<size_t> g_markN{ 0 };
std::atomic<size_t> g_fixN{ 0 };
std::atomic<size_t> g_fixWroteN{ 0 };
std::atomic<size_t> g_clearN{ 0 };
std::atomic<size_t> g_clearHeldN{ 0 };
std::atomic<size_t> g_clearJiaN{ 0 };
std::atomic<size_t> g_clearYiN{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

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
        LOG(RTLOG_ERROR, "[GCV2][heldfree] health probe_live=1 env=MRT_GCV2_HELDFREE=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

uintptr_t Peel(uintptr_t raw)
{
    return raw & 0xffffffffffffULL;
}

uintptr_t AsAddr(BaseObject* object)
{
    return Peel(reinterpret_cast<uintptr_t>(object));
}

size_t SlotIdx(uintptr_t slot)
{
    return (slot >> 3) & (kSlotCap - 1);
}

void PushRing(const Rec& rec)
{
    uint32_t i = g_ringNext.fetch_add(1, std::memory_order_relaxed);
    g_ring[i & (kRingCap - 1)] = rec;
}

void StoreEnum(uintptr_t slot, const Rec& rec)
{
    if (slot == 0) {
        return;
    }
    size_t idx = SlotIdx(slot);
    for (size_t n = 0; n < 8; ++n) {
        size_t i = (idx + n) & (kSlotCap - 1);
        uintptr_t cur = g_slots[i].lastEnum.slot;
        if (cur == 0 || cur == slot) {
            g_slots[i].lastEnum = rec;
            return;
        }
    }
    g_slots[idx].lastEnum = rec;
}

void StoreFix(uintptr_t slot, const Rec& rec)
{
    if (slot == 0) {
        return;
    }
    size_t idx = SlotIdx(slot);
    for (size_t n = 0; n < 8; ++n) {
        size_t i = (idx + n) & (kSlotCap - 1);
        uintptr_t cur = g_slots[i].lastFix.slot;
        if (cur == 0 || cur == slot) {
            g_slots[i].lastFix = rec;
            return;
        }
    }
    g_slots[idx].lastFix = rec;
}

bool FindSlot(uintptr_t slot, Rec* enumRec, Rec* fixRec)
{
    if (slot == 0) {
        return false;
    }
    size_t idx = SlotIdx(slot);
    bool found = false;
    for (size_t n = 0; n < 8; ++n) {
        size_t i = (idx + n) & (kSlotCap - 1);
        if (g_slots[i].lastEnum.slot == slot) {
            *enumRec = g_slots[i].lastEnum;
            found = true;
        }
        if (g_slots[i].lastFix.slot == slot) {
            *fixRec = g_slots[i].lastFix;
            found = true;
        }
        if (g_slots[i].lastEnum.slot == 0 && g_slots[i].lastFix.slot == 0) {
            break;
        }
    }
    return found;
}

bool WasMarked(uintptr_t obj)
{
    if (obj == 0) {
        return false;
    }
    uint32_t seq = g_seq.load(std::memory_order_relaxed);
    uint32_t n = g_ringNext.load(std::memory_order_relaxed);
    uint32_t scan = n < kRingCap ? n : kRingCap;
    for (uint32_t i = 0; i < scan; ++i) {
        const Rec& rec = g_ring[(n - 1 - i) & (kRingCap - 1)];
        if (rec.kind == KIND_MARK && rec.target == obj && rec.seq + 1 >= seq) {
            return true;
        }
    }
    return false;
}

void DumpRec(const char* tag, const Rec& rec)
{
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][heldfree] %s slot=%#zx target=%#zx seq=%u kind=%u wrote=%u site=%s\n", tag, rec.slot,
                      rec.target, rec.seq, rec.kind, rec.wrote, rec.site != nullptr ? rec.site : "none");
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void DumpRegion(const char* tag, uintptr_t addr)
{
    if (addr < 0x1000 || !Heap::IsHeapAddress(reinterpret_cast<void*>(addr))) {
        char line[192];
        int n = sprintf_s(line, sizeof(line), "[GCV2][heldfree] %s addr=%#zx heap=0\n", tag, addr);
        if (n > 0) {
            WriteLine(line, static_cast<size_t>(n));
        }
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(addr);
    unsigned young = 0;
    unsigned garbage = 0;
    unsigned freeR = 0;
    unsigned ghost = 0;
    unsigned route = 255;
    unsigned marked = 0;
    uintptr_t start = 0;
    uintptr_t alloc = 0;
    uintptr_t end = 0;
    uint64_t live = 0;
    if (region != nullptr) {
        young = region->IsYoungRegion() ? 1U : 0U;
        garbage = region->IsGarbageRegion() ? 1U : 0U;
        freeR = region->IsFreeRegion() ? 1U : 0U;
        ghost = RegionInfo::InGhostFromRegion(reinterpret_cast<BaseObject*>(addr)) ? 1U : 0U;
        route = static_cast<unsigned>(region->GetRouteState());
        start = region->GetRegionStart();
        alloc = region->GetRegionAllocPtr();
        end = region->GetRegionEnd();
        live = region->GetLiveByteCount();
        marked = region->IsMarkedObject(reinterpret_cast<BaseObject*>(addr)) ? 1U : 0U;
    }
    char clearBuf[192];
    clearBuf[0] = '\0';
    (void)TraceClear::Lookup(static_cast<MAddress>(addr), clearBuf, sizeof(clearBuf));
    char line[512];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][heldfree] %s addr=%#zx region=%p start=%#zx alloc=%#zx end=%#zx young=%u "
                      "garbage=%u free=%u ghost=%u route=%u live=%llu markedNow=%u tableMarked=%u clear=%s\n",
                      tag, addr, region, start, alloc, end, young, garbage, freeR, ghost, route,
                      static_cast<unsigned long long>(live), marked, WasMarked(addr) ? 1U : 0U,
                      clearBuf[0] != '\0' ? clearBuf : "none");
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void JoinTarget(const char* tag, uintptr_t raw)
{
    uintptr_t addr = Peel(raw);
    DumpRegion(tag, addr);
    uint32_t seq = g_seq.load(std::memory_order_relaxed);
    uint32_t n = g_ringNext.load(std::memory_order_relaxed);
    uint32_t scan = n < kRingCap ? n : kRingCap;
    uint32_t hitEnum = 0;
    uint32_t hitFix = 0;
    uint32_t hitPush = 0;
    Rec lastEnum{};
    Rec lastFix{};
    Rec lastPush{};
    for (uint32_t i = 0; i < scan; ++i) {
        const Rec& rec = g_ring[(n - 1 - i) & (kRingCap - 1)];
        if (rec.target != addr && rec.slot != addr) {
            continue;
        }
        if (rec.kind == KIND_ENUM) {
            ++hitEnum;
            if (lastEnum.slot == 0) {
                lastEnum = rec;
            }
        } else if (rec.kind == KIND_FIX) {
            ++hitFix;
            if (lastFix.slot == 0) {
                lastFix = rec;
            }
        } else if (rec.kind == KIND_PUSH || rec.kind == KIND_MARK) {
            ++hitPush;
            if (lastPush.slot == 0) {
                lastPush = rec;
            }
        }
    }
    Rec slotEnum{};
    Rec slotFix{};
    unsigned haveSlot = FindSlot(addr, &slotEnum, &slotFix) ? 1U : 0U;
    const char* verdict = "unknown";
    if (addr < 0x1000) {
        verdict = "small_or_zero";
    } else if (hitEnum > 0 && hitPush == 0 && !WasMarked(addr)) {
        verdict = "jia_enum_nomark";
    } else if (hitEnum == 0 && haveSlot == 0) {
        verdict = "yi_not_enum";
    } else if (hitEnum > 0 && (hitFix == 0 || lastFix.wrote == 0) && WasMarked(addr)) {
        verdict = "yi_prime_enum_nofix";
    } else if (hitEnum > 0 && WasMarked(addr)) {
        verdict = "enum_and_mark";
    }
    char line[384];
    int wn = sprintf_s(line, sizeof(line),
                       "[GCV2][heldfree] join tag=%s raw=%#zx peel=%#zx seq=%u hitEnum=%u hitFix=%u "
                       "hitPush=%u tableMarked=%u haveSlot=%u verdict=%s\n",
                       tag, raw, addr, seq, hitEnum, hitFix, hitPush, WasMarked(addr) ? 1U : 0U, haveSlot, verdict);
    if (wn > 0) {
        WriteLine(line, static_cast<size_t>(wn));
    }
    if (lastEnum.slot != 0) {
        DumpRec("lastEnumByTarget", lastEnum);
    }
    if (lastFix.slot != 0) {
        DumpRec("lastFixByTarget", lastFix);
    }
    if (lastPush.slot != 0) {
        DumpRec("lastPushByTarget", lastPush);
    }
    if (slotEnum.slot != 0) {
        DumpRec("slotAsEnumKey", slotEnum);
    }
    if (slotFix.slot != 0) {
        DumpRec("slotAsFixKey", slotFix);
    }
}

void JoinZeroSlots(uintptr_t smallVal)
{
    uint32_t seq = g_seq.load(std::memory_order_relaxed);
    uint32_t n = g_ringNext.load(std::memory_order_relaxed);
    uint32_t scan = n < kRingCap ? n : kRingCap;
    uint32_t printed = 0;
    uint32_t zeroNow = 0;
    uint32_t clearedTarget = 0;
    uint32_t enumNoMark = 0;
    for (uint32_t i = 0; i < scan && printed < 8; ++i) {
        const Rec& rec = g_ring[(n - 1 - i) & (kRingCap - 1)];
        if (rec.kind != KIND_ENUM || rec.seq + 1 < seq) {
            continue;
        }
        if (rec.target < 0x1000) {
            continue;
        }
        char clearBuf[192];
        clearBuf[0] = '\0';
        bool cleared = TraceClear::Lookup(static_cast<MAddress>(rec.target), clearBuf, sizeof(clearBuf));
        uintptr_t now = 0;
        bool readable = false;
        if (rec.slot != 0 && Heap::IsHeapAddress(reinterpret_cast<void*>(rec.slot))) {
            uintptr_t words[1] = { 0 };
            std::memcpy(words, reinterpret_cast<const void*>(rec.slot), sizeof(words));
            now = Peel(words[0]);
            readable = true;
        }
        bool nowSmall = readable && now < 0x1000;
        if (!cleared && !nowSmall) {
            continue;
        }
        if (nowSmall) {
            ++zeroNow;
        }
        if (cleared) {
            ++clearedTarget;
        }
        bool marked = WasMarked(rec.target);
        const char* verdict = marked ? "yi_prime_cleared_marked" : "jia_cleared_unmarked";
        if (!cleared && nowSmall) {
            verdict = "slot_now_small";
        }
        if (!marked) {
            ++enumNoMark;
        }
        char line[512];
        int wn = sprintf_s(line, sizeof(line),
                           "[GCV2][heldfree] zeroJoin small=%#zx slot=%#zx was=%#zx now=%#zx seq=%u "
                           "site=%s marked=%u cleared=%u verdict=%s clear=%s\n",
                           smallVal, rec.slot, rec.target, now, rec.seq, rec.site != nullptr ? rec.site : "none",
                           marked ? 1U : 0U, cleared ? 1U : 0U, verdict, cleared ? clearBuf : "none");
        if (wn > 0) {
            WriteLine(line, static_cast<size_t>(wn));
        }
        ++printed;
    }
    char sum[256];
    int sn = sprintf_s(sum, sizeof(sum),
                       "[GCV2][heldfree] zeroSummary small=%#zx printed=%u zeroNow=%u clearedTarget=%u "
                       "enumNoMark=%u\n",
                       smallVal, printed, zeroNow, clearedTarget, enumNoMark);
    if (sn > 0) {
        WriteLine(sum, static_cast<size_t>(sn));
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void BeginYoungCycle()
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_seq.fetch_add(1, std::memory_order_relaxed);
}

void NoteEnumSlot(const void* slot, BaseObject* target, const char* site)
{
    if (!GateOn()) {
        return;
    }
    Rec rec;
    rec.slot = reinterpret_cast<uintptr_t>(slot);
    rec.target = AsAddr(target);
    rec.seq = g_seq.load(std::memory_order_relaxed);
    rec.kind = KIND_ENUM;
    rec.site = site;
    StoreEnum(rec.slot, rec);
    PushRing(rec);
    g_enumN.fetch_add(1, std::memory_order_relaxed);
}

void NotePush(BaseObject* object, const char* site)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    Rec rec;
    rec.slot = 0;
    rec.target = AsAddr(object);
    rec.seq = g_seq.load(std::memory_order_relaxed);
    rec.kind = KIND_PUSH;
    rec.site = site;
    PushRing(rec);
    g_pushN.fetch_add(1, std::memory_order_relaxed);
}

void NoteMark(BaseObject* object)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    Rec rec;
    rec.slot = 0;
    rec.target = AsAddr(object);
    rec.seq = g_seq.load(std::memory_order_relaxed);
    rec.kind = KIND_MARK;
    rec.site = "MarkObject";
    PushRing(rec);
    g_markN.fetch_add(1, std::memory_order_relaxed);
}

void NoteFixSlot(const void* slot, BaseObject* target, int wrote, const char* site)
{
    if (!GateOn()) {
        return;
    }
    Rec rec;
    rec.slot = reinterpret_cast<uintptr_t>(slot);
    rec.target = AsAddr(target);
    rec.seq = g_seq.load(std::memory_order_relaxed);
    rec.kind = KIND_FIX;
    rec.wrote = static_cast<uint16_t>(wrote);
    rec.site = site;
    StoreFix(rec.slot, rec);
    PushRing(rec);
    g_fixN.fetch_add(1, std::memory_order_relaxed);
    if (wrote != 0) {
        g_fixWroteN.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteClearRange(uintptr_t start, size_t size)
{
    if (!GateOn() || size == 0 || start == 0) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    uintptr_t end = start + size;
    uint32_t seq = g_seq.load(std::memory_order_relaxed);
    uint32_t n = g_ringNext.load(std::memory_order_relaxed);
    uint32_t scan = n < kRingCap ? n : kRingCap;
    uint32_t printed = 0;
    uint32_t held = 0;
    uint32_t jia = 0;
    uint32_t yi = 0;
    uint32_t markedHeld = 0;
    uint32_t enumHeld = 0;
    uint32_t fixHeld = 0;
    for (uint32_t i = 0; i < scan; ++i) {
        const Rec& rec = g_ring[(n - 1 - i) & (kRingCap - 1)];
        if (rec.target < start || rec.target >= end) {
            continue;
        }
        if (rec.kind != KIND_ENUM && rec.kind != KIND_FIX && rec.kind != KIND_PUSH && rec.kind != KIND_MARK) {
            continue;
        }
        ++held;
        bool marked = WasMarked(rec.target);
        Rec slotEnum{};
        Rec slotFix{};
        bool haveSlot = rec.slot != 0 && FindSlot(rec.slot, &slotEnum, &slotFix);
        if (rec.kind == KIND_ENUM || haveSlot) {
            ++enumHeld;
        }
        if (rec.kind == KIND_FIX || slotFix.slot != 0) {
            ++fixHeld;
        }
        if (marked) {
            ++markedHeld;
        }
        const char* verdict = "yi_held_not_enum";
        if ((rec.kind == KIND_ENUM || haveSlot) && !marked) {
            verdict = "jia_enum_nomark";
            ++jia;
        } else if (rec.kind == KIND_ENUM && marked && rec.kind != KIND_FIX && slotFix.slot == 0) {
            verdict = "yi_prime_enum_nofix";
            ++yi;
        } else if (rec.kind != KIND_ENUM && !haveSlot) {
            verdict = "yi_held_not_enum";
            ++yi;
        } else if (marked) {
            verdict = "enum_and_mark";
        }
        if (printed < 8) {
            char line[512];
            int wn = sprintf_s(line, sizeof(line),
                               "[GCV2][heldfree] clearJoin start=%#zx end=%#zx slot=%#zx target=%#zx kind=%u "
                               "wrote=%u seq=%u site=%s marked=%u verdict=%s\n",
                               start, end, rec.slot, rec.target, rec.kind, rec.wrote, rec.seq,
                               rec.site != nullptr ? rec.site : "none", marked ? 1U : 0U, verdict);
            if (wn > 0) {
                WriteLine(line, static_cast<size_t>(wn));
            }
            ++printed;
        }
    }
    g_clearN.fetch_add(1, std::memory_order_relaxed);
    g_clearHeldN.fetch_add(held, std::memory_order_relaxed);
    g_clearJiaN.fetch_add(jia, std::memory_order_relaxed);
    g_clearYiN.fetch_add(yi, std::memory_order_relaxed);
    if (held > 0 || (g_clearN.load(std::memory_order_relaxed) <= 4)) {
        char sum[384];
        int sn = sprintf_s(sum, sizeof(sum),
                           "[GCV2][heldfree] clearSummary start=%#zx size=%zx seq=%u printed=%u held=%u "
                           "enumHeld=%u fixHeld=%u markedHeld=%u jia=%u yi=%u\n",
                           start, size, seq, printed, held, enumHeld, fixHeld, markedHeld, jia, yi);
        if (sn > 0) {
            WriteLine(sum, static_cast<size_t>(sn));
        }
    }
}

void NoteCrashRegs(uintptr_t rax, uintptr_t rbx, uintptr_t rcx, uintptr_t rdx, uintptr_t rsi, uintptr_t rdi,
                   uintptr_t r12, uintptr_t r14, uintptr_t rbp)
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][heldfree] crash seq=%u enumN=%zu pushN=%zu markN=%zu fixN=%zu fixWrote=%zu "
                      "clearN=%zu clearHeld=%zu clearJia=%zu clearYi=%zu "
                      "rax=%#zx rbx=%#zx rcx=%#zx rdx=%#zx rsi=%#zx rdi=%#zx r12=%#zx r14=%#zx rbp=%#zx\n",
                      g_seq.load(std::memory_order_relaxed), g_enumN.load(std::memory_order_relaxed),
                      g_pushN.load(std::memory_order_relaxed), g_markN.load(std::memory_order_relaxed),
                      g_fixN.load(std::memory_order_relaxed), g_fixWroteN.load(std::memory_order_relaxed),
                      g_clearN.load(std::memory_order_relaxed), g_clearHeldN.load(std::memory_order_relaxed),
                      g_clearJiaN.load(std::memory_order_relaxed), g_clearYiN.load(std::memory_order_relaxed), rax, rbx,
                      rcx, rdx, rsi, rdi, r12, r14, rbp);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
    JoinTarget("rdi", rdi);
    JoinTarget("rax", rax);
    JoinTarget("rcx", rcx);
    JoinTarget("r12", r12);
    JoinTarget("r14", r14);
    JoinTarget("rbx", rbx);
    if (Peel(rdi) < 0x1000 || Peel(rax) < 0x1000 || Peel(rcx) < 0x1000 || Peel(r12) < 0x1000 || Peel(rbx) < 0x1000) {
        JoinZeroSlots(Peel(rdi) < 0x1000 ? Peel(rdi) : (Peel(rax) < 0x1000 ? Peel(rax) : Peel(rcx)));
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][heldfree] report point=%s seq=%u enumN=%zu pushN=%zu markN=%zu fixN=%zu "
        "fixWrote=%zu clearN=%zu clearHeld=%zu clearJia=%zu clearYi=%zu env=MRT_GCV2_HELDFREE=1",
        point != nullptr ? point : "none", g_seq.load(std::memory_order_relaxed),
        g_enumN.load(std::memory_order_relaxed), g_pushN.load(std::memory_order_relaxed),
        g_markN.load(std::memory_order_relaxed), g_fixN.load(std::memory_order_relaxed),
        g_fixWroteN.load(std::memory_order_relaxed), g_clearN.load(std::memory_order_relaxed),
        g_clearHeldN.load(std::memory_order_relaxed), g_clearJiaN.load(std::memory_order_relaxed),
        g_clearYiN.load(std::memory_order_relaxed));
}

} // namespace HeldFreeDiag
} // namespace MapleRuntime
