// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Heap/z/zThreadLocalData.hpp"
#include "Heap/z/zMark.hpp"
#include "Mutator/ThreadLocal.h"
#include "Mutator/ThreadSMR.h"
#include "Mutator/Mutator.h"
#include <mutex>

namespace MapleRuntime {
namespace {
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

bool ThreadGCData::FlushMarkStacks(ZMark& domain)
{
    const size_t index = domain.Generation() == MarkingStacks::MarkingGeneration::YOUNG ? 0 : 1;
    return markStacks[index].Flush(domain.Stripes());
}

ThreadGCData::~ThreadGCData()
{
    delete storeBarrierBuffer;
}

void ThreadGCData::VisitOwners(
    const std::function<void(ThreadGCData&, Mutator*, ThreadLocalData*)>& visitor)
{
    // HotSpot runtime/threads.cpp:238-262: distinct Java and non-Java lists.
    {
        ThreadsListHandle threads;
        for (size_t i = 0; i < threads.length(); ++i) {
            Mutator* owner = threads.thread_at(i);
            visitor(owner->GetGCData(), owner, nullptr);
        }
    }
    for (CleanThreadLocalData::Iterator it; !it.End(); it.Step()) {
        auto* owner = it.Current();
        visitor(owner->nativeData, nullptr, owner->NativeTLS());
    }
}
} // namespace MapleRuntime
