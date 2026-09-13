// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#pragma once
#include <cstdint>
#include "Common/TypeDef.h"
namespace MapleRuntime {
// GCPhase describes phases for stw/concurrent gc.
enum GCPhase : uint8_t {
    GC_PHASE_UNDEF = 0,
    GC_PHASE_IDLE = 1,
    GC_PHASE_FINISH = 2,
    GC_PHASE_RECLAIM_SATB_NODE = 3,
    GC_PHASE_INIT = 8,

    // only gc phase after GC_PHASE_INIT ( enum value > GC_PHASE_INIT) needs barrier.
    GC_PHASE_ENUM = 9,
    GC_PHASE_TRACE = 10,
    GC_PHASE_CLEAR_SATB_BUFFER = 11,
    GC_PHASE_POST_TRACE = 12,
    GC_PHASE_PREFORWARD = 13,
    GC_PHASE_FORWARD = 14,
    // Generation-local mark end, before non-strong reference processing.
    // ZGenerationOld::mark_end (zGeneration.cpp:1271).
    GC_PHASE_MARK_COMPLETE = 15,
};

}

namespace MapleRuntime {
namespace MarkPartialArray {
// zGlobals.hpp:82-84. MIN_LENGTH is in elements; our ref slots are 8 bytes,
// same as ZGC's oopSize with compressed oops off.
constexpr size_t MIN_SIZE_SHIFT = 12; // 4K
constexpr size_t MIN_SIZE = static_cast<size_t>(1) << MIN_SIZE_SHIFT;
constexpr size_t MIN_LENGTH = MIN_SIZE / sizeof(MAddress);

}
}

namespace MapleRuntime {
namespace {
constexpr size_t MARK_STRIPE_SHIFT = 20;
}
}
