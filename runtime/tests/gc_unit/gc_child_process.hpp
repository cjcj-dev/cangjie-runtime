// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_GC_CHILD_PROCESS_HPP
#define MRT_GC_CHILD_PROCESS_HPP

#if defined(__linux__)
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cerrno>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace MapleRuntime::GcUnit {
// This deadline reports a harness failure; it never substitutes for a scenario
// assertion. Capture all child threads before terminating an unresponsive VM.
inline void DumpChildStacks(pid_t child)
{
    const std::string pid = std::to_string(child);
    std::fprintf(stderr, "[ TIMEOUT ] other-vm pid=%s; collecting child stacks\n", pid.c_str());
    std::fflush(stderr);
    const pid_t debugger = fork();
    if (debugger == 0) {
        execlp("gdb", "gdb", "-nx", "-batch", "-ex", "set pagination off",
               "-ex", "thread apply all bt", "-p", pid.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (debugger < 0) {
        std::fprintf(stderr, "[ ERROR ] cannot fork stack collector: %d\n", errno);
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int status = 0;
    for (;;) {
        const pid_t result = waitpid(debugger, &status, WNOHANG);
        if (result == debugger) {
            std::fprintf(stderr, "[ STACKS ] collector status=%d\n", status);
            return;
        }
        if (result < 0 && errno != EINTR) { return; }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(debugger, SIGKILL);
            while (waitpid(debugger, &status, 0) < 0 && errno == EINTR) {}
            std::fprintf(stderr, "[ ERROR ] stack collector exceeded 10 seconds\n");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

inline bool WaitChildExit(pid_t child, int& status,
                          std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) { return true; }
        if (result < 0 && errno != EINTR) { return false; }
        if (std::chrono::steady_clock::now() >= deadline) {
            DumpChildStacks(child);
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
} // namespace MapleRuntime::GcUnit
#endif
#endif
