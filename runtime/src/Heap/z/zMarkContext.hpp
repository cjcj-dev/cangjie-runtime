// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_ZMARKCONTEXT_HPP
#define MRT_ZMARKCONTEXT_HPP
#include <cstddef>
#include "Heap/z/zMarkCache.hpp"

namespace MapleRuntime {
class MarkStripeSet;
class MarkThreadLocalStacks;
class MarkContext {
public:
    MarkContext(size_t workerCount, size_t workerId, MarkStripeSet& stripes, MarkThreadLocalStacks& stacks);

    size_t StripeId() const { return stripeId; }
    size_t NStripes() const { return nstripes; }
    void SetNStripes(size_t value) { nstripes = value; }
    void SetStripeId(size_t value)
    {
        cache.Flush();
        stripeId = value;
    }
    MarkThreadLocalStacks& Stacks() { return *stacks; }
    MarkLiveCache& Cache() { return cache; }

private:
    size_t stripeId;
    size_t nstripes;
    MarkThreadLocalStacks* stacks;
    MarkLiveCache cache;
};
} // namespace MapleRuntime
#endif
