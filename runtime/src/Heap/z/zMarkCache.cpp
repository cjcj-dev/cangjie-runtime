// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zMarkCache.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zUtils.inline.hpp"
namespace MapleRuntime {
MarkLiveCache::MarkLiveCache(size_t stripeCount) : shift(MARK_STRIPE_SHIFT + Log2Exact(stripeCount)) {}

MarkLiveCache::~MarkLiveCache()
{
    Flush();
}

} // namespace MapleRuntime

#include "Heap/z/zMarkCache.inline.hpp"
