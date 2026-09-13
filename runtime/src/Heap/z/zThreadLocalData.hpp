// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <memory>
#include "Heap/z/zStoreBarrierBuffer.hpp"
#include "Heap/z/zMarkStack.hpp"
namespace MapleRuntime {
// ZThreadLocalData: one store buffer and two generation stacks per OS thread.
struct ThreadGCData {
    StoreBarrierBuffer storeBarrierBuffer;
    std::unique_ptr<MarkThreadLocalStacks> markStacks[2];
};

}
