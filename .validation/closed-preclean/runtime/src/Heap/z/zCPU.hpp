// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// gc/z/zCPU.hpp:33-50
#pragma once
#include <cstdint>

namespace MapleRuntime {
class ZCPU {
private:
    // I17 (PLAN §5): GC threads are bare pthreads (no HotSpot Thread object);
    // the thread identity is the address of this thread's _cpu slot.
    struct ZCPUAffinity {
        const void* _thread;
    };

    struct alignas(64) PaddedZCPUAffinity : ZCPUAffinity {};

    static PaddedZCPUAffinity*        _affinity;
    static thread_local const void*   _self;
    static thread_local uint32_t      _cpu;

    static uint32_t id_slow();

public:
    static void initialize();

    static uint32_t count();
    static uint32_t id();
};
} // namespace MapleRuntime
