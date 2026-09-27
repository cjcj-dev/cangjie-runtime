// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkStack.hpp"
namespace MapleRuntime {
MarkContext::MarkContext(size_t workerCount, size_t workerId, MarkStripeSet& stripes, MarkThreadLocalStacks& stacks)
    : stripe(stripes.StripeForWorker(workerCount, workerId)), nstripes(stripes.NStripes()), stacks(&stacks),
      cache(stripes.NStripes())
{}


} // namespace MapleRuntime

namespace MapleRuntime {
MarkStripe* MarkContext::Stripe() const { return stripe; }
}

namespace MapleRuntime {
size_t MarkContext::NStripes() const { return nstripes; }
}

namespace MapleRuntime {
void MarkContext::SetNStripes(size_t value)
{
    cache.SetNStripes(value);
    nstripes = value;
}
}

namespace MapleRuntime {
void MarkContext::SetStripe(MarkStripe* value)
    {
        stripe = value;
    }
}

namespace MapleRuntime {
MarkThreadLocalStacks& MarkContext::Stacks() { return *stacks; }
}

namespace MapleRuntime {
MarkLiveCache& MarkContext::Cache() { return cache; }
}
