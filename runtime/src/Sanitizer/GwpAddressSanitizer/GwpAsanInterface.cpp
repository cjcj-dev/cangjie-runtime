// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RuntimeConfig.h"
#include "GwpAsanInterface.h"

#include <climits>
#include <random>
#include <set>

#include "Base/Log.h"
#include "Base/SpinLock.h"
#include "Heap/Allocator/RegionSpace.h"
#include "ObjectModel/MArray.inline.h"
#include "Sanitizer/SanitizerCompilerCalls.h"
#include "securec.h"

namespace MapleRuntime {
namespace Sanitizer {
using namespace std;

static bool g_gwpEnabled = false;

// sampling config
static long int g_samplingRate = 5000;
static atomic_uint g_counter = { 0 };

// canary logger, <addr, size> pair
static std::map<void*, uint64_t>* g_canary;
static SpinLock g_lock;

static void PrintGwpAsanHelpMessage()
{
    PRINT_INFO("Available flags for cangjie GWP-ASan:\n");
    PRINT_INFO("\tcjEnableGwpAsan\n");
    PRINT_INFO("\t\t- If true, enable GWP-ASan for detect array overflow and acquire leak. (Current Value: %s)\n",
               g_gwpEnabled ? "true" : "false");

    PRINT_INFO("\tcjGwpAsanSampleRate\n");
    PRINT_INFO(
        "\t\t- The probability (1 / SampleRate) that an array which "
        "is acquired by using std.core.acquireArrayRawData is selected for GWP-ASan sampling.\n"
        "\t\t  Default is 5000. Sample rates up to (2^31 - 1) are supported. (Current Value: %lu)\n",
        g_samplingRate);

    PRINT_INFO("\tcjGwpAsanHelp\n");
    PRINT_INFO("\t\t- Print the flag descriptions. (Current Value: true)\n");
}

void SetupGwpAsanAsNeeded()
{
    auto enabled = GetRuntimeConfigValue("cjEnableGwpAsan");
    if (enabled != nullptr) {
        g_gwpEnabled = CString::ParseFlagFromEnv(enabled);
    }

    auto sampling = GetRuntimeConfigValue("cjGwpAsanSampleRate");
    if (sampling != nullptr) {
        char* pEnd{};
        auto val = std::strtol(sampling, &pEnd, 10);
        // not a negative number, full number, overflow
        if (sampling[0] == '-' || *pEnd != '\0' || val <= 0 || val > INT_MAX || errno == ERANGE) {
            LOG(RTLOG_FATAL, "Unsupported cjGwpAsanSampleRate parameter. Valid sample rates range is (0, 2^31 - 1].\n");
            BUILTIN_UNREACHABLE();
        }
        g_samplingRate = static_cast<int>(val);
    }

    // to show the configured value correctly, this must be the last one to do
    auto help = GetRuntimeConfigValue("cjGwpAsanHelp");
    if (help != nullptr && CString::ParseFlagFromEnv(help)) {
        PrintGwpAsanHelpMessage();
    }
}

void OnHeapAllocated(void*, size_t)
{
    if (UNLIKELY(g_gwpEnabled)) {
        g_canary = new (std::nothrow) std::map<void*, uint64_t>();
        CHECK_DETAIL(g_canary != nullptr, "gwpasan metadata allocation failed.");
    }
}

void OnHeapDeallocated(void*, size_t)
{
    if (LIKELY(!g_gwpEnabled)) {
        return;
    }

    if (!g_canary->empty()) {
        for (auto array : *g_canary) {
            Logger::GetLogger().FormatLog(RTLOG_FAIL, true, "Unreleased array: %p", array.first);
        }
        delete g_canary;
        g_canary = nullptr;

        Logger::GetLogger().FormatLog(RTLOG_FATAL, true, "Detect un-released array");
        BUILTIN_UNREACHABLE();
    }

    delete g_canary;
    g_canary = nullptr;
}

} // namespace Sanitizer
} // namespace MapleRuntime