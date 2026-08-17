// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Collector/ZForwardingLife.h"

#include <cstdio>
#include <cstdlib>

namespace MapleRuntime {

std::atomic<uint64_t> ZForwardingLife::g_retainRefusedReleased{ 0 };
std::atomic<uint64_t> ZForwardingLife::g_retainRefusedClaimed{ 0 };
std::atomic<uint64_t> ZForwardingLife::g_detachWaited{ 0 };

namespace {
struct DumpOnce {
    DumpOnce()
    {
        std::atexit([]() {
            std::fprintf(stderr,
                         "[GCV2][zlife] atexit refuse_released=%llu refuse_claimed=%llu detach_waited=%llu\n",
                         static_cast<unsigned long long>(ZForwardingLife::RetainRefusedReleased()),
                         static_cast<unsigned long long>(ZForwardingLife::RetainRefusedClaimed()),
                         static_cast<unsigned long long>(ZForwardingLife::DetachWaited()));
            std::fflush(stderr);
        });
    }
};
const DumpOnce g_dumpOnce;
} // namespace

} // namespace MapleRuntime
