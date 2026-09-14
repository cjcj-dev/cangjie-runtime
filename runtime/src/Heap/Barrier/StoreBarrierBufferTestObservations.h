// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


// Test observation fragment; included only by its product translation unit.
#pragma once

namespace {
#if defined(MRT_TESTABLE_INTERNALS)
thread_local StoreBarrierFlushObserver g_flushObserver = nullptr;

void NotifyFlushObserver(StoreBarrierFlushEvent event, const StoreBarrierEntry& entry)
{
    if (g_flushObserver != nullptr) {
        g_flushObserver(event, entry);
    }
}
#endif

}

#if defined(MRT_TESTABLE_INTERNALS)
void StoreBarrierBuffer::SetFlushObserverForTest(StoreBarrierFlushObserver observer)
{
    g_flushObserver = observer;
}

#endif

