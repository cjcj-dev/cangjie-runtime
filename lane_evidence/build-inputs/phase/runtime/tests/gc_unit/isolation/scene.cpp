// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Standalone regression of the real header harness (no runtime substitute).
#include "gc_unittest.hpp"
#include <mutex>

using namespace MapleRuntime::GcUnit;
static std::mutex held;

__attribute__((noinline)) void IsolationBlockedWorker()
{
    held.lock();
}

int main()
{
    const char* mode = std::getenv("ISOLATION_MODE");
    const int depth = std::atoi(std::getenv("ISOLATION_DEPTH"));
    constexpr const char* name = "Isolation.Scene";
    if (depth > 0) {
        if (std::getenv("GC_UNIT_OTHER_VM_CHILD") != nullptr) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        const std::string next = std::to_string(depth - 1);
        setenv("ISOLATION_DEPTH", next.c_str(), 1);
        const bool abortScene = std::strcmp(mode, "abort") == 0 || std::strcmp(mode, "wrong") == 0;
        try {
            RunInOtherVm(name, depth == 1 && abortScene ? "ISOLATION_EXPECTED_ABORT" : nullptr);
        } catch (const AssertFailure& error) {
            std::fprintf(stderr, "ISOLATION_FAILURE %s\n", error.what());
            return 1;
        }
    } else {
        std::fprintf(stderr, "ISOLATION_LEAF_PID=%d\n", getpid());
        std::fflush(stderr);
        if (std::strcmp(mode, "block") == 0) {
            held.lock();
            std::thread worker(IsolationBlockedWorker);
            worker.join();
        }
        if (std::strcmp(mode, "abort") == 0 || std::strcmp(mode, "wrong") == 0) {
            std::fprintf(stderr, "%s\n", std::strcmp(mode, "abort") == 0 ?
                         "ISOLATION_EXPECTED_ABORT" : "ISOLATION_WRONG_ABORT");
            std::raise(SIGABRT);
        }
        if (std::strcmp(mode, "missing") == 0) { return 0; }
    }
    std::fprintf(stderr, "GC_UNIT_OTHER_VM_OKIDOKI %s\n", name);
    return 0;
}
