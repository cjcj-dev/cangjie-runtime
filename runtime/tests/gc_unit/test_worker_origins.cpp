// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "Cangjie.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zHeuristics.hpp"
#include "Heap/z/zGlobals.hpp"
#include "os/Processor.h"
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace MapleRuntime;
extern "C" int CJ_ScheduleManagerInit();
extern "C" void CJ_MRT_CjRuntimeInit();
namespace {
RuntimeParam WorkerParams()
{
    RuntimeParam param{};
    param.heapParam.heapSize = 1024 * 1024;
    param.coParam.processorNum = 1;
    return param;
}
void WorkerInit(RuntimeParam& param, bool env)
{
    setenv("cjUncommit", "0", 1);
    setenv("cjEnableGC", "0", 1);
    if (env) {
        setenv("cjHeapSize", "1GB", 1);
        setenv("cjProcessorNum", "1", 1);
        GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
        CJ_MRT_CjRuntimeInit();
    } else {
        GC_EXPECT_EQ(InitCJRuntime(&param), E_OK);
    }
}
void CheckParallel(bool env, bool explicitFlag)
{
    RuntimeParam param = WorkerParams();
    param.gcParam.parallelGCThreads = 3;
    param.gcParam.parallelGCThreadsSet = explicitFlag;
    if (env && explicitFlag) { setenv("cjParallelGCThreads", "3", 1); }
    else { unsetenv("cjParallelGCThreads"); }
    WorkerInit(param, env);
    const uint32_t heuristic = ZHeuristics::nparallel_workers();
    const uint32_t expected = explicitFlag ? 3 : heuristic;
    const uint32_t actual = ZCollectedHeap::heap()->safepoint_workers()->max_workers();
    std::fprintf(stderr, "PARALLEL_WORKERS_TARGET env=%d explicit=%d heuristic=%u flag=%u actual=%u expected=%u\n",
                 env, explicitFlag, heuristic, ParallelGCThreads, actual, expected);
    GC_EXPECT_NE(heuristic, 3u);
    GC_EXPECT_EQ(actual, expected);
    GC_EXPECT_EQ(ZCollectedHeap::heap()->safepoint_workers()->active_workers(), expected);
}
void RejectZero(const char* flag, bool env)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) _exit(126);
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        RuntimeParam param = WorkerParams();
        if (std::strcmp(flag, "Parallel") == 0) param.gcParam.parallelGCThreadsSet = true;
        if (std::strcmp(flag, "Conc") == 0) param.gcParam.concGCThreadsSet = true;
        if (std::strcmp(flag, "Young") == 0) param.gcParam.youngGCThreadsSet = true;
        if (std::strcmp(flag, "Old") == 0) param.gcParam.oldGCThreadsSet = true;
        if (env) { setenv((std::string("cj") + flag + "GCThreads").c_str(), "0", 1); }
        WorkerInit(param, env);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) transcript.append(buffer, count);
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "WORKER_ZERO_TARGET flag=%s env=%d status=%d\n%s", flag, env, status, transcript.c_str());
    GC_EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    GC_EXPECT_TRUE(transcript.find(env ? "GC worker count must be a positive uint32" : "GCThreads must be") != std::string::npos);
}
void ExplicitGenerations(bool env, bool statically, bool raiseDefault)
{
    auto param = WorkerParams();
    param.gcParam.concGCThreads = 10;
    param.gcParam.concGCThreadsSet = !raiseDefault;
    param.gcParam.youngGCThreads = raiseDefault ? 12 : 4;
    param.gcParam.youngGCThreadsSet = true;
    param.gcParam.oldGCThreads = 3;
    param.gcParam.oldGCThreadsSet = true;
    param.gcParam.staticGCThreads = statically;
    if (env) {
        if (raiseDefault) unsetenv("cjConcGCThreads");
        else setenv("cjConcGCThreads", "10", 1);
        setenv("cjYoungGCThreads", raiseDefault ? "12" : "4", 1);
        setenv("cjOldGCThreads", "3", 1);
        setenv("cjUseDynamicNumberOfGCThreads", statically ? "0" : "1", 1);
    }
    WorkerInit(param, env);
    const uint32_t total = raiseDefault ? std::max(12u, ZHeuristics::nconcurrent_workers()) : 10;
    std::fprintf(stderr, "GENERATION_ORIGIN_TARGET env=%d static=%d raise=%d c=%u y=%u o=%u expected_c=%u\n",
                 env, statically, raiseDefault, ConcGCThreads, ZYoungGCThreads, ZOldGCThreads, total);
    GC_EXPECT_EQ(ConcGCThreads, total);
    GC_EXPECT_EQ(ZYoungGCThreads, raiseDefault ? 12u : 4u);
    GC_EXPECT_EQ(ZOldGCThreads, 3u);
}
void OriginsDefault()
{
    auto param = WorkerParams();
    // Nonzero residual values with default origin must all be ignored.
    param.gcParam.parallelGCThreads = 99;
    param.gcParam.concGCThreads = 99;
    param.gcParam.youngGCThreads = 99;
    param.gcParam.oldGCThreads = 99;
    WorkerInit(param, false);
    std::fprintf(stderr, "WORKER_ORIGIN_DEFAULT_TARGET p=%u c=%u y=%u o=%u\n", ParallelGCThreads, ConcGCThreads, ZYoungGCThreads, ZOldGCThreads);
    GC_EXPECT_EQ(ParallelGCThreads, ZHeuristics::nparallel_workers());
    GC_EXPECT_EQ(ConcGCThreads, ZHeuristics::nconcurrent_workers());
    GC_EXPECT_EQ(ZYoungGCThreads, ConcGCThreads);
    GC_EXPECT_EQ(ZOldGCThreads, ConcGCThreads);
}
void FrozenCPU(bool narrowBeforeInit)
{
    cpu_set_t original;
    GC_EXPECT_EQ(sched_getaffinity(0, sizeof(original), &original), 0);
    cpu_set_t one;
    CPU_ZERO(&one);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &original)) { CPU_SET(cpu, &one); break; }
    }
    GC_EXPECT_TRUE(CPU_COUNT(&original) > 1);
    if (narrowBeforeInit) { GC_EXPECT_EQ(sched_setaffinity(0, sizeof(one), &one), 0); }
    auto param = WorkerParams();
    WorkerInit(param, false);
    const uint32_t before = OS::InitialActiveProcessorCount();
    const uint32_t workers = ZHeuristics::nparallel_workers();
    GC_EXPECT_EQ(sched_setaffinity(0, sizeof(one), &one), 0);
    const uint32_t after = OS::InitialActiveProcessorCount();
    const uint32_t afterWorkers = ZHeuristics::nparallel_workers();
    GC_EXPECT_EQ(sched_setaffinity(0, sizeof(original), &original), 0);
    std::fprintf(stderr, "FROZEN_CPU_TARGET narrow=%d original=%d before=%u after=%u workers=%u/%u\n",
                 narrowBeforeInit, CPU_COUNT(&original), before, after, workers, afterWorkers);
    GC_EXPECT_EQ(before, narrowBeforeInit ? 1u : static_cast<uint32_t>(CPU_COUNT(&original)));
    GC_EXPECT_EQ(after, before);
    GC_EXPECT_EQ(afterWorkers, workers);
    if (narrowBeforeInit) GC_EXPECT_EQ(workers, 1u);
    else GC_EXPECT_TRUE(workers > 1);
}
}
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ParallelExplicitApi) { CheckParallel(false, true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ParallelExplicitEnv) { CheckParallel(true, true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ParallelDefaultApi) { CheckParallel(false, false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ParallelDefaultEnv) { CheckParallel(true, false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, DefaultIgnoresResidualValues) { OriginsDefault(); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ParallelZeroApi) { RejectZero("Parallel", false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ConcZeroApi) { RejectZero("Conc", false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, YoungZeroApi) { RejectZero("Young", false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, OldZeroApi) { RejectZero("Old", false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ParallelZeroEnv) { RejectZero("Parallel", true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ConcZeroEnv) { RejectZero("Conc", true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, YoungZeroEnv) { RejectZero("Young", true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, OldZeroEnv) { RejectZero("Old", true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, FrozenInitialCPU) { FrozenCPU(false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, NarrowInitialCPUControl) { FrozenCPU(true); }

GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ExplicitDynamicApi) { ExplicitGenerations(false, false, false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ExplicitDynamicEnv) { ExplicitGenerations(true, false, false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ExplicitStaticApi) { ExplicitGenerations(false, true, false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ExplicitStaticEnv) { ExplicitGenerations(true, true, false); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ExplicitYoungRaisesDefaultApi) { ExplicitGenerations(false, false, true); }
GC_RUNTIME_OTHER_VM_TEST(WorkerOrigins, ExplicitYoungRaisesDefaultEnv) { ExplicitGenerations(true, false, true); }
