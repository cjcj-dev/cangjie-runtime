// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/StartWhoDiag.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Base/Log.h"
#include "Heap/Verify/DiagGate.h"
#include "securec.h"

namespace MapleRuntime {
namespace StartWhoDiag {
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
        return EnvIsOne("MRT_GCV2_STARTWHO") || DiagGate::TokenOn("startwho");
    }();
    return on;
}

std::atomic<size_t> g_markObjectEnter{ 0 };
std::atomic<bool> g_healthOnce{ false };
std::atomic<bool> g_atexitOnce{ false };
thread_local const char* g_currentCaller = nullptr;
thread_local uintptr_t g_currentObject = 0;

void HealthOnce()
{
    bool expected = false;
    if (g_healthOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        LOG(RTLOG_ERROR,
            "[GCV2][startwho] health probe_live=1 caller=WCollector::MarkObject env=MRT_GCV2_STARTWHO=1");
    }
}

void EnsureAtexit()
{
    bool expected = false;
    if (g_atexitOnce.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::atexit([]() { Report("atexit"); });
    }
}

} // namespace

bool Enabled()
{
    return GateOn();
}

ScopedCaller::ScopedCaller(const char* caller, BaseObject* object)
    : active_(GateOn()), previousCaller_(nullptr), previousObject_(0)
{
    if (!active_) {
        return;
    }
    previousCaller_ = g_currentCaller;
    previousObject_ = g_currentObject;
    g_currentCaller = caller;
    g_currentObject = reinterpret_cast<uintptr_t>(object);
    g_markObjectEnter.fetch_add(1, std::memory_order_relaxed);
    EnsureAtexit();
    HealthOnce();
}

ScopedCaller::~ScopedCaller()
{
    if (!active_) {
        return;
    }
    g_currentCaller = previousCaller_;
    g_currentObject = previousObject_;
}

void NoteCrash()
{
    if (!GateOn()) {
        return;
    }
    char line[384];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][startwho] crash caller=%s object=%#zx active=%u markObjectEnter=%zu "
                      "health=%u env=MRT_GCV2_STARTWHO=1\n",
                      g_currentCaller != nullptr ? g_currentCaller : "none", g_currentObject,
                      g_currentCaller != nullptr ? 1U : 0U,
                      g_markObjectEnter.load(std::memory_order_relaxed),
                      g_healthOnce.load(std::memory_order_relaxed) ? 1U : 0U);
    if (n > 0) {
        (void)write(STDERR_FILENO, line, static_cast<size_t>(n));
    }
}

void Report(const char* point)
{
    if (!GateOn()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][startwho] report point=%s markObjectEnter=%zu health=%u env=MRT_GCV2_STARTWHO=1",
        point != nullptr ? point : "none", g_markObjectEnter.load(std::memory_order_relaxed),
        g_healthOnce.load(std::memory_order_relaxed) ? 1U : 0U);
}

} // namespace StartWhoDiag
} // namespace MapleRuntime
