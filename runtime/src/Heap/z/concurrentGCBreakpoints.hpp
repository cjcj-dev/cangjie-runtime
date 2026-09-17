// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_CONCURRENT_GC_BREAKPOINTS_HPP
#define MRT_CONCURRENT_GC_BREAKPOINTS_HPP
#include <mutex>
#include <condition_variable>
#include "Base/Macros.h"
namespace MapleRuntime {
// concurrentGCBreakpoints.hpp: requests are serialized by the controlling
// mutator. Notification calls belong to the major GC driver only.
class ConcurrentGCBreakpoints {
    friend class ZBreakpoint;
    static std::mutex mutex;
    static std::condition_variable condition;
    static const char* runTo;
    static bool wantIdle;
    static bool stopped;
    static bool idle;
    static void ResetRequestState();
    static bool IsControlled(); // mutex held
    static void RunToIdleImpl(bool acquiring);
public:
    MRT_EXPORT static void AcquireControl();
    MRT_EXPORT static void ReleaseControl();
    MRT_EXPORT static void RunToIdle();
    // The caller retains the name until this synchronous operation returns.
    MRT_EXPORT static bool RunTo(const char* name);
    static void At(const char* name);
    static void NotifyActiveToIdle();
    static void NotifyIdleToActive();
    static std::mutex& monitor() { return mutex; }
};
}
#endif
