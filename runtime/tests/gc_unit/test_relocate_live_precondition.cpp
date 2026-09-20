// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zRelocate.cpp:355: relocate_object_inner asserts is_object_live before
// object_size. ForwardObjectExclusive is the product worker entry.
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zRelocate.hpp"

#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

GC_TEST(RelocateLivePrecondition, DeadFromAbortsBeforeSize)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    GC_EXPECT_TRUE(heap.region0->IsRelocatable());
    GC_EXPECT_FALSE(heap.region0->is_object_live(from_object(heap.obj0)));
    const char* testable = std::getenv("MRT_TESTABLE_INTERNALS");
    if (testable == nullptr || testable[0] != '1') {
        return;
    }
    int childStderr[2];
    GC_EXPECT_EQ(pipe(childStderr), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(childStderr[0]);
        if (dup2(childStderr[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(childStderr[1]);
        (void)signal(SIGABRT, SIG_DFL);
        (void)ZRelocate::ForwardObjectExclusive(heap.obj0);
        _exit(0);
    }
    close(childStderr[1]);
    std::string transcript;
    char buffer[512];
    for (;;) {
        const ssize_t count = read(childStderr[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        transcript.append(buffer, static_cast<size_t>(count));
    }
    close(childStderr[0]);
    (void)std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(transcript.find("is_object_live") != std::string::npos);
}

GC_TEST(RelocateLivePrecondition, LiveFromFindHitSkipsCopy)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(heap.region0, heap.obj0));
    const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
    const MAddress to = reinterpret_cast<MAddress>(heap.obj1);
    auto publication = forwarding_for_page(heap.region0, from);
    GC_EXPECT_TRUE(static_cast<bool>(publication));
    GC_EXPECT_EQ(publication->insert(from, to), to);
    GC_EXPECT_TRUE(ZRelocate::ForwardObjectExclusive(heap.obj0) == heap.obj1);
}
