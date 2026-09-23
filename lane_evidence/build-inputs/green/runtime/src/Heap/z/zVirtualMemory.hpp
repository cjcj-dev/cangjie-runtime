// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zVirtualMemory.hpp:31-38.

#pragma once
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zRange.hpp"

namespace MapleRuntime {

class ZVirtualMemory : public ZRange<zoffset, zoffset_end> {
public:
  ZVirtualMemory();
  ZVirtualMemory(zoffset start, size_t size);
  ZVirtualMemory(const ZRange<zoffset, zoffset_end>& range);

  int granule_count() const;
};

} // namespace MapleRuntime
