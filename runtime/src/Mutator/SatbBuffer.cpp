// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SatbBuffer.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Allocator/RegionSpace.h"

#include "Base/ImmortalWrapper.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace MapleRuntime {
namespace {
// Default-off live A/B probe. It replaces real SATB targets with target+8 while preserving the
// target as knownBase, then reports whether that exact host reached the mark bitmap.
constexpr size_t SATB_CARRY_PROBE_HOST_CAPACITY = 4096;
std::atomic<size_t> g_satbCarryProbeInjected{ 0 };
std::atomic<size_t> g_satbCarryIngress{ 0 };
std::atomic<size_t> g_satbCarryInjectedIngress{ 0 };
std::atomic<size_t> g_satbCarryNaturalIngress{ 0 };
std::atomic<size_t> g_satbCarryHostDequeued{ 0 };
std::atomic<size_t> g_satbCarryProbeHostCount{ 0 };
std::atomic<size_t> g_satbCarryProbeHostReported{ 0 };
std::atomic<size_t> g_satbCarryHostMarked{ 0 };
std::atomic<size_t> g_satbCarryLeak{ 0 };
std::array<std::atomic<BaseObject*>, SATB_CARRY_PROBE_HOST_CAPACITY> g_satbCarryInjectedTargets{};
std::array<std::atomic<BaseObject*>, SATB_CARRY_PROBE_HOST_CAPACITY> g_satbCarryProbeHosts{};

bool SatbCarryProbeOn()
{
    static const bool on = []() {
        const char* value = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_SATB_CARRY_PROBE */;
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return on;
}
} // namespace

static ImmortalWrapper<SatbBuffer> g_instance;

SatbBuffer& SatbBuffer::Instance() noexcept { return *g_instance; }

void SatbBuffer::MaybeInjectCarryProbe(BaseObject*& target, BaseObject*& knownBase)
{
    if (!SatbCarryProbeOn() || target == nullptr || knownBase != nullptr ||
        !Collector::PlausibleManagedObjectGate("SatbCarryProbe.host", target) ||
        RegionSpace::GetAllocSize(*target) <= 8u) {
        return;
    }
    size_t index = g_satbCarryProbeInjected.fetch_add(1, std::memory_order_relaxed);
    if (index >= SATB_CARRY_PROBE_HOST_CAPACITY) {
        return;
    }
    knownBase = target;
    target = to_object(to_zaddress(reinterpret_cast<MAddress>(target) + 8u));
    g_satbCarryInjectedTargets[index].store(target, std::memory_order_relaxed);
}

void SatbBuffer::NoteInteriorEnqueued(const BaseObject* target, const BaseObject* knownBase)
{
    if (!SatbCarryProbeOn() || knownBase == nullptr) {
        return;
    }
    g_satbCarryIngress.fetch_add(1, std::memory_order_relaxed);
    bool injected = false;
    size_t injectedCount = std::min(g_satbCarryProbeInjected.load(std::memory_order_relaxed),
                                    SATB_CARRY_PROBE_HOST_CAPACITY);
    for (size_t index = 0; index < injectedCount; ++index) {
        if (target == g_satbCarryInjectedTargets[index].load(std::memory_order_relaxed)) {
            injected = true;
            break;
        }
    }
    if (injected) {
        g_satbCarryInjectedIngress.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_satbCarryNaturalIngress.fetch_add(1, std::memory_order_relaxed);
    }
}

void SatbBuffer::NoteInteriorRetained(const BaseObject* knownBase)
{
    if (!SatbCarryProbeOn() || knownBase == nullptr) {
        return;
    }
    size_t index = g_satbCarryProbeHostCount.fetch_add(1, std::memory_order_relaxed);
    if (index < SATB_CARRY_PROBE_HOST_CAPACITY) {
        g_satbCarryProbeHosts[index].store(const_cast<BaseObject*>(knownBase), std::memory_order_relaxed);
    }
}

void SatbBuffer::NoteHostDequeued(const BaseObject* knownBase)
{
    if (SatbCarryProbeOn() && knownBase != nullptr) {
        g_satbCarryHostDequeued.fetch_add(1, std::memory_order_relaxed);
    }
}

void SatbBuffer::ReportCarryProbe()
{
    if (!SatbCarryProbeOn()) {
        return;
    }
    size_t end = std::min(g_satbCarryProbeHostCount.load(std::memory_order_relaxed),
                          SATB_CARRY_PROBE_HOST_CAPACITY);
    size_t begin = g_satbCarryProbeHostReported.exchange(end, std::memory_order_relaxed);
    size_t marked = 0;
    size_t leaked = 0;
    for (size_t index = begin; index < end; ++index) {
        BaseObject* host = g_satbCarryProbeHosts[index].load(std::memory_order_relaxed);
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(host));
        if (host != nullptr && region != nullptr && !region->IsFreeRegion() && !region->IsGarbageRegion() &&
            RegionSpace::IsMarkedObject<Generation::Old>(host)) {
            ++marked;
        } else {
            ++leaked;
        }
    }
    g_satbCarryHostMarked.fetch_add(marked, std::memory_order_relaxed);
    g_satbCarryLeak.fetch_add(leaked, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[GCV2][satbcarry] interiorIngress=%zu injectedIngress=%zu naturalIngress=%zu "
                 "hostDequeued=%zu hostMarked=%zu leak=%zu\n",
                 g_satbCarryIngress.load(std::memory_order_relaxed),
                 g_satbCarryInjectedIngress.load(std::memory_order_relaxed),
                 g_satbCarryNaturalIngress.load(std::memory_order_relaxed),
                 g_satbCarryHostDequeued.load(std::memory_order_relaxed),
                 g_satbCarryHostMarked.load(std::memory_order_relaxed),
                 g_satbCarryLeak.load(std::memory_order_relaxed));
    std::fflush(stderr);
}

bool SatbBuffer::ShouldEnqueue(const BaseObject* obj)
{
    if (UNLIKELY(obj == nullptr)) {
        return false;
    }
    return RegionSpace::ShouldEnqueue<Generation::Old>(obj);
}

void SatbBuffer::Filter(Node* node)
{
    size_t retainedIndex = Node::CONTAINER_CAPACITY;
    size_t sourceIndex = Node::CONTAINER_CAPACITY;
    while (sourceIndex != node->index) {
        Node::Entry entry = node->entryContainer[--sourceIndex];
        BaseObject* objectToMark = entry.knownBase != nullptr ? entry.knownBase : entry.target;
        if (Heap::IsHeapAddress(objectToMark) && ShouldEnqueue(objectToMark)) {
            node->entryContainer[--retainedIndex] = entry;
            NoteInteriorRetained(entry.knownBase);
        }
    }
    while (node->index != retainedIndex) {
        node->entryContainer[node->index++] = { nullptr, nullptr };
    }
}

void SatbBuffer::FlushQueue(Node*& node)
{
    if (node == nullptr) {
        return;
    }
    Filter(node);
    if (node->IsEmpty()) {
        freeNodes.Push(node);
    } else {
        retiredNodes.Push(node);
    }
    node = nullptr;
}

static ImmortalWrapper<WeakRefBuffer> g_weakRefBuffer;

WeakRefBuffer& WeakRefBuffer::Instance() noexcept { return *g_weakRefBuffer; }
} // namespace MapleRuntime
