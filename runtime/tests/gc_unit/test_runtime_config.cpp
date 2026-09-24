// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "gc_unittest.hpp"
#include "CangjieRuntime.h"
#include "RuntimeConfig.h"
#include "Heap/z/zHeuristics.hpp"
#include "Base/Log.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zGlobals.hpp"
#include <sys/wait.h>
#include <csignal>
#include <cstring>
#include <string>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>

extern "C" void CJ_MRT_CjRuntimeInit();
extern "C" void CJ_MRT_CjRuntimeInitWithConfigV1(const MapleRuntime::RuntimeConfigEntryV1*, size_t);
extern "C" int CJ_ScheduleManagerInit();

namespace {
void CheckHeap(bool embedded, const char* environment, size_t expected)
{
    using namespace MapleRuntime;
    if (environment == nullptr) {
        unsetenv("cjHeapSize");
    } else {
        setenv("cjHeapSize", environment, 1);
    }
    // Keep the configuration alive for the runtime's lifetime, as codegen does.
    static const RuntimeConfigEntryV1 entries[] = {{"cjHeapSize", "64M"}};
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (embedded) {
        CJ_MRT_CjRuntimeInitWithConfigV1(entries, 1);
    } else {
        CJ_MRT_CjRuntimeInit();
    }
    const size_t actual = ZHeuristics::max_heap_size() / 1024;
    std::printf("RUNTIME_CONFIG_HEAP actual_kib=%zu expected_kib=%zu\n", actual, expected);
    std::fflush(stdout);
    GC_EXPECT_EQ(actual, expected);
}
}

// Startup argument delivery corresponds to HotSpot threads.cpp:495, before
// heap initialization. Each case executes the real exported product entry.
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EmbeddedHeap) { CheckHeap(true, nullptr, 64 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentHeap) { CheckHeap(false, "64M", 64 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentOverridesEmbedded) { CheckHeap(true, "128M", 128 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EmptyTableEnvironment)
{
    using namespace MapleRuntime;
    setenv("cjHeapSize", "64M", 1);
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    CJ_MRT_CjRuntimeInitWithConfigV1(nullptr, 0);
    const size_t actual = ZHeuristics::max_heap_size() / 1024;
    std::printf("RUNTIME_CONFIG_HEAP actual_kib=%zu expected_kib=65536\n", actual);
    GC_EXPECT_EQ(actual, 64 * 1024UL);
}

namespace {
void CheckHeapDumpPath(bool embedded, const char* environment, const char* expectedBase)
{
    using namespace MapleRuntime;
    if (environment == nullptr) {
        unsetenv("cjHeapDumpLog");
    } else {
        setenv("cjHeapDumpLog", environment, 1);
    }
    // Existing absolute directories: no temporary files or OOM trigger needed.
    static const RuntimeConfigEntryV1 entries[] = {{"cjHeapDumpLog", "/usr/"}};
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    if (embedded) {
        CJ_MRT_CjRuntimeInitWithConfigV1(entries, 1);
    } else {
        CJ_MRT_CjRuntimeInit();
    }
    CString actual;
    Logger::GetLogPath("cjHeapDumpLog", actual);
    const std::string expected = std::string(expectedBase) + "." + std::to_string(getpid());
    std::printf("RUNTIME_CONFIG_HEAP_DUMP_PATH actual=%s expected=%s\n", actual.Str(), expected.c_str());
    std::fflush(stdout);
    GC_EXPECT_EQ(std::strcmp(actual.Str(), expected.c_str()), 0);
}
}

// HotSpot heapDumper.cpp:2994 consumes the configured path. Exercise the real
// product path consumer after startup; path parsing and validation stay intact.
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EmbeddedHeapDumpPath)
{
    CheckHeapDumpPath(true, nullptr, "/usr");
}
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentHeapDumpPath)
{
    CheckHeapDumpPath(false, "/usr/", "/usr");
}
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentOverridesEmbeddedHeapDumpPath)
{
    CheckHeapDumpPath(true, "/var/", "/var");
}

// HotSpot arguments.cpp:2112-2125 and parseInteger.hpp:120-176.
// Observe the maximum produced by the exported managed runtime entry, not a
// separately compiled parser. Each registered case runs in a fresh process.
namespace {
void CheckMaximumBytes(const char* input, size_t expected)
{
    using namespace MapleRuntime;
    setenv("cjHeapSize", input, 1);
    setenv("cjProcessorNum", "1", 1);
    setenv("cjConcGCThreads", "2", 1);
    unsetenv("cjSoftMaxHeapSize");
    GC_EXPECT_EQ(CJ_ScheduleManagerInit(), 0);
    CJ_MRT_CjRuntimeInit();
    const size_t actual = ZHeuristics::max_heap_size();
    const size_t capacity = Heap::GetHeap().max_capacity();
    const size_t expectedCapacity = (expected + ZGranuleSize - 1) / ZGranuleSize * ZGranuleSize;
    std::fprintf(stderr, "MAX_BYTES_ASSERT input=%s actual=%zu expected=%zu capacity=%zu expected_capacity=%zu\n",
                 input, actual, expected, capacity, expectedCapacity);
    GC_EXPECT_EQ(actual, expected);
    GC_EXPECT_EQ(capacity, expectedCapacity);
}
void CheckMaximumRejected(const char* input)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) _exit(126);
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        setenv("cjHeapSize", input, 1);
        setenv("cjProcessorNum", "1", 1);
        setenv("cjConcGCThreads", "2", 1);
        unsetenv("cjSoftMaxHeapSize");
        CJ_ScheduleManagerInit();
        CJ_MRT_CjRuntimeInit();
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
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    const bool diagnosed = transcript.find("Invalid cjHeapSize") != std::string::npos;
    std::fprintf(stderr, "MAX_REJECT_ASSERT input=%s status=%d rejected=%d diagnosed=%d\n%s",
                 input, status, rejected, diagnosed, transcript.c_str());
    GC_EXPECT_TRUE(rejected && diagnosed);
}
}
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LeadingZeroEight) { CheckMaximumBytes("08M", 8UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LeadingZeroDecimal) { CheckMaximumBytes("040M", 40UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, SingleUnit) { CheckMaximumBytes("64M", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Hexadecimal) { CheckMaximumBytes("0x40M", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ExactBytes) { CheckMaximumBytes("67108865", 67108865); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LowercaseUnit) { CheckMaximumBytes("65537k", 65537UL * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, TwoCharacterUnit) { CheckMaximumRejected("64MB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, InteriorWhitespace) { CheckMaximumRejected("6 4MB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LeadingWhitespace) { CheckMaximumRejected(" 64M"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, TrailingWhitespace) { CheckMaximumRejected("64M "); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, NumberOverflow) { CheckMaximumRejected("18446744073709551616"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, UnitOverflow) { CheckMaximumRejected("18014398509481984K"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Negative) { CheckMaximumRejected("-64M"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Zero) { CheckMaximumRejected("0"); }
