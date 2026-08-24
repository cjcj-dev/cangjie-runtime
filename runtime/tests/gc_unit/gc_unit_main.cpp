// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gc_unittest.hpp"

int main(int argc, char** argv)
{
    constexpr const char* filterPrefix = "--gtest_filter=";
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [--gtest_filter=Suite.Test]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], filterPrefix, std::strlen(filterPrefix)) != 0 ||
            argv[i][std::strlen(filterPrefix)] == '\0') {
            std::fprintf(stderr, "usage: %s [--gtest_filter=Suite.Test]\n", argv[0]);
            return 2;
        }
        (void)setenv("GC_UNIT_FILTER", argv[i] + std::strlen(filterPrefix), 1);
    }
    return MapleRuntime::GcUnit::RunAll();
}
