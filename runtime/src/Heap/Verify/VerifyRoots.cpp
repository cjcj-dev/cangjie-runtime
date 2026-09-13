// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/z/zVerify.hpp"
#include "Common/ColourPredicates.h"
#include "Heap/Collector/TracingCollector.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
namespace MapleRuntime {
namespace {
// zVerify.cpp:206-256. Do not normalize raw roots before verifying them.
void ColoredRoot(NativeSlot& root, bool afterOldMark)
{
    DCHECK(!Heap::IsHeapAddress(&root));
    const zpointer value = root.GetFieldValue(std::memory_order_acquire);
    if (!ColourPredicates::has_address(raw(value))) { return; }
    CHECK_DETAIL(ClassifySlotWord(raw(value)) != SlotWordVerdict::kIllegal, "Bad colored root at %p", &root);
    if (afterOldMark) {
        CHECK_DETAIL(ColourPredicates::is_marked_old(raw(value), ::g_cjMarkBadMask),
                     "Unmarked old root at %p", &root);
    }
    ZVerify::Object(Heap::GetBarrier().ReadStaticRef(root), &root);
}
void PlainRoot(ObjectRef& root)
{
    DCHECK(!Heap::IsHeapAddress(&root));
    const uintptr_t value = raw(root.LoadPlain(std::memory_order_acquire));
    if (value == 0) { return; }
    // Object checks the uncolored address before it is dereferenced.
    ZVerify::Object(reinterpret_cast<BaseObject*>(value), &root);
}
}
void ZVerify::RootsStrong(bool afterOldMark)
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    auto& collector = static_cast<TracingCollector&>(Heap::GetHeap().GetCollector());
    collector.VisitStrongColoredRoots([&](NativeSlot& root) { ColoredRoot(root, afterOldMark); });
    collector.VisitStrongPlainRoots(PlainRoot, [](Mutator& mutator) {
        mutator.VisitProcessedRoots([&](ObjectRef& root) {
            mutator.VisitHeapRootSlots(root, PlainRoot);
        });
    });
}
void ZVerify::RootsWeak()
{
    DCHECK(MutatorManager::Instance().WorldStopped());
    DCHECK(!Heap::GetHeap().GetCollectorResources().IsResurrectionBlocked());
    auto& collector = static_cast<TracingCollector&>(Heap::GetHeap().GetCollector());
    collector.VisitWeakColoredRoots([](NativeSlot& root) { ColoredRoot(root, true); });
}
} // namespace MapleRuntime
