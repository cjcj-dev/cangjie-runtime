#include "Heap/Verify/EnumPushDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Common/BaseObject.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "securec.h"

namespace MapleRuntime {
namespace EnumPushDiag {
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
        return EnvIsOne("MRT_GCV2_ENUMPUSH") || DiagGate::TokenOn("enumpush");
    }();
    return on;
}

size_t EnvSizeT(const char* name, size_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    unsigned long n = std::strtoul(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return static_cast<size_t>(n);
}

uintptr_t Peel(uintptr_t raw) { return raw & 0x0000ffffffffffffULL; }

uintptr_t WatchAddr()
{
    static const uintptr_t watch = []() -> uintptr_t {
        const char* v = std::getenv("MRT_GCV2_ENUMPUSH_WATCH");
        if (v == nullptr || v[0] == '\0') {
            return 0;
        }
        return static_cast<uintptr_t>(std::strtoull(v, nullptr, 0));
    }();
    return watch;
}

bool TypeWanted(const char* name)
{
    static const char* filter = std::getenv("MRT_GCV2_ENUMPUSH_TYPES");
    if (filter == nullptr || filter[0] == '\0' || name == nullptr) {
        return true;
    }
    return std::strstr(name, filter) != nullptr;
}

std::atomic<size_t> g_frameN{ 0 };
std::atomic<size_t> g_mapN{ 0 };
std::atomic<size_t> g_pushN{ 0 };
std::atomic<size_t> g_skipN{ 0 };
std::atomic<size_t> g_watchHit{ 0 };
std::atomic<size_t> g_lines{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };

size_t LineCap()
{
    static const size_t cap = EnvSizeT("MRT_GCV2_ENUMPUSH_MAX", 4096);
    return cap;
}

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
        LOG(RTLOG_ERROR, "[GCV2][enumpush] health probe_live=1 env=MRT_GCV2_ENUMPUSH=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

bool AllowLine()
{
    size_t n = g_lines.fetch_add(1, std::memory_order_relaxed);
    return n < LineCap();
}

const char* TypeNameOf(BaseObject* obj)
{
    uintptr_t peeled = Peel(reinterpret_cast<uintptr_t>(obj));
    auto* plain = reinterpret_cast<BaseObject*>(peeled);
    if (plain == nullptr || !Heap::IsHeapAddress(plain)) {
        return nullptr;
    }
    if (!Collector::PlausibleManagedObjectGate("enumpush.type", plain)) {
        return nullptr;
    }
    TypeInfo* tip = plain->GetTypeInfo();
    if (tip == nullptr) {
        return nullptr;
    }
    return tip->GetName();
}

bool MatchesWatch(BaseObject* obj)
{
    uintptr_t watch = WatchAddr();
    if (watch == 0 || obj == nullptr) {
        return false;
    }
    return Peel(reinterpret_cast<uintptr_t>(obj)) == Peel(watch);
}

} // namespace

bool Enabled() { return GateOn(); }

void NoteFrame(uintptr_t startIP, uintptr_t frameIP, uintptr_t fa, int managed, int valid, int nSlots, int nRegs,
               const char* funcName)
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_frameN.fetch_add(1, std::memory_order_relaxed);
    if (!AllowLine()) {
        return;
    }
    char line[512];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][enumpush] frame start=%#zx pc=%#zx fa=%#zx managed=%d valid=%d "
                      "nSlots=%d nRegs=%d func=%s\n",
                      startIP, frameIP, fa, managed, valid, nSlots, nRegs,
                      (funcName != nullptr && funcName[0] != '\0') ? funcName : "?");
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void NoteMapSlot(uintptr_t fa, intptr_t bias, BaseObject* obj)
{
    if (!GateOn()) {
        return;
    }
    g_mapN.fetch_add(1, std::memory_order_relaxed);
    const char* tname = TypeNameOf(obj);
    if (MatchesWatch(obj)) {
        g_watchHit.fetch_add(1, std::memory_order_relaxed);
    } else if (WatchAddr() != 0 && (tname == nullptr || !TypeWanted(tname))) {
        return;
    } else if (WatchAddr() == 0 && tname != nullptr && !TypeWanted(tname)) {
        return;
    }
    if (!AllowLine()) {
        return;
    }
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][enumpush] map slot fa=%#zx bias=%zd obj=%#zx type=%s watch=%d\n", fa, bias,
                      Peel(reinterpret_cast<uintptr_t>(obj)), tname != nullptr ? tname : "?",
                      MatchesWatch(obj) ? 1 : 0);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void NoteMapReg(uintptr_t fa, int reg, BaseObject* obj)
{
    if (!GateOn()) {
        return;
    }
    g_mapN.fetch_add(1, std::memory_order_relaxed);
    const char* tname = TypeNameOf(obj);
    if (MatchesWatch(obj)) {
        g_watchHit.fetch_add(1, std::memory_order_relaxed);
    } else if (WatchAddr() != 0 && (tname == nullptr || !TypeWanted(tname))) {
        return;
    } else if (WatchAddr() == 0 && tname != nullptr && !TypeWanted(tname)) {
        return;
    }
    if (!AllowLine()) {
        return;
    }
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][enumpush] map reg fa=%#zx reg=%d obj=%#zx type=%s watch=%d\n", fa, reg,
                      Peel(reinterpret_cast<uintptr_t>(obj)), tname != nullptr ? tname : "?",
                      MatchesWatch(obj) ? 1 : 0);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void NotePush(BaseObject* obj, const char* site, const void* slot)
{
    if (!GateOn()) {
        return;
    }
    HealthOnce();
    EnsureAtexit();
    g_pushN.fetch_add(1, std::memory_order_relaxed);
    const char* tname = TypeNameOf(obj);
    int hit = MatchesWatch(obj) ? 1 : 0;
    if (hit != 0) {
        g_watchHit.fetch_add(1, std::memory_order_relaxed);
    }
    if (WatchAddr() != 0 && hit == 0 && (tname == nullptr || !TypeWanted(tname))) {
        return;
    }
    if (WatchAddr() == 0 && tname != nullptr && !TypeWanted(tname)) {
        return;
    }
    if (!AllowLine()) {
        return;
    }
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][enumpush] PUSH site=%s obj=%#zx slot=%#zx type=%s watch=%d\n",
                      site != nullptr ? site : "?", Peel(reinterpret_cast<uintptr_t>(obj)),
                      reinterpret_cast<uintptr_t>(slot), tname != nullptr ? tname : "?", hit);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void NoteSkip(BaseObject* obj, const char* site, const char* reason)
{
    if (!GateOn()) {
        return;
    }
    g_skipN.fetch_add(1, std::memory_order_relaxed);
    int hit = MatchesWatch(obj) ? 1 : 0;
    if (hit != 0) {
        g_watchHit.fetch_add(1, std::memory_order_relaxed);
    } else if (WatchAddr() != 0) {
        return;
    }
    if (!AllowLine()) {
        return;
    }
    char line[320];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][enumpush] SKIP site=%s obj=%#zx reason=%s watch=%d\n", site != nullptr ? site : "?",
                      Peel(reinterpret_cast<uintptr_t>(obj)), reason != nullptr ? reason : "?", hit);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][enumpush] report point=%s frames=%zu maps=%zu push=%zu skip=%zu watchHit=%zu lines=%zu "
        "env=MRT_GCV2_ENUMPUSH=1",
        point != nullptr ? point : "?", g_frameN.load(std::memory_order_relaxed),
        g_mapN.load(std::memory_order_relaxed), g_pushN.load(std::memory_order_relaxed),
        g_skipN.load(std::memory_order_relaxed), g_watchHit.load(std::memory_order_relaxed),
        g_lines.load(std::memory_order_relaxed));
}

} // namespace EnumPushDiag
} // namespace MapleRuntime
