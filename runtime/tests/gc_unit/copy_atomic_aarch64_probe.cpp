// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Freestanding AArch64 utility test. Links the product Copy_aarch64.S object.
// This validates the platform primitive; it is not a complete runtime test.
#include "Base/Copy.h"

alignas(16) static uintptr_t source[100];
alignas(16) static uintptr_t target[100];

extern "C" int copy_atomic_platform_test()
{
    // Include all small counts, both source alignments, both destination
    // alignments, the pipelined loop, and every computed-branch remainder.
    for (size_t fromOffset = 0; fromOffset <= 1; ++fromOffset) {
        for (size_t toOffset = 0; toOffset <= 1; ++toOffset) {
            for (size_t count = 0; count <= 80; ++count) {
                for (size_t i = 0; i < 100; ++i) {
                    source[i] = 0x12340000 + i;
                    target[i] = 0xfeed;
                }
                MapleRuntime::Copy::disjoint_words_atomic(source + fromOffset, target + toOffset, count);
                for (size_t i = 0; i < 100; ++i) {
                    const uintptr_t expected = i >= toOffset && i < toOffset + count
                        ? 0x12340000 + i - toOffset + fromOffset : 0xfeed;
                    if (target[i] != expected || source[i] != 0x12340000 + i) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

extern "C" void _start()
{
    const int result = copy_atomic_platform_test();
    register long status __asm__("x0") = result;
    register long syscallNumber __asm__("x8") = 93;
    __asm__ volatile("svc 0" : : "r"(status), "r"(syscallNumber) : "memory");
    __builtin_unreachable();
}
