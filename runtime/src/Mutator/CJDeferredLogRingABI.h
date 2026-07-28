//===- CJDeferredLogRingABI.h - Cangjie deferred-log ABI ------*- C++ -*-===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#ifndef CANGJIE_DEFERRED_LOG_RING_ABI_H
#define CANGJIE_DEFERRED_LOG_RING_ABI_H

#include <cstdint>

#define CANGJIE_DEFERRED_LOG_RING_ABI_VERSION 1
#define CANGJIE_DEFERRED_LOG_RING_OFFSET 8
#define CANGJIE_DEFERRED_LOG_RING_INDEX_OFFSET 264
#define CANGJIE_DEFERRED_LOG_RING_CAPACITY 32

#define CANGJIE_DEFERRED_LOG_RING_SYMBOL_IMPL(version, offset, indexOffset, capacity) \
  CJ_MCC_FlushDeferredLogRing_v##version##_##offset##_##indexOffset##_##capacity
#define CANGJIE_DEFERRED_LOG_RING_SYMBOL_EXPAND(version, offset, indexOffset, capacity) \
  CANGJIE_DEFERRED_LOG_RING_SYMBOL_IMPL(version, offset, indexOffset, capacity)
#define CANGJIE_DEFERRED_LOG_RING_SYMBOL \
  CANGJIE_DEFERRED_LOG_RING_SYMBOL_EXPAND( \
      CANGJIE_DEFERRED_LOG_RING_ABI_VERSION, CANGJIE_DEFERRED_LOG_RING_OFFSET, \
      CANGJIE_DEFERRED_LOG_RING_INDEX_OFFSET, CANGJIE_DEFERRED_LOG_RING_CAPACITY)

#define CANGJIE_DEFERRED_LOG_RING_STRING_IMPL(symbol) #symbol
#define CANGJIE_DEFERRED_LOG_RING_STRING_EXPAND(symbol) \
  CANGJIE_DEFERRED_LOG_RING_STRING_IMPL(symbol)
#define CANGJIE_DEFERRED_LOG_RING_SYMBOL_STRING \
  CANGJIE_DEFERRED_LOG_RING_STRING_EXPAND(CANGJIE_DEFERRED_LOG_RING_SYMBOL)

namespace CangjieDeferredLogRingABI {
constexpr uint32_t Version = CANGJIE_DEFERRED_LOG_RING_ABI_VERSION;
constexpr uint32_t Offset = CANGJIE_DEFERRED_LOG_RING_OFFSET;
constexpr uint32_t IndexOffset = CANGJIE_DEFERRED_LOG_RING_INDEX_OFFSET;
constexpr uint32_t Capacity = CANGJIE_DEFERRED_LOG_RING_CAPACITY;
constexpr char FlushFunctionName[] = CANGJIE_DEFERRED_LOG_RING_SYMBOL_STRING;

static_assert(IndexOffset == Offset + Capacity * sizeof(uint64_t),
              "invalid deferred-log ring ABI contract");
} // namespace CangjieDeferredLogRingABI

#endif // CANGJIE_DEFERRED_LOG_RING_ABI_H
