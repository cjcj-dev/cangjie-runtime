// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"
#include "Base/CString.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" char* CJ_MRT_DemangleHandle(const char*);

// Exercise the public product entry, not a test instantiation of Demangler.
// The package abbreviation is emitted by the structured-concurrency compiler.
GC_TEST(UpstreamDemangle, ConcurrentPackageUsesPublicResult)
{
    char* result = CJ_MRT_DemangleHandle("_CNcr11threadScopeHv");
    const bool matches = result != nullptr && std::strcmp(result, "std.concurrent.threadScope()") == 0;
    std::printf("UPSTREAM_DEMANGLE_TARGET package=concurrent actual=%s expected=std.concurrent.threadScope()\n",
                result == nullptr ? "<null>" : result);
    std::free(result);
    GC_EXPECT_TRUE(matches);
}

GC_TEST(UpstreamDemangle, CorePackageUsesPublicResult)
{
    char* result = CJ_MRT_DemangleHandle("_CNat3fooHv");
    const bool matches = result != nullptr && std::strcmp(result, "std.core.foo()") == 0;
    std::printf("UPSTREAM_DEMANGLE_TARGET package=core actual=%s expected=std.core.foo()\n",
                result == nullptr ? "<null>" : result);
    std::free(result);
    GC_EXPECT_TRUE(matches);
}

GC_TEST(UpstreamDemangle, CStringAssignmentControl)
{
    MapleRuntime::CString value("short");
    const MapleRuntime::CString source("a longer independently owned string");
    value = source;
    std::printf("UPSTREAM_CSTRING_TARGET actual=%s\n", value.Str());
    GC_EXPECT_TRUE(value == source);
}
