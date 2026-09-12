// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/MarkStripe.h"
#include "Heap/Collector/MarkEngine.h"

#include <limits>

#include <algorithm>
#include <new>
#include <type_traits>

#include "Base/Log.h"
#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace {
constexpr size_t FIRST_STACK_CAPACITY = 128;
constexpr size_t REGULAR_STACK_CAPACITY = 512;
constexpr size_t MARK_STRIPE_SHIFT = 20;
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<MarkStripeStack::StorageObserver> storageObserver{nullptr};
#endif

bool IsPowerOfTwo(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

size_t Log2Exact(size_t value)
{
    CHECK_DETAIL(IsPowerOfTwo(value), "mark stripe count must be a power of two: %zu", value);
    size_t result = 0;
    while ((static_cast<size_t>(1) << result) != value) {
        ++result;
    }
    return result;
}
} // namespace

MarkStripeStack* MarkStripeStack::Create(bool firstStack)
{
    // ZGC zMarkStack.cpp:33-47: one allocation owns the header and entries.
    // Only these bounded capacities reach ZAttachedArray's size arithmetic.
    static_assert(std::is_trivially_destructible<MarkStackEntry>::value,
                  "attached entries must not require per-element destruction");
    const size_t capacity = firstStack ? FIRST_STACK_CAPACITY : REGULAR_STACK_CAPACITY;
    void* const memory = AttachedArray::alloc(capacity);
    if (memory == nullptr) {
        return nullptr;
    }
    auto* const stack = ::new (memory) MarkStripeStack(capacity);
#if defined(MRT_TESTABLE_INTERNALS)
    if (const auto observer = storageObserver.load(std::memory_order_acquire)) {
        observer(stack, stack->entries(stack), capacity, true);
    }
#endif
    return stack;
}

void MarkStripeStack::Destroy(MarkStripeStack* stack)
{
    // Local slots can be null, unlike the non-null-only ZGC destroy caller.
    if (stack == nullptr) {
        return;
    }
#if defined(MRT_TESTABLE_INTERNALS)
    if (const auto observer = storageObserver.load(std::memory_order_acquire)) {
        observer(stack, stack->entries(stack), stack->Capacity(), false);
    }
#endif
    stack->~MarkStripeStack();
    AttachedArray::free(stack);
}

MarkStripeStack::MarkStripeStack(size_t capacity)
    : entries(capacity)
{}

#if defined(MRT_TESTABLE_INTERNALS)
void MarkStripeStack::SetStorageObserver(StorageObserver observer)
{
    storageObserver.store(observer, std::memory_order_release);
}
#endif

void MarkStripeStack::Push(const MarkStackEntry& entry)
{
    CHECK_DETAIL(!IsFull(), "cannot push to a full mark stripe stack");
    entries(this)[top++] = entry;
}

MarkStackEntry MarkStripeStack::Pop()
{
    CHECK_DETAIL(!IsEmpty(), "cannot pop from an empty mark stripe stack");
    return entries(this)[--top];
}

MarkingSMR::MarkingSMR(size_t workerCount)
    : workerCount(workerCount), workers(new (std::nothrow) WorkerState[workerCount])
{
    CHECK_DETAIL(workerCount != 0, "marking SMR needs at least one worker");
    CHECK_DETAIL(workers != nullptr, "failed to allocate marking SMR states workers=%zu", workerCount);
}

MarkingSMR::~MarkingSMR()
{
    Free();
}

std::atomic<MarkStripeStackListNode*>& MarkingSMR::Hazard(size_t workerId)
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR worker id=%zu count=%zu", workerId, workerCount);
    return workers[workerId].hazard;
}

void MarkingSMR::Retire(size_t workerId, MarkStripeStackListNode* node)
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR retire worker id=%zu count=%zu", workerId, workerCount);
    WorkerState& local = workers[workerId];
    local.freeing.push_back(node);
    if (local.freeing.size() >= workerCount * 8) {
        Reclaim(workerId);
    }
}

void MarkingSMR::Reclaim(size_t workerId)
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR reclaim worker id=%zu count=%zu", workerId, workerCount);
    WorkerState& local = workers[workerId];
    for (size_t i = 0; i < workerCount; ++i) {
        MarkStripeStackListNode* const hazard = workers[i].hazard.load(std::memory_order_acquire);
        if (hazard != nullptr) {
            local.scannedHazards.push_back(hazard);
        }
    }

    size_t kept = 0;
    for (MarkStripeStackListNode* node : local.freeing) {
        if (std::find(local.scannedHazards.begin(), local.scannedHazards.end(), node) !=
            local.scannedHazards.end()) {
            local.freeing[kept++] = node;
        } else {
            delete node;
        }
    }
    local.scannedHazards.clear();
    local.freeing.resize(kept);
}

void MarkingSMR::Free()
{
    if (workers == nullptr) {
        return;
    }
    // Called only after all mark workers have joined. At that point no hazard
    // can be legitimately held and all delayed nodes can be freed directly.
    for (size_t i = 0; i < workerCount; ++i) {
        CHECK_DETAIL(workers[i].hazard.load(std::memory_order_relaxed) == nullptr,
                     "marking SMR worker %zu still holds a hazard during Free", i);
        for (MarkStripeStackListNode* node : workers[i].freeing) {
            delete node;
        }
        workers[i].freeing.clear();
        workers[i].scannedHazards.clear();
    }
}

size_t MarkingSMR::PendingCount(size_t workerId) const
{
    CHECK_DETAIL(workerId < workerCount, "invalid SMR pending worker id=%zu count=%zu", workerId, workerCount);
    return workers[workerId].freeing.size();
}

size_t MarkStripeStackList::Length() const
{
    const ptrdiff_t value = length.load(std::memory_order_relaxed);
    return value < 0 ? 0 : static_cast<size_t>(value);
}

void MarkStripeStackList::Push(MarkStripeStack* stack)
{
    CHECK_DETAIL(stack != nullptr && !stack->IsEmpty(), "never publish an empty mark stripe stack");
    auto* const node = new (std::nothrow) MarkStripeStackListNode(stack);
    CHECK_DETAIL(node != nullptr, "failed to allocate mark stripe list node");

    MarkStripeStackListNode* observed = head.load(std::memory_order_relaxed);
    for (;;) {
        node->SetNext(observed);
        // ABA on push is benign: the observed node is never dereferenced.
        if (head.compare_exchange_weak(observed, node, std::memory_order_release, std::memory_order_relaxed)) {
            length.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

MarkStripeStack* MarkStripeStackList::Pop(MarkingSMR& smr, size_t workerId)
{
    std::atomic<MarkStripeStackListNode*>& hazard = smr.Hazard(workerId);
    MarkStripeStackListNode* observed = head.load(std::memory_order_relaxed);
    for (;;) {
        if (observed == nullptr) {
            hazard.store(nullptr, std::memory_order_release);
            return nullptr;
        }

        // Publish before dereferencing observed->Next(). The full fence and
        // acquire reload are the ZGC zMarkStack.cpp:98-121 handshake: either a
        // reclaimer sees this hazard or we see that the head changed.
        hazard.store(observed, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        MarkStripeStackListNode* const afterPublish = head.load(std::memory_order_acquire);
        if (afterPublish != observed) {
            observed = afterPublish;
            continue;
        }

        MarkStripeStackListNode* const next = observed->Next();
        if (head.compare_exchange_strong(observed, next, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            hazard.store(nullptr, std::memory_order_release);
            length.fetch_sub(1, std::memory_order_relaxed);
            MarkStripeStack* const stack = observed->Stack();
            smr.Retire(workerId, observed);
            return stack;
        }
    }
}

void MarkStripe::PublishStack(MarkStripeStack* stack, bool publish, MarkTerminate* terminate)
{
    if (publish) {
        published.Push(stack);
    } else {
        overflowed.Push(stack);
    }
    if (terminate != nullptr) {
        terminate->Wake();
    }
}

MarkStripeStack* MarkStripe::StealStack(MarkingSMR& smr, size_t workerId)
{
    MarkStripeStack* const overflow = overflowed.Pop(smr, workerId);
    return overflow != nullptr ? overflow : published.Pop(smr, workerId);
}

MarkStripeSet::MarkStripeSet(size_t stripeCount)
    : capacityMask(stripeCount - 1), nstripesMask(stripeCount - 1)
{
    CHECK_DETAIL(IsPowerOfTwo(stripeCount), "mark stripe count must be a power of two: %zu", stripeCount);
    stripes.reserve(stripeCount);
    for (size_t i = 0; i < stripeCount; ++i) {
        stripes.emplace_back(new (std::nothrow) MarkStripe());
        CHECK_DETAIL(stripes.back() != nullptr, "failed to allocate mark stripe index=%zu", i);
    }
}

void MarkStripeSet::SetNStripes(size_t value)
{
    CHECK_DETAIL(IsPowerOfTwo(value) && value <= stripes.size(),
                 "nstripes=%zu must be power of two within capacity=%zu", value, stripes.size());
    nstripesMask.store(value - 1, std::memory_order_relaxed);
}

bool MarkStripeSet::TrySetNStripes(size_t oldNStripes, size_t newNStripes)
{
    CHECK_DETAIL(IsPowerOfTwo(newNStripes) && newNStripes >= 1 && newNStripes <= stripes.size(),
                 "nstripes=%zu must be power of two within capacity=%zu", newNStripes, stripes.size());
    size_t expected = oldNStripes - 1;
    return nstripesMask.compare_exchange_strong(expected, newNStripes - 1, std::memory_order_relaxed);
}

size_t MarkStripeSet::CalculateNStripes(size_t nworkers) const
{
    constexpr size_t multiplier = 4;
    size_t target = std::max(nworkers * multiplier, multiplier);
    size_t count = 1;
    while (count < target && count < stripes.size()) {
        count <<= 1;
    }
    return count;
}

bool MarkStripeSet::IsCrowded() const
{
    size_t population = 0;
    const size_t crowdedThreshold = NStripes() << 4;
    for (const auto& stripe : stripes) {
        population += stripe->Population();
        if (population > crowdedThreshold) {
            return true;
        }
    }
    return false;
}

bool MarkStripeSet::IsEmpty() const
{
    for (const auto& stripe : stripes) {
        if (!stripe->IsEmpty()) {
            return false;
        }
    }
    return true;
}

size_t MarkStripeSet::Population() const
{
    size_t population = 0;
    for (const auto& stripe : stripes) {
        population += stripe->Population();
    }
    return population;
}

size_t MarkStripeSet::FirstNonEmptyStripe() const
{
    for (size_t i = 0; i < stripes.size(); ++i) {
        if (!stripes[i]->IsEmpty()) {
            return i;
        }
    }
    return std::numeric_limits<size_t>::max();
}

size_t MarkStripeSet::StripeForAddress(uintptr_t address) const
{
    return (address >> MARK_STRIPE_SHIFT) & NStripesMask();
}

size_t MarkStripeSet::StripeForWorker(size_t nworkers, size_t workerId) const
{
    CHECK_DETAIL(nworkers != 0 && workerId < nworkers, "invalid mark worker id=%zu count=%zu", workerId,
                 nworkers);
    const size_t mask = NStripesMask();
    const size_t active = mask + 1;
    const size_t spilloverLimit = (nworkers / active) * active;
    if (workerId < spilloverLimit) {
        return workerId & mask;
    }
    const size_t spilloverWorkers = nworkers - spilloverLimit;
    const size_t spilloverId = workerId - spilloverLimit;
    return static_cast<size_t>(static_cast<double>(spilloverId) *
                               (static_cast<double>(active) / static_cast<double>(spilloverWorkers)));
}

MarkThreadLocalStacks::MarkThreadLocalStacks(size_t stripeCount) : stacks(stripeCount, nullptr) {}

MarkThreadLocalStacks::~MarkThreadLocalStacks()
{
    for (MarkStripeStack* stack : stacks) {
        MarkStripeStack::Destroy(stack);
    }
}

bool MarkThreadLocalStacks::IsEmpty() const
{
    for (MarkStripeStack* stack : stacks) {
        if (stack != nullptr) {
            return false;
        }
    }
    return true;
}

size_t MarkThreadLocalStacks::Population() const
{
    size_t population = 0;
    for (MarkStripeStack* stack : stacks) {
        if (stack != nullptr) {
            // ZGC treats an installed (even empty) local chunk as non-empty;
            // normally Pop destroys it immediately, so one here is a leaked
            // ownership receipt rather than an object count.
            population += stack->IsEmpty() ? 1 : stack->Size();
        }
    }
    return population;
}

void MarkThreadLocalStacks::Push(MarkStripeSet& stripes, size_t stripeId, const MarkStackEntry& entry,
                                 bool publish)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid local mark stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack*& slot = stacks[stripeId];
    MarkStripeStack* const previous = slot;
    if (previous != nullptr) {
        if (!previous->IsFull()) {
            previous->Push(entry);
            return;
        }
        stripes.At(stripeId).PublishStack(previous, publish, stripes.Terminate());
        slot = nullptr;
    }

    slot = MarkStripeStack::Create(previous == nullptr);
    CHECK_DETAIL(slot != nullptr, "failed to allocate local mark stripe stack");
    slot->Push(entry);
}

bool MarkThreadLocalStacks::Pop(MarkingSMR& smr, size_t workerId, MarkStripeSet& stripes, size_t stripeId,
                                MarkStackEntry& entry)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid pop mark stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack*& slot = stacks[stripeId];
    if (slot == nullptr) {
        slot = stripes.At(stripeId).StealStack(smr, workerId);
        if (slot == nullptr) {
            return false;
        }
    }
    entry = slot->Pop();
    if (slot->IsEmpty()) {
        MarkStripeStack::Destroy(slot);
        slot = nullptr;
    }
    return true;
}

MarkStripeStack* MarkThreadLocalStacks::StealLocal(size_t stripeId)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid steal-local stripe=%zu count=%zu", stripeId, stacks.size());
    MarkStripeStack* const result = stacks[stripeId];
    stacks[stripeId] = nullptr;
    return result;
}

void MarkThreadLocalStacks::Install(size_t stripeId, MarkStripeStack* stack)
{
    CHECK_DETAIL(stripeId < stacks.size(), "invalid install stripe=%zu count=%zu", stripeId, stacks.size());
    CHECK_DETAIL(stacks[stripeId] == nullptr, "install target stripe=%zu is not empty", stripeId);
    stacks[stripeId] = stack;
}

bool MarkThreadLocalStacks::Flush(MarkStripeSet& stripes, bool publish)
{
    bool flushed = false;
    for (size_t i = 0; i < stacks.size(); ++i) {
        MarkStripeStack*& stack = stacks[i];
        if (stack == nullptr) {
            continue;
        }
        stripes.At(i).PublishStack(stack, publish, stripes.Terminate());
        stack = nullptr;
        flushed = true;
    }
    return flushed;
}

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::atomic<MarkClosureObserver> g_markClosureObserver{nullptr};
}
void SetMarkClosureObserverForTest(MarkClosureObserver observer)
{
    g_markClosureObserver.store(observer, std::memory_order_release);
}
void ObserveMarkClosureForTest(const std::vector<BaseObject*>* objects)
{
    auto observer = g_markClosureObserver.load(std::memory_order_acquire);
    if (observer != nullptr) {
        observer(objects);
    }
}
#endif

MarkLiveCache::MarkLiveCache(size_t stripeCount) : shift(MARK_STRIPE_SHIFT + Log2Exact(stripeCount)) {}

MarkLiveCache::~MarkLiveCache()
{
    Flush();
}

void MarkLiveCache::IncLive(RegionInfo* region, size_t bytes)
{
    CHECK_DETAIL(region != nullptr, "cannot cache live bytes for a null region");
    const size_t index = (region->GetRegionStart() >> shift) & (CACHE_SIZE - 1);
    Entry& entry = entries[index];
    if (entry.region != region) {
        Evict(entry);
        entry.region = region;
    }
    entry.bytes += bytes;
    ++entry.objects;
}

void MarkLiveCache::Evict(Entry& entry)
{
    if (entry.region != nullptr) {
        entry.region->AddLiveCounts(entry.objects, entry.bytes);
        entry.region = nullptr;
        entry.bytes = 0;
        entry.objects = 0;
    }
}

void MarkLiveCache::Flush()
{
    for (Entry& entry : entries) {
        Evict(entry);
    }
}

MarkContext::MarkContext(size_t workerCount, size_t workerId, MarkStripeSet& stripes, MarkThreadLocalStacks& stacks)
    : stripeId(stripes.StripeForWorker(workerCount, workerId)), nstripes(stripes.NStripes()), stacks(&stacks),
      cache(stripes.Count())
{}

} // namespace MapleRuntime
