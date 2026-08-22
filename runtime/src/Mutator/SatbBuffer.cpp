// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "SatbBuffer.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Heap.h"
#include "Heap/Verify/AFamilyDiag.h"
#include "Heap/Verify/MarkCompleteVerify.h"

#include "Base/ImmortalWrapper.h"

#include <atomic>

namespace MapleRuntime {
static ImmortalWrapper<SatbBuffer> g_instance;

SatbBuffer& SatbBuffer::Instance() noexcept { return *g_instance; }

bool SatbBuffer::ShouldEnqueue(const BaseObject* obj)
{
    if (UNLIKELY(obj == nullptr)) {
        return false;
    }
    // Young concurrent mark paints the Young face (ClearLiveInfo<Young> at
    // PrepareYoungGarbageCandidates). SATB used the Old face unconditionally, so a
    // stale major mark on a still-young object skipped enqueue — the current-face
    // target then showed up as Stw2CurrentAudit uncovered (REPORT-youngconcstw2).
    // ZGC heap_store_slow_path marks the *new* address (zBarrier.cpp:253-261 /
    // zBarrier.inline.hpp:735-739 mark_and_remember). Using the Young face during
    // GC_REASON_YOUNG is the SATB equivalent of that keep-alive.
    // gc_unit fixtures never Heap::Init — CollectorProxy::currentCollector is null.
    // IsGcStarted lives on CollectorResources (always constructed). During a live
    // young TRACE window it is true; otherwise keep the Old-face legacy.
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    if (resources.IsGcStarted() && resources.GetGCStats().reason == GC_REASON_YOUNG) {
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
        if (region != nullptr && region->IsYoungRegion()) {
            return RegionSpace::ShouldEnqueue<Generation::Young>(obj);
        }
    }
    return RegionSpace::ShouldEnqueue<Generation::Old>(obj);
}

// Why an entry was dropped, re-derived off the hot path. ShouldEnqueue answers
// yes/no, and the difference between its reasons is the difference between two
// unrelated defects: "target sits in a trace region" discards a record for a whole
// class of pages, while the EnqueueObject dedupe discards a record because the same
// object was already enqueued once this cycle -- which is wrong if that earlier
// record was consumed without the object ending up marked.
// Gated with MarkCompleteVerify, which is what reports the dead edges these drops
// are suspected of producing; costs nothing when that gate is off.
namespace {
std::atomic<uint64_t> g_filterDropNonHeap{ 0 };
std::atomic<uint64_t> g_filterDropDeadRegion{ 0 };
std::atomic<uint64_t> g_filterDropTraceRegion{ 0 };
std::atomic<uint64_t> g_filterDropAlreadyMarked{ 0 };
std::atomic<uint64_t> g_filterDropDedupe{ 0 };
} // namespace

void SatbBuffer::NoteFilterDrop(BaseObject* obj)
{
    if (!MarkCompleteVerify::Enabled()) {
        return;
    }
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        g_filterDropNonHeap.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RegionInfo* region = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (region == nullptr || region->IsFreeRegion() || region->IsGarbageRegion()) {
        g_filterDropDeadRegion.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (region->IsTraceRegion()) {
        g_filterDropTraceRegion.fetch_add(1, std::memory_order_relaxed);
        AFamilyDiag::NoteClaim(obj, AFamilyDiag::CH_TRACE_REGION);
        return;
    }
    if (RegionSpace::IsMarkedObject<Generation::Old>(obj)) {
        g_filterDropAlreadyMarked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Not dead, not a trace region, not marked -- so ShouldEnqueue said no because
    // EnqueueObject reported this object already enqueued this cycle. That is the
    // one drop class that can lose a record the mark closure still needed.
    g_filterDropDedupe.fetch_add(1, std::memory_order_relaxed);
}

void SatbBuffer::ReportFilterDrops(const char* point)
{
    if (!MarkCompleteVerify::Enabled()) {
        return;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][satbdrop] point=%s nonHeap=%llu deadRegion=%llu traceRegion=%llu alreadyMarked=%llu dedupe=%llu",
        point == nullptr ? "?" : point,
        static_cast<unsigned long long>(g_filterDropNonHeap.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_filterDropDeadRegion.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_filterDropTraceRegion.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_filterDropAlreadyMarked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_filterDropDedupe.load(std::memory_order_relaxed)));
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
        } else {
            NoteFilterDrop(objectToMark);
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
