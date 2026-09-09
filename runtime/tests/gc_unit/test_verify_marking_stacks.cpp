// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Heap/Collector/MarkStripe.h"
#include "Heap/Verify/MarkCompleteVerify.h"
#include "Heap/Verify/VerifyMarkingStacks.h"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;
using namespace MapleRuntime::VerifyMarkingStacks;

GC_TEST(VerifyMarkingStacks, PopulationCountsEntriesAndPublishedChunks)
{
    MarkStripeSet stripes(4);
    MarkThreadLocalStacks local(4);
    local.Push(stripes, 2, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x1000)), true);
    local.Push(stripes, 2, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x2000)), true);

    GC_EXPECT_EQ(local.Population(), 2u);
    GC_EXPECT_EQ(stripes.Population(), 0u);
    GC_EXPECT_TRUE(local.Flush(stripes, true));
    GC_EXPECT_EQ(local.Population(), 0u);
    GC_EXPECT_EQ(stripes.Population(), 1u);
    GC_EXPECT_EQ(stripes.FirstNonEmptyStripe(), 2u);

    MarkingSMR smr(1);
    MarkStripeStack* published = stripes.At(2).StealStack(smr, 0);
    GC_EXPECT_TRUE(published != nullptr);
    MarkStripeStack::Destroy(published);
    smr.Reclaim(0);
    GC_EXPECT_EQ(stripes.Population(), 0u);
    GC_EXPECT_EQ(stripes.FirstNonEmptyStripe(), NO_MARKING_INDEX);
}

GC_OTHER_VM_TEST(VerifyMarkingStacks, MarkingFaceRecordsPositiveProducerAndZeroBoundary)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
    GC_EXPECT_TRUE(Enabled());
    GC_EXPECT_FALSE(MarkCompleteVerify::Enabled());
    const Snapshot before = ReadSnapshot();
    NoteProducer(MarkingGeneration::MAJOR, MarkingContainer::TASK, 7);
    VerifyEmpty(MarkingGeneration::MAJOR, MarkingBoundary::TASK_EXIT, MarkingContainer::TASK, 0, 0, 3);
    const Snapshot after = ReadSnapshot();
    const size_t taskProducer = after.ProducerMax(MarkingGeneration::MAJOR, MarkingContainer::TASK);
    const uint64_t taskExitDelta =
        after.BoundaryCount(MarkingGeneration::MAJOR, MarkingBoundary::TASK_EXIT, MarkingContainer::TASK) -
        before.BoundaryCount(MarkingGeneration::MAJOR, MarkingBoundary::TASK_EXIT, MarkingContainer::TASK);

    std::fprintf(stderr, "DETAIL marking_stack_unit producer=%zu task_exit=%llu\n",
                 taskProducer, static_cast<unsigned long long>(taskExitDelta));
    GC_EXPECT_EQ(taskProducer, 7u);
    GC_EXPECT_EQ(taskExitDelta, 1u);
}

GC_OTHER_VM_TEST(VerifyMarkingStacks, ReceiptsPreserveContainerCoordinate)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
    const Snapshot before = ReadSnapshot();
    VerifyEmpty(MarkingGeneration::MAJOR, MarkingBoundary::END, MarkingContainer::OWNER, 0, 0);
    VerifyEmpty(MarkingGeneration::MAJOR, MarkingBoundary::END, MarkingContainer::FOREIGN, 0, 0);
    const Snapshot after = ReadSnapshot();

    const auto delta = [&before, &after](MarkingContainer container) {
        return after.BoundaryCount(MarkingGeneration::MAJOR, MarkingBoundary::END, container) -
               before.BoundaryCount(MarkingGeneration::MAJOR, MarkingBoundary::END, container);
    };
    std::fprintf(stderr, "DETAIL marking_stack_coordinates owner=%llu foreign=%llu pool=%llu\n",
                 static_cast<unsigned long long>(delta(MarkingContainer::OWNER)),
                 static_cast<unsigned long long>(delta(MarkingContainer::FOREIGN)),
                 static_cast<unsigned long long>(delta(MarkingContainer::POOL)));
    GC_EXPECT_EQ(delta(MarkingContainer::OWNER), 1u);
    GC_EXPECT_EQ(delta(MarkingContainer::FOREIGN), 1u);
    GC_EXPECT_EQ(delta(MarkingContainer::POOL), 0u);
}

GC_OTHER_VM_TEST(VerifyMarkingStacks, RejectsMajorTaskDebtAtTaskExit)
{
#if defined(__linux__)
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
    int stderrPipe[2];
    GC_EXPECT_EQ(pipe(stderrPipe), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(stderrPipe[0]);
        (void)dup2(stderrPipe[1], STDERR_FILENO);
        close(stderrPipe[1]);
        (void)signal(SIGABRT, SIG_DFL);
        VerifyEmpty(MarkingGeneration::MAJOR, MarkingBoundary::TASK_EXIT,
                    MarkingContainer::TASK, 3, 0, 4);
        _exit(0);
    }
    close(stderrPipe[1]);
    std::string output;
    char buffer[1024];
    for (;;) {
        const ssize_t bytes = read(stderrPipe[0], buffer, sizeof(buffer));
        if (bytes > 0) {
            output.append(buffer, static_cast<size_t>(bytes));
            continue;
        }
        if (bytes == 0) {
            break;
        }
        GC_EXPECT_TRUE(errno == EINTR);
    }
    close(stderrPipe[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_FALSE(output.empty());
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(output.find("generation=major boundary=task-exit container=task") != std::string::npos);
    GC_EXPECT_TRUE(output.find("owner=0 worker=4 stripe=-1 pending=3") != std::string::npos);
#else
    GC_EXPECT_TRUE(true);
#endif
}

GC_OTHER_VM_TEST(VerifyMarkingStacks, RejectsPublishedYoungStripeDebtAndAcceptsDrainedControl)
{
#if defined(__linux__)
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_MARKING", "1", 1), 0);
    MarkStripeSet stripes(2);
    MarkThreadLocalStacks local(2);
    local.Push(stripes, 1, MarkStackEntry::MarkAndFollow(reinterpret_cast<BaseObject*>(0x1000)), true);
    GC_EXPECT_TRUE(local.Flush(stripes, true));
    GC_EXPECT_EQ(stripes.Population(), 1u);

    int stderrPipe[2];
    GC_EXPECT_EQ(pipe(stderrPipe), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(stderrPipe[0]);
        (void)dup2(stderrPipe[1], STDERR_FILENO);
        close(stderrPipe[1]);
        (void)signal(SIGABRT, SIG_DFL);
        VerifyEmpty(MarkingGeneration::YOUNG, MarkingBoundary::JOIN, MarkingContainer::STRIPE,
                    stripes.Population(), NO_MARKING_INDEX, NO_MARKING_INDEX, stripes.FirstNonEmptyStripe());
        _exit(0);
    }
    close(stderrPipe[1]);
    std::string output;
    char buffer[1024];
    for (;;) {
        const ssize_t bytes = read(stderrPipe[0], buffer, sizeof(buffer));
        if (bytes > 0) {
            output.append(buffer, static_cast<size_t>(bytes));
            continue;
        }
        if (bytes == 0) {
            break;
        }
        GC_EXPECT_TRUE(errno == EINTR);
    }
    close(stderrPipe[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
    GC_EXPECT_TRUE(output.find("generation=young boundary=join container=stripe") != std::string::npos);
    GC_EXPECT_TRUE(output.find("stripe=1 pending=1") != std::string::npos);

    MarkingSMR smr(1);
    MarkStripeStack* published = stripes.At(1).StealStack(smr, 0);
    GC_EXPECT_TRUE(published != nullptr);
    MarkStripeStack::Destroy(published);
    smr.Reclaim(0);
    GC_EXPECT_EQ(stripes.Population(), 0u);
    VerifyEmpty(MarkingGeneration::YOUNG, MarkingBoundary::JOIN, MarkingContainer::STRIPE,
                stripes.Population(), NO_MARKING_INDEX, NO_MARKING_INDEX, stripes.FirstNonEmptyStripe());
    std::fprintf(stderr,
                 "DETAIL marking_stack_stripe_control abort_signal=%d "
                 "diagnostic=young/join/stripe cut_pending=1 restored_pending=0\n",
                 SIGABRT);
#else
    GC_EXPECT_TRUE(true);
#endif
}

GC_OTHER_VM_TEST(VerifyMarkingStacks, ObjectsFaceOwnsMarkCompleteAdmission)
{
    GC_EXPECT_EQ(setenv("MRT_GCV2_VERIFY_OBJECTS", "1", 1), 0);
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_VERIFY_MARKING"), 0);
    GC_EXPECT_EQ(unsetenv("MRT_GCV2_MARKCOMPLETE"), 0);
    GC_EXPECT_TRUE(MarkCompleteVerify::Enabled());
    GC_EXPECT_FALSE(Enabled());
}
