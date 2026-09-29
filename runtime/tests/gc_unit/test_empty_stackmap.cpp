// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <cstdio>
#include "StackMap/StackMap.h"
#include "gc_unittest.hpp"
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
namespace {
// An input image in the existing ELF metadata ABI. These tests cover the
// shipped header decoder; compiler-emitted entry slots are tested separately.
struct EmptyMetadata {
    int32_t slot = 0;
    uint32_t code[3] = {};
    uint32_t descriptor[8] = {};
};

void CheckEmpty(bool descriptorPresent)
{
    EmptyMetadata image;
    if (descriptorPresent) {
        image.slot = reinterpret_cast<char*>(image.descriptor) - reinterpret_cast<char*>(&image.slot);
    }
    const Uptr pc = reinterpret_cast<Uptr>(image.code);
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        // The only result sent to the parent is computed from product APIs.
        const auto desc = MFuncDesc::GetFuncDesc(pc);
        const auto head = CompressedStackMapHead::GetStackMapHead(pc);
        StackMapBuilder builder(pc, pc, 0);
        unsigned result = 0;
        if ((desc != nullptr) == descriptorPresent) { result |= 1; }
        if (!head.IsValid()) { result |= 2; }
        if (!head.GetStackMapEntry(pc, pc).IsValid()) { result |= 4; }
        if (!builder.Build<HeapReferenceMap>().IsValid()) { result |= 8; }
        if (!builder.Build<StackPtrMap>().IsValid()) { result |= 16; }
        if (!builder.Build<MethodMap>().IsValid()) { result |= 32; }
        if (builder.GetInvalidReason() == StackMapInvalidReason::ZERO_ENTRIES) { result |= 64; }
        const auto wrote = write(output[1], &result, sizeof(result));
        _exit(wrote == sizeof(result) ? 0 : 2);
    }
    close(output[1]);
    unsigned result = 0;
    const auto bytes = read(output[0], &result, sizeof(result));
    close(output[0]);
    int status = 0;
    const auto waited = waitpid(child, &status, 0);
    // A decode fault cannot terminate this assertion's process or mask it
    // behind an earlier existence assertion.
    std::fprintf(stderr, "EMPTY_STACKMAP_TARGET descriptor=%d result=%u bytes=%zd status=%d waited=%d\n",
                 descriptorPresent, result, bytes, status, waited == child);
    GC_EXPECT_EQ(result, 127u);
    GC_EXPECT_EQ(bytes, static_cast<ssize_t>(sizeof(result)));
    GC_EXPECT_EQ(status, 0);
}
}
GC_TEST(EmptyStackMap, AbsentDescriptor) { CheckEmpty(false); }
GC_TEST(EmptyStackMap, AbsentStackMap) { CheckEmpty(true); }
#endif
