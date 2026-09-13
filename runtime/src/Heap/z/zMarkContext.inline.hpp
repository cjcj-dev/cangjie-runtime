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

namespace MapleRuntime {
size_t MarkContext::StripeId() const { return stripeId; }
}

namespace MapleRuntime {
size_t MarkContext::NStripes() const { return nstripes; }
}

namespace MapleRuntime {
void MarkContext::SetNStripes(size_t value) { nstripes = value; }
}

namespace MapleRuntime {
void MarkContext::SetStripeId(size_t value)
    {
        cache.Flush();
        stripeId = value;
    }
}

namespace MapleRuntime {
MarkThreadLocalStacks& MarkContext::Stacks() { return *stacks; }
}

namespace MapleRuntime {
MarkLiveCache& MarkContext::Cache() { return cache; }
}
