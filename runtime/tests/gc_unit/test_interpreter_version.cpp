// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <climits>
#include <cstdio>
#include <string>
#include <unistd.h>
#include "Interpreter/RuntimeAPI.h"
#include "gc_unittest.hpp"

#ifdef INTERPRETER_ENABLED
namespace {
void CheckInterpreterVersion(const char* expectedVersion, RTErrorCode expectedResult)
{
    char executable[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    GC_EXPECT_TRUE(length > 0 && static_cast<size_t>(length) < sizeof(executable) - 1);
    executable[length] = '\0';
    const std::string path = std::string(executable).substr(0, std::string(executable).find_last_of('/') + 1) +
        "libcj_interpreter_version_fixture.so";
    RuntimeParam params{};
    params.heapParam.heapSize = 64 * 1024;
    params.coParam.processorNum = 1;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
    const char* arguments[] = {expectedVersion};
    InterpreterParam interpreter{path.c_str(), 1, arguments, nullptr};
    const RTErrorCode result = InitCJInterpreter(&interpreter);
    std::fprintf(stderr, "INTERPRETER_VERSION_TARGET expected_version=%s actual_rc=%d expected_rc=%d\n",
        expectedVersion, static_cast<int>(result), static_cast<int>(expectedResult));
    GC_EXPECT_EQ(result, expectedResult);
    GC_EXPECT_EQ(FiniCJRuntime(), E_OK);
}
}

GC_RUNTIME_OTHER_VM_TEST(InterpreterVersion, AcceptCurrentLayout)
{
    CheckInterpreterVersion("4", E_OK);
}

GC_RUNTIME_OTHER_VM_TEST(InterpreterVersion, RejectPreviousLayout)
{
    CheckInterpreterVersion("3", E_FAILED);
}
#endif
