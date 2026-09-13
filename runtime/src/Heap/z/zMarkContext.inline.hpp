// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkStack.hpp"
namespace MapleRuntime {
MarkContext::MarkContext(size_t workerCount, size_t workerId, MarkStripeSet& stripes, MarkThreadLocalStacks& stacks)
    : stripeId(stripes.StripeForWorker(workerCount, workerId)), nstripes(stripes.NStripes()), stacks(&stacks),
      cache(stripes.Count())
{}


} // namespace MapleRuntime
