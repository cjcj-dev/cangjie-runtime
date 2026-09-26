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
    static const RuntimeConfigEntryV1 entries[] = {{"cjHeapSize", "64MB"}};
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
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentHeap) { CheckHeap(false, "64MB", 64 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EnvironmentOverridesEmbedded) { CheckHeap(true, "128MB", 128 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(RuntimeConfigV1, EmptyTableEnvironment)
{
    using namespace MapleRuntime;
    setenv("cjHeapSize", "64MB", 1);
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

// Cangjie upstream Base/CString.cpp:394-423; GC sizing remains ZGC-shaped.
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
    const size_t capacity = Heap::GetHeap().GetMaxCapacity();
    const size_t expectedCapacity = (expected + ZGranuleSize - 1) / ZGranuleSize * ZGranuleSize;
    std::fprintf(stderr, "MAX_BYTES_ASSERT input=%s actual=%zu expected=%zu capacity=%zu expected_capacity=%zu\n",
                 input, actual, expectedCapacity, capacity, expectedCapacity);
    GC_EXPECT_EQ(actual, expectedCapacity);
    GC_EXPECT_EQ(capacity, expectedCapacity);
}
void CheckMaximumRejected(const char* input, const char* diagnostic = "Invalid cjHeapSize")
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
    const bool diagnosed = transcript.find(diagnostic) != std::string::npos;
    std::fprintf(stderr, "MAX_REJECT_ASSERT input=%s status=%d rejected=%d diagnosed=%d\n%s",
                 input, status, rejected, diagnosed, transcript.c_str());
    GC_EXPECT_TRUE(rejected && diagnosed);
}
}
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LeadingZeroEight) { CheckMaximumRejected("08MB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LeadingZeroDecimal) { CheckMaximumBytes("040MB", 32UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, SingleUnit) { CheckMaximumBytes("64MB", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Hexadecimal) { CheckMaximumRejected("0x40MB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ExactBytes) { CheckMaximumRejected("67108865"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LowercaseUnit) { CheckMaximumBytes("65537kB", 65537UL * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, TwoCharacterUnit) { CheckMaximumBytes("64MB", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, InteriorWhitespace) { CheckMaximumBytes("6 4MB", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, LeadingWhitespace) { CheckMaximumBytes(" 64MB", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, TrailingWhitespace) { CheckMaximumBytes("64MB ", 64UL * 1024 * 1024); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, NumberOverflow) { CheckMaximumRejected("18446744073709551616KB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, UnitOverflow) { CheckMaximumRejected("18014398509481984KB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Negative) { CheckMaximumRejected("-64MB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Zero) { CheckMaximumRejected("0"); }

GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ApiKilobytes)
{
    using namespace MapleRuntime;
    unsetenv("cjHeapSize");
    unsetenv("cjSoftMaxHeapSize");
    RuntimeParam params{};
    params.heapParam.heapSize = 65537;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const size_t actual = ZHeuristics::max_heap_size();
    const size_t capacity = Heap::GetHeap().GetMaxCapacity();
    std::fprintf(stderr, "MAX_API_ASSERT actual=%zu expected=%zu capacity=%zu\n", actual, 66UL * 1024 * 1024, capacity);
    GC_EXPECT_EQ(actual, 66UL * 1024 * 1024);
    GC_EXPECT_EQ(capacity, 66UL * 1024 * 1024);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ApiEnvironmentBytes)
{
    using namespace MapleRuntime;
    setenv("cjHeapSize", "65537KB", 1);
    unsetenv("cjSoftMaxHeapSize");
    RuntimeParam params{};
    params.heapParam.heapSize = 128 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const size_t actual = ZHeuristics::max_heap_size();
    const size_t capacity = Heap::GetHeap().GetMaxCapacity();
    std::fprintf(stderr, "MAX_API_ENV_ASSERT actual=%zu expected=69206016 capacity=%zu\n", actual, capacity);
    GC_EXPECT_EQ(actual, 66UL * 1024 * 1024);
    GC_EXPECT_EQ(capacity, 66UL * 1024 * 1024);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, TooSmall) { CheckMaximumRejected("1MB"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, AlignmentOverflow) { CheckMaximumRejected("18014398509481983KB", "maximum heap alignment overflows bytes"); }
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, Minimum) { CheckMaximumBytes("2MB", 2UL * 1024 * 1024); }

GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ApiMinimum)
{
    using namespace MapleRuntime;
    unsetenv("cjHeapSize");
    unsetenv("cjSoftMaxHeapSize");
    RuntimeParam params{};
    params.heapParam.heapSize = 2 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    const auto rc = InitCJRuntime(&params);
    std::fprintf(stderr, "MAX_API_MINIMUM_ASSERT rc=%d expected=%d\n", rc, E_OK);
    GC_EXPECT_EQ(rc, E_OK);
    GC_EXPECT_EQ(ZHeuristics::max_heap_size(), 2UL * 1024 * 1024);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ApiTooSmall)
{
    using namespace MapleRuntime;
    unsetenv("cjHeapSize");
    RuntimeParam params{};
    params.heapParam.heapSize = 1024;
    const auto rc = InitCJRuntime(&params);
    std::fprintf(stderr, "MAX_API_SMALL_ASSERT rc=%d expected=%d\n", rc, E_ARGS);
    GC_EXPECT_EQ(rc, E_ARGS);
}
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, ApiOverflow)
{
    using namespace MapleRuntime;
    unsetenv("cjHeapSize");
    RuntimeParam params{};
    params.heapParam.heapSize = SIZE_MAX;
    const auto rc = InitCJRuntime(&params);
    std::fprintf(stderr, "MAX_API_OVERFLOW_ASSERT rc=%d expected=%d\n", rc, E_ARGS);
    GC_EXPECT_EQ(rc, E_ARGS);
}
GC_RUNTIME_OTHER_VM_TEST(MaxHeapSize, EnvironmentOverridesApiOverflow)
{
    using namespace MapleRuntime;
    setenv("cjHeapSize", "64MB", 1);
    unsetenv("cjSoftMaxHeapSize");
    RuntimeParam params{};
    params.heapParam.heapSize = SIZE_MAX;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    const auto rc = InitCJRuntime(&params);
    std::fprintf(stderr, "MAX_API_SOURCE_ASSERT rc=%d expected=%d\n", rc, E_OK);
    GC_EXPECT_EQ(rc, E_OK);
    GC_EXPECT_EQ(ZHeuristics::max_heap_size(), 64UL * 1024 * 1024);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}

namespace {
// Cangjie upstream CString.cpp:394-423: case-insensitive kb/mb/gb, in KB.
// Exercise the API entry and observe the actual heap consumer after startup.
void CheckOfficialHeapUnit(const char* input, size_t expected, bool soft)
{
    using namespace MapleRuntime;
    unsetenv("cjHeapSize");
    unsetenv("cjSoftMaxHeapSize");
    setenv(soft ? "cjSoftMaxHeapSize" : "cjHeapSize", input, 1);
    RuntimeParam params{};
    params.heapParam.heapSize = 32UL * 1024 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.concGCThreads = 2;
    const auto rc = InitCJRuntime(&params);
    const auto expectedRc = expected == 0 ? E_ARGS : E_OK;
    std::fprintf(stderr, "OFFICIAL_UNIT_ACCEPT input=%s soft=%d actual=%d expected=%d\n",
                 input, soft, rc, expectedRc);
    GC_EXPECT_EQ(rc, expectedRc);
    if (rc == E_OK) {
        const size_t actual = soft ? Heap::GetHeap().soft_max_capacity() : Heap::GetHeap().GetMaxCapacity();
        std::fprintf(stderr, "OFFICIAL_UNIT_CAPACITY input=%s soft=%d actual=%zu expected=%zu\n",
                     input, soft, actual, expected);
        GC_EXPECT_EQ(actual, expected);
        GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
    }
}
}
#define OFFICIAL_HEAP_UNIT(id, input, bytes) \
    GC_RUNTIME_OTHER_VM_TEST(OfficialHeapUnit, Hard##id) { CheckOfficialHeapUnit(input, bytes, false); } \
    GC_RUNTIME_OTHER_VM_TEST(OfficialHeapUnit, Soft##id) { CheckOfficialHeapUnit(input, bytes, true); }
OFFICIAL_HEAP_UNIT(GB, "20GB", 20UL * 1024 * 1024 * 1024)
OFFICIAL_HEAP_UNIT(MB, "32768MB", 32UL * 1024 * 1024 * 1024)
OFFICIAL_HEAP_UNIT(kb, "20480kb", 20UL * 1024 * 1024)
OFFICIAL_HEAP_UNIT(Invalid, "20XB", 0)
OFFICIAL_HEAP_UNIT(MixedCase, "20mB", 20UL * 1024 * 1024)
OFFICIAL_HEAP_UNIT(SingleLetter, "20M", 0)
#undef OFFICIAL_HEAP_UNIT
