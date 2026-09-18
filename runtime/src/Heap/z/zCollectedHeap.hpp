// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_COLLECTOR_H
#define MRT_COLLECTOR_H

#include "Heap/z/zAddress.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_set>
#include <vector>

#include "Base/Macros.h"
#include "Heap/z/zDriverPort.hpp"
#include "Heap/z/zStat.hpp"

#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zForwardingLookup.hpp"
namespace MapleRuntime {
enum class Generation : uint8_t;
enum CollectorType {
    NO_COLLECTOR = 0, // No Collector
    PROXY_COLLECTOR,  // Proxy of Collector
    COPY_COLLECTOR,   // Regional-Copying GC
    SMOOTH_COLLECTOR, // wgc
    COLLECTOR_TYPE_COUNT,
};

class Collector;

class ZCollectedHeap {
public:
    static void stop();
};
} // namespace MapleRuntime

#endif // MRT_COLLECTOR_H
