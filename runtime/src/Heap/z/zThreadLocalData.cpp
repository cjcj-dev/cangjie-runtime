// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zMark.hpp"
#include <map>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <vector>

namespace MapleRuntime {
namespace {
struct DataOwners {
    struct Owner {
        ThreadGCData* data;
        Mutator* mutator;
        ThreadLocalData* native;
        size_t readers = 0;
        Owner(ThreadGCData* d, Mutator* m, ThreadLocalData* n) : data(d), mutator(m), native(n) {}
    };
    std::mutex mutex;
    std::condition_variable released;
    std::map<ThreadGCData*, std::unique_ptr<Owner>> owners;
};
DataOwners& Owners()
{
    static DataOwners owners;
    return owners;
}
std::mutex masksMutex;
ThreadGCData::Masks publishedMasks{};
}

void ThreadGCData::PublishMasks(const Masks& masks)
{
    std::lock_guard<std::mutex> lock(masksMutex);
    publishedMasks = masks;
}

ThreadGCData::Masks ThreadGCData::PublishedMasks()
{
    std::lock_guard<std::mutex> lock(masksMutex);
    return publishedMasks;
}

void ThreadGCData::InstallMasks(const Masks& masks)
{
    loadGoodMask = masks.loadGood;
    loadBadMask = masks.loadBad;
    markBadMask = masks.markBad;
    storeGoodMask = masks.storeGood;
    storeBadMask = masks.storeBad;
}

// A managed owner is the logical CJThread/Mutator, not the OS thread on
// which it happens to be executing. Native executors own a separate data.
void ThreadGCData::Attach(Mutator* owner, ThreadLocalData* nativeOwner, zaddress_unsafe* root)
{
    invisibleRoot = root;
    const Masks masks = PublishedMasks();
    if (masks.storeGood == 0) {
        // Native allocation can bootstrap before pointer colors initialize.
        // This storage is not attached to the GC inventory or managed code yet.
        return;
    }
    auto& registry = Owners();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (registry.owners.find(this) != registry.owners.end()) {
        return; // Rebinding must not overwrite the owner's pending old color.
    }
    InstallMasks(masks);
    storeBarrierBuffer->Initialize(masks.storeGood);
    invisibleRoot = root;
    registry.owners.emplace(this, std::make_unique<DataOwners::Owner>(this, owner, nativeOwner));
}

bool ThreadGCData::FlushMarkStacks(MarkDomain& domain)
{
    const size_t index = domain.Generation() == MarkingStacks::MarkingGeneration::YOUNG ? 0 : 1;
    return markStacks[index].Flush(domain.Stripes(), true);
}

void ThreadGCData::Detach()
{
    auto& registry = Owners();
    std::unique_lock<std::mutex> lock(registry.mutex);
    auto it = registry.owners.find(this);
    if (it == registry.owners.end()) {
        return;
    }
    std::unique_ptr<DataOwners::Owner> owner = std::move(it->second);
    registry.owners.erase(it);
    registry.released.wait(lock, [&] { return owner->readers == 0; });
}

ThreadGCData::~ThreadGCData()
{
    Detach();
    delete storeBarrierBuffer;
}

void ThreadGCData::VisitOwners(
    const std::function<void(ThreadGCData&, Mutator*, ThreadLocalData*)>& visitor)
{
    auto& registry = Owners();
    std::vector<DataOwners::Owner*> snapshot;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        snapshot.reserve(registry.owners.size());
        for (const auto& entry : registry.owners) {
            ++entry.second->readers;
            snapshot.push_back(entry.second.get());
        }
    }
    struct ReleaseSnapshot {
        DataOwners& registry;
        const std::vector<DataOwners::Owner*>& snapshot;
        ~ReleaseSnapshot()
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            for (auto* owner : snapshot) { --owner->readers; }
            registry.released.notify_all();
        }
    } release{registry, snapshot};
    // Never hold the inventory lock across a target mutator lock or marking:
    // those operations can allocate/attach another native producer.
    for (const auto* entry : snapshot) {
        visitor(*entry->data, entry->mutator, entry->native);
    }
}
} // namespace MapleRuntime
