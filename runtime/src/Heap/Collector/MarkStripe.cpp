// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


#include "Heap/z/zMarkStack.hpp"


namespace MapleRuntime {
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


} // namespace MapleRuntime
