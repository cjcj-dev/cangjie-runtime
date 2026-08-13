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
#include <mutex>
#include <unordered_map>
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
std::atomic<size_t> g_producedRoot{ 0 };
std::atomic<size_t> g_producedHeap{ 0 };
std::atomic<size_t> g_producedRemset{ 0 };
std::atomic<size_t> g_markRoot{ 0 };
std::atomic<size_t> g_markHeap{ 0 };
std::atomic<size_t> g_markRemset{ 0 };
std::atomic<size_t> g_markMixed{ 0 };
std::atomic<size_t> g_markNone{ 0 };
thread_local const char* g_currentCaller = nullptr;
thread_local uintptr_t g_currentObject = 0;
thread_local uint8_t g_currentSourceMask = 0;
thread_local const char* g_currentProducerSite = nullptr;
thread_local uintptr_t g_currentProducerSlot = 0;
thread_local uintptr_t g_currentProducerHolder = 0;
thread_local uintptr_t g_currentRootBase = 0;
thread_local uintptr_t g_currentRootDerived = 0;
thread_local size_t g_currentRootOffset = 0;

struct ProducerRecord {
    uint8_t mask = 0;
    const char* site = nullptr;
    uintptr_t slot = 0;
    uintptr_t holder = 0;
    uintptr_t rootBase = 0;
    uintptr_t rootDerived = 0;
    size_t rootOffset = 0;
};

std::mutex g_producerLock;
std::unordered_map<uintptr_t, ProducerRecord> g_producers;
std::unordered_map<uintptr_t, ProducerRecord> g_pendingRoots;

const char* SourceName(uint8_t mask)
{
    switch (mask) {
        case 0:
            return "none";
        case static_cast<uint8_t>(Source::ROOT_DERIVED):
            return "root_derived";
        case static_cast<uint8_t>(Source::HEAP_FIELD):
            return "heap_field";
        case static_cast<uint8_t>(Source::REMSET):
            return "remset";
        default:
            return "mixed";
    }
}

void CountProduced(Source source)
{
    switch (source) {
        case Source::ROOT_DERIVED:
            g_producedRoot.fetch_add(1, std::memory_order_relaxed);
            return;
        case Source::HEAP_FIELD:
            g_producedHeap.fetch_add(1, std::memory_order_relaxed);
            return;
        case Source::REMSET:
            g_producedRemset.fetch_add(1, std::memory_order_relaxed);
            return;
    }
}

void MergeProduced(uintptr_t object, const ProducerRecord& incoming, Source source)
{
    std::lock_guard<std::mutex> lock(g_producerLock);
    ProducerRecord& record = g_producers[object];
    record.mask |= static_cast<uint8_t>(source);
    record.site = incoming.site;
    record.slot = incoming.slot;
    record.holder = incoming.holder;
    if (source == Source::ROOT_DERIVED) {
        record.rootBase = incoming.rootBase;
        record.rootDerived = incoming.rootDerived;
        record.rootOffset = incoming.rootOffset;
    }
}

void CaptureProducer(BaseObject* object)
{
    ProducerRecord record;
    {
        std::lock_guard<std::mutex> lock(g_producerLock);
        auto it = g_producers.find(reinterpret_cast<uintptr_t>(object));
        if (it != g_producers.end()) {
            record = it->second;
            g_producers.erase(it);
        }
    }
    g_currentSourceMask = record.mask;
    g_currentProducerSite = record.site;
    g_currentProducerSlot = record.slot;
    g_currentProducerHolder = record.holder;
    g_currentRootBase = record.rootBase;
    g_currentRootDerived = record.rootDerived;
    g_currentRootOffset = record.rootOffset;
    switch (record.mask) {
        case 0:
            g_markNone.fetch_add(1, std::memory_order_relaxed);
            break;
        case static_cast<uint8_t>(Source::ROOT_DERIVED):
            g_markRoot.fetch_add(1, std::memory_order_relaxed);
            break;
        case static_cast<uint8_t>(Source::HEAP_FIELD):
            g_markHeap.fetch_add(1, std::memory_order_relaxed);
            break;
        case static_cast<uint8_t>(Source::REMSET):
            g_markRemset.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            g_markMixed.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

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

void NoteRootCandidate(BaseObject* object, const char* site, const void* slot,
                       BaseObject* base, BaseObject* derived)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    ProducerRecord record;
    record.site = site;
    record.slot = reinterpret_cast<uintptr_t>(slot);
    record.rootBase = reinterpret_cast<uintptr_t>(base);
    record.rootDerived = reinterpret_cast<uintptr_t>(derived);
    if (record.rootBase != 0 && record.rootDerived >= record.rootBase) {
        record.rootOffset = record.rootDerived - record.rootBase;
    }
    std::lock_guard<std::mutex> lock(g_producerLock);
    g_pendingRoots[reinterpret_cast<uintptr_t>(object)] = record;
}

void NoteProducedRootIfPending(BaseObject* object)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    ProducerRecord record;
    {
        std::lock_guard<std::mutex> lock(g_producerLock);
        auto it = g_pendingRoots.find(reinterpret_cast<uintptr_t>(object));
        if (it == g_pendingRoots.end()) {
            return;
        }
        record = it->second;
        g_pendingRoots.erase(it);
    }
    MergeProduced(reinterpret_cast<uintptr_t>(object), record, Source::ROOT_DERIVED);
    CountProduced(Source::ROOT_DERIVED);
}

void DiscardRootCandidate(BaseObject* object)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_producerLock);
    g_pendingRoots.erase(reinterpret_cast<uintptr_t>(object));
}

void NoteProduced(BaseObject* object, Source source, const char* site, const void* slot, BaseObject* holder)
{
    if (!GateOn() || object == nullptr) {
        return;
    }
    ProducerRecord record;
    record.site = site;
    record.slot = reinterpret_cast<uintptr_t>(slot);
    record.holder = reinterpret_cast<uintptr_t>(holder);
    MergeProduced(reinterpret_cast<uintptr_t>(object), record, source);
    CountProduced(source);
}

ScopedCaller::ScopedCaller(const char* caller, BaseObject* object)
    : active_(GateOn()), previousCaller_(nullptr), previousObject_(0), previousSourceMask_(0),
      previousProducerSite_(nullptr), previousProducerSlot_(0), previousProducerHolder_(0), previousRootBase_(0),
      previousRootDerived_(0), previousRootOffset_(0)
{
    if (!active_) {
        return;
    }
    previousCaller_ = g_currentCaller;
    previousObject_ = g_currentObject;
    previousSourceMask_ = g_currentSourceMask;
    previousProducerSite_ = g_currentProducerSite;
    previousProducerSlot_ = g_currentProducerSlot;
    previousProducerHolder_ = g_currentProducerHolder;
    previousRootBase_ = g_currentRootBase;
    previousRootDerived_ = g_currentRootDerived;
    previousRootOffset_ = g_currentRootOffset;
    g_currentCaller = caller;
    g_currentObject = reinterpret_cast<uintptr_t>(object);
    CaptureProducer(object);
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
    g_currentSourceMask = previousSourceMask_;
    g_currentProducerSite = previousProducerSite_;
    g_currentProducerSlot = previousProducerSlot_;
    g_currentProducerHolder = previousProducerHolder_;
    g_currentRootBase = previousRootBase_;
    g_currentRootDerived = previousRootDerived_;
    g_currentRootOffset = previousRootOffset_;
}

void NoteCrash()
{
    if (!GateOn()) {
        return;
    }
    char line[768];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][startwho] crash caller=%s object=%#zx active=%u source=%s sourceMask=%u "
                      "producerSite=%s producerSlot=%#zx holder=%#zx rootBase=%#zx rootDerived=%#zx "
                      "rootOffset=%zu markObjectEnter=%zu producedRoot=%zu producedHeap=%zu producedRemset=%zu "
                      "markRoot=%zu markHeap=%zu markRemset=%zu markMixed=%zu markNone=%zu "
                      "health=%u env=MRT_GCV2_STARTWHO=1\n",
                      g_currentCaller != nullptr ? g_currentCaller : "none", g_currentObject,
                      g_currentCaller != nullptr ? 1U : 0U,
                      SourceName(g_currentSourceMask), static_cast<unsigned>(g_currentSourceMask),
                      g_currentProducerSite != nullptr ? g_currentProducerSite : "none", g_currentProducerSlot,
                      g_currentProducerHolder, g_currentRootBase, g_currentRootDerived, g_currentRootOffset,
                      g_markObjectEnter.load(std::memory_order_relaxed),
                      g_producedRoot.load(std::memory_order_relaxed),
                      g_producedHeap.load(std::memory_order_relaxed),
                      g_producedRemset.load(std::memory_order_relaxed),
                      g_markRoot.load(std::memory_order_relaxed), g_markHeap.load(std::memory_order_relaxed),
                      g_markRemset.load(std::memory_order_relaxed), g_markMixed.load(std::memory_order_relaxed),
                      g_markNone.load(std::memory_order_relaxed),
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
        "[GCV2][startwho] report point=%s markObjectEnter=%zu producedRoot=%zu producedHeap=%zu "
        "producedRemset=%zu markRoot=%zu markHeap=%zu markRemset=%zu markMixed=%zu markNone=%zu "
        "health=%u env=MRT_GCV2_STARTWHO=1",
        point != nullptr ? point : "none", g_markObjectEnter.load(std::memory_order_relaxed),
        g_producedRoot.load(std::memory_order_relaxed), g_producedHeap.load(std::memory_order_relaxed),
        g_producedRemset.load(std::memory_order_relaxed), g_markRoot.load(std::memory_order_relaxed),
        g_markHeap.load(std::memory_order_relaxed), g_markRemset.load(std::memory_order_relaxed),
        g_markMixed.load(std::memory_order_relaxed), g_markNone.load(std::memory_order_relaxed),
        g_healthOnce.load(std::memory_order_relaxed) ? 1U : 0U);
}

} // namespace StartWhoDiag
} // namespace MapleRuntime
