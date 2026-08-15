// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/WhoPushDiag.h"

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
namespace WhoPushDiag {
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
        return EnvIsOne("MRT_GCV2_WHOPUSH") || DiagGate::TokenOn("whopush");
    }();
    return on;
}

std::atomic<size_t> g_pushTotal{ 0 };
std::atomic<size_t> g_pushStart{ 0 };
std::atomic<size_t> g_pushInterior{ 0 };
std::atomic<size_t> g_pushUnrecoverable{ 0 };
std::atomic<bool> g_firstInteriorLogged{ false };
std::atomic<bool> g_atexitOnce{ false };

struct PushRec {
    uintptr_t object = 0;
    uintptr_t host = 0;
    uintptr_t slot = 0;
    uintptr_t holder = 0;
    uintptr_t ra0 = 0;
    size_t hostOff = 0;
    const char* site = nullptr;
    unsigned interior = 0;
};

PushRec g_firstInteriorRec;
thread_local PushRec g_lastPush{};

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

void FormatRec(char* line, size_t cap, const char* kind, const PushRec& rec, uintptr_t rdi)
{
    (void)sprintf_s(line, cap,
                    "[GCV2][whopush] %s obj=%#zx host=%#zx hostOff=%zu site=%s slot=%#zx holder=%#zx "
                    "interior=%u ra0=%p rdi=%#zx pushTotal=%zu pushStart=%zu pushInterior=%zu "
                    "pushUnrecoverable=%zu env=MRT_GCV2_WHOPUSH=1\n",
                    kind, rec.object, rec.host, rec.hostOff, rec.site != nullptr ? rec.site : "none", rec.slot,
                    rec.holder, rec.interior, reinterpret_cast<void*>(rec.ra0), rdi,
                    g_pushTotal.load(std::memory_order_relaxed), g_pushStart.load(std::memory_order_relaxed),
                    g_pushInterior.load(std::memory_order_relaxed),
                    g_pushUnrecoverable.load(std::memory_order_relaxed));
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NotePush(BaseObject* object, const char* site, const void* slot, BaseObject* holder)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    EnsureAtexit();
    g_pushTotal.fetch_add(1, std::memory_order_relaxed);
    PushRec rec;
    rec.object = reinterpret_cast<uintptr_t>(object);
    rec.slot = reinterpret_cast<uintptr_t>(slot);
    rec.holder = reinterpret_cast<uintptr_t>(holder);
    rec.site = site;
    rec.ra0 = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    BaseObject* host = nullptr;
    if (Heap::IsHeapAddress(object)) {
        host = Collector::TryRecoverInteriorBase(object);
    }
    if (host != nullptr && host != object) {
        rec.interior = 1;
        rec.host = reinterpret_cast<uintptr_t>(host);
        uintptr_t o = rec.object;
        uintptr_t h = rec.host;
        rec.hostOff = (o > h) ? (o - h) : 0;
        g_pushInterior.fetch_add(1, std::memory_order_relaxed);
        bool expected = false;
        if (g_firstInteriorLogged.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            g_firstInteriorRec = rec;
            char line[768];
            FormatRec(line, sizeof(line), "first_interior", rec, 0);
            WriteLine(line, std::strlen(line));
        }
    } else if (object != nullptr && !object->IsValidObject()) {
        rec.host = 0;
        g_pushUnrecoverable.fetch_add(1, std::memory_order_relaxed);
    } else {
        rec.host = rec.object;
        g_pushStart.fetch_add(1, std::memory_order_relaxed);
    }
    g_lastPush = rec;
}

void NoteCrashRdi(uintptr_t rdi)
{
    if (!GateOn()) {
        return;
    }
    char line[768];
    if (g_firstInteriorLogged.load(std::memory_order_relaxed)) {
        FormatRec(line, sizeof(line), "crash_first_interior", g_firstInteriorRec, rdi);
        WriteLine(line, std::strlen(line));
    }
    const PushRec& last = g_lastPush;
    unsigned match = (last.object != 0 && last.object == rdi) ? 1 : 0;
    unsigned near = 0;
    size_t delta = 0;
    if (last.object != 0 && rdi >= last.object && (rdi - last.object) <= 64) {
        near = 1;
        delta = rdi - last.object;
    } else if (last.host != 0 && rdi >= last.host && (rdi - last.host) <= 64) {
        near = 1;
        delta = rdi - last.host;
    }
    (void)sprintf_s(line, sizeof(line),
                    "[GCV2][whopush] crash rdi=%#zx lastObj=%#zx lastHost=%#zx lastOff=%zu lastSite=%s "
                    "match=%u near=%u nearDelta=%zu lastInterior=%u env=MRT_GCV2_WHOPUSH=1\n",
                    rdi, last.object, last.host, last.hostOff, last.site != nullptr ? last.site : "none", match, near,
                    delta, last.interior);
    WriteLine(line, std::strlen(line));
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][whopush] report point=%s pushTotal=%zu pushStart=%zu pushInterior=%zu "
        "pushUnrecoverable=%zu firstInterior=%u env=MRT_GCV2_WHOPUSH=1",
        point != nullptr ? point : "none", g_pushTotal.load(std::memory_order_relaxed),
        g_pushStart.load(std::memory_order_relaxed), g_pushInterior.load(std::memory_order_relaxed),
        g_pushUnrecoverable.load(std::memory_order_relaxed),
        g_firstInteriorLogged.load(std::memory_order_relaxed) ? 1U : 0U);
}

} // namespace WhoPushDiag
} // namespace MapleRuntime
