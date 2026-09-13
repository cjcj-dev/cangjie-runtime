// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_Z_BREAKPOINT_HPP
#define MRT_Z_BREAKPOINT_HPP
namespace MapleRuntime {
class ZBreakpoint {
    static bool startGC;
public:
    static void StartGC();
    static void AtBeforeGC();
    static void AtAfterGC();
    static void AtAfterMarkingStarted();
    static void AtBeforeMarkingCompleted();
    static void AtAfterReferenceProcessingStarted();
};
}
#endif
