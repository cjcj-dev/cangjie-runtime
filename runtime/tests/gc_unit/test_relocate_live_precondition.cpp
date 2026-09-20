// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC zRelocate.cpp:355 / :904: relocate_object_inner reads object_size only
// after assert(is_object_live). A relocatable unmarked from-object must abort
// at that check instead of reading TypeInfo.
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

#if defined(MRT_TESTABLE_INTERNALS)
namespace MapleRuntime {
struct RelocateLivePreconditionAccess {
    static BaseObject* RelocateInner(BaseObject* from, ZPage* page)
    {
        return ZGeneration::generation(page->generation_id())->relocate().relocate_object_inner(from, page);
    }
};
}

namespace {
int RunDeadRelocateChild(ZPage* page, BaseObject* object)
{
    int childStderr[2];
    if (pipe(childStderr) != 0) {
        return 126;
    }
    const pid_t child = fork();
    if (child < 0) {
        return 126;
    }
    if (child == 0) {
        close(childStderr[0]);
        if (dup2(childStderr[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(childStderr[1]);
        (void)signal(SIGABRT, SIG_DFL);
        (void)RelocateLivePreconditionAccess::RelocateInner(object, page);
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
    if (waitpid(child, &status, 0) != child) {
        return 127;
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        return 1;
    }
    if (transcript.find("IsSurvivedObject") == std::string::npos) {
        return 2;
    }
    return 0;
}
}

GC_TEST(RelocateLivePrecondition, DeadFromAbortsBeforeSize)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    GC_EXPECT_TRUE(heap.region0->IsRelocatable());
    GC_EXPECT_FALSE(heap.region0->is_object_live(from_object(heap.obj0)));
    const char* testable = std::getenv("MRT_TESTABLE_INTERNALS");
    if (testable != nullptr && testable[0] == '1') {
        GC_EXPECT_EQ(RunDeadRelocateChild(heap.region0, heap.obj0), 0);
    }
}

GC_TEST(RelocateLivePrecondition, LiveFromDoesNotHitLiveCheck)
{
    GcHeapFixture heap;
    heap.InstallPageOwner(heap.region0);
    GC_EXPECT_TRUE(GcHeapFixture::MarkStrong(heap.region0, heap.obj0));
    GC_EXPECT_TRUE(heap.region0->is_object_live(from_object(heap.obj0)));
    Heap::GetHeap().GetZGeneration(heap.region0->generation_id()).set_phase(ZGenerationPhase::Relocate);
    BaseObject* to = RelocateLivePreconditionAccess::RelocateInner(heap.obj0, heap.region0);
    (void)to;
    GC_EXPECT_TRUE(heap.region0->is_object_live(from_object(heap.obj0)));
}
#endif
