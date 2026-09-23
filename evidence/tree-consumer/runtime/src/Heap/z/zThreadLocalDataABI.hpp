// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.

#pragma once

#include <cstddef>

namespace MapleRuntime {
// The 64-bit target ABI, independent of the compiler host's data layout.
// ZGC zThreadLocalData.hpp:36-41,115-133: field offsets are added to the
// owning thread's inline GC data offset. These are offsets within GC data.
namespace ThreadGCDataABI {
// Carrier ThreadLocalData points at the current logical thread's inline data.
constexpr size_t GCDataPointer = 96;
constexpr size_t LoadGoodMask = 0;
constexpr size_t LoadBadMask = 8;
constexpr size_t MarkBadMask = 16;
constexpr size_t StoreGoodMask = 24;
constexpr size_t StoreBadMask = 32;
constexpr size_t StoreBarrierBuffer = 40;
} // namespace ThreadGCDataABI
} // namespace MapleRuntime
