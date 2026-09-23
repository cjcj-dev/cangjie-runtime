// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstdint>
#include <cstddef>
namespace MapleRuntime {
}

namespace MapleRuntime {
namespace MarkPartialArray {
// zGlobals.hpp:82-84. MIN_LENGTH is in elements; our ref slots are 8 bytes,
// same as ZGC's oopSize with compressed oops off.
constexpr size_t MIN_SIZE_SHIFT = 12; // 4K
constexpr size_t MIN_SIZE = static_cast<size_t>(1) << MIN_SIZE_SHIFT;
constexpr size_t MIN_LENGTH = MIN_SIZE / sizeof(uintptr_t);

}
}

namespace MapleRuntime {
constexpr size_t ZGranuleSizeShift = 21;
constexpr size_t ZGranuleSize = size_t(1) << ZGranuleSizeShift;
constexpr size_t ZVirtualToPhysicalRatio = 16;
constexpr size_t ZMaxVirtualReservations = 100;
constexpr int ZPageSizeSmallShift = ZGranuleSizeShift;
constexpr size_t ZPageSizeSmall = size_t(1) << ZPageSizeSmallShift;
extern int ZPageSizeMediumMaxShift;
extern size_t ZPageSizeMediumMax;
extern size_t ZPageSizeMediumMin;
extern bool ZPageSizeMediumEnabled;
extern size_t ZObjectSizeLimitMedium;
extern const int& ZObjectAlignmentSmallShift;
extern int ZObjectAlignmentMediumShift;
extern const int& ZObjectAlignmentSmall;
extern int ZObjectAlignmentMedium;
constexpr size_t ZObjectSizeLimitSmall = ZPageSizeSmall / 8;
constexpr int ZObjectAlignmentLargeShift = ZGranuleSizeShift;
constexpr int ZObjectAlignmentLarge = 1 << ZObjectAlignmentLargeShift;
constexpr size_t ZCacheLineSize = 64;
#define ZCACHE_ALIGNED alignas(MapleRuntime::ZCacheLineSize)
constexpr size_t ZMarkStripeShift = ZGranuleSizeShift;
constexpr size_t MARK_STRIPE_SHIFT = ZMarkStripeShift;
constexpr size_t ZMarkStripesMax = 16;
constexpr size_t ZMarkCacheSize = 1024;
constexpr size_t ZMarkPartialArrayMinSizeShift = 12;
constexpr size_t ZMarkPartialArrayMinSize = size_t(1) << ZMarkPartialArrayMinSizeShift;
constexpr size_t ZMarkPartialArrayMinLength = ZMarkPartialArrayMinSize / sizeof(uintptr_t);
constexpr size_t ZMarkProactiveFlushMax = 10;
constexpr uint64_t ZMarkCompleteTimeout = 200;
// gc/shared/gc_globals.hpp ConcGCThreads; ZCollectedHeap::start_gc_threads
// publishes the runtime budget after heap initialization.
extern uint32_t MaxTenuringThreshold;
extern int32_t ZTenuringThreshold;
extern uint32_t ConcGCThreads;
extern uint32_t ZYoungGCThreads;
extern uint32_t ZOldGCThreads;
}
