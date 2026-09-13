// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


// Test observation fragment; included only by its product translation unit.
#pragma once

namespace {
#if defined(MRT_TESTABLE_INTERNALS)
std::atomic<MarkStripeStack::StorageObserver> storageObserver{nullptr};
#endif
}

#if defined(MRT_TESTABLE_INTERNALS)
void MarkStripeStack::SetStorageObserver(StorageObserver observer)
{
    storageObserver.store(observer, std::memory_order_release);
}
#endif

