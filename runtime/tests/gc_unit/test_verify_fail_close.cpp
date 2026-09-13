// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Verify/ZVerify.h"
#include "ObjectModel/RefField.inline.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

template <typename Fn>
void ExpectSceneAbort(const char* expectedDiagnostic, Fn&& fn)
{
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
        fn();
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
    std::fflush(stderr);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(transcript.find(expectedDiagnostic) != std::string::npos);
}

} // namespace

// zVerify.cpp:119-128 / zAddress.inline.hpp:505-522: illegal addresses are
// rejected before metadata access; no region inventory or scene counters.
GC_OTHER_VM_TEST(ZVerify, RejectsColoredAddressWithoutUncoloring)
{
    GcHeapFixture fixture;
    const uintptr_t colored = reinterpret_cast<uintptr_t>(fixture.obj0) | ZPointerRemapped00;
    ExpectSceneAbort("Bad object", [&] {
        ZVerify::Object(reinterpret_cast<BaseObject*>(colored), &colored);
    });
    ZVerify::Object(fixture.obj0, &fixture.obj0);
}

GC_OTHER_VM_TEST(ZVerify, RejectsUnmanagedAddress)
{
    GcHeapFixture fixture;
    ExpectSceneAbort("Bad object", [&] {
        ZVerify::Object(reinterpret_cast<BaseObject*>(0x1000), nullptr);
    });
    ZVerify::Object(fixture.obj0, &fixture.obj0);
}

GC_OTHER_VM_TEST(ZVerify, RememberedCurrentAndPreviousFaces)
{
    GcHeapFixture fixture;
    RememberedSet& remset = Heap::GetHeap().GetRememberedSet();
    remset.Initialize(fixture.heapStart, 2 * RegionInfo::UNIT_SIZE);
    const MAddress slot = reinterpret_cast<MAddress>(fixture.obj0) + TYPEINFO_PTR_SIZE;
    remset.Record(slot);
    GC_EXPECT_TRUE(remset.Contains(slot));
    GC_EXPECT_FALSE(remset.ContainsPrevious(slot));
    GC_EXPECT_FALSE(remset.IsClearInRange(fixture.heapStart, RegionInfo::UNIT_SIZE, true));
    remset.FlipForMinor();
    GC_EXPECT_FALSE(remset.Contains(slot));
    GC_EXPECT_TRUE(remset.ContainsPrevious(slot));
    GC_EXPECT_TRUE(remset.IsClearInRange(fixture.heapStart, RegionInfo::UNIT_SIZE, true));
}
