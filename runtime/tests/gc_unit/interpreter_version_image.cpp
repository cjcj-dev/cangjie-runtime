// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <cstdio>
#include <cstdlib>
#include "Interpreter/RTInterface.h"

// External ABI consumer loaded by the product's dlopen/dlsym path. This image
// contains no runtime implementation; it checks the table delivered by runtime.
extern "C" int interpreter_bridge_init(INT_InterpreterInterface* interpreter,
    DYN_CJNativeInterface* native, int argc, INT_InterpreterArgs argv)
{
    if (native == nullptr || interpreter == nullptr || argc != 1 || argv == nullptr) {
        return 38;
    }
    const int expected = std::atoi(argv[0]);
    const int result = native->version == expected ? 0 : 37;
    std::fprintf(stderr, "INTERPRETER_VERSION_CONSUMER actual=%lld expected=%d result=%d\n",
        static_cast<long long>(native->version), expected, result);
    interpreter->version = INT_INTERPRETER_INTERFACE_VERSION;
    return result;
}
