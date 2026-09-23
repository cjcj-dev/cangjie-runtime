// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#ifndef MRT_GC_CHILD_PROCESS_HPP
#define MRT_GC_CHILD_PROCESS_HPP

#if defined(__linux__)
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <thread>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace MapleRuntime::GcUnit {
// Only the outer supervisor owns the process group. Nested exec scenes stay
// in that group, so terminating their intermediate parent cannot detach them
// from timeout diagnostics or cleanup. Subreaping lets this supervisor wait
// for those scenes after their immediate parent has been terminated.
class ChildVmSupervisor {
public:
    ChildVmSupervisor()
    {
        const char* inherited = std::getenv("GC_UNIT_OTHER_VM_GROUP");
        ownsGroup = inherited == nullptr || std::to_string(getpgrp()) != inherited;
        if (ownsGroup) {
            valid = prctl(PR_GET_CHILD_SUBREAPER, &previousSubreaper) == 0 &&
                prctl(PR_SET_CHILD_SUBREAPER, 1) == 0;
        }
    }
    ~ChildVmSupervisor()
    {
        if (ownsGroup && valid) { (void)prctl(PR_SET_CHILD_SUBREAPER, previousSubreaper); }
    }
    bool OwnsGroup() const { return ownsGroup; }
    bool IsValid() const { return valid; }

private:
    int previousSubreaper = 0;
    bool ownsGroup = false;
    bool valid = true;
};

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

inline void TerminateChildVmGroup(pid_t group)
{
    // Stop every scene before enumerating it: no intermediate supervisor can
    // exit and no scene can fork while its process group is being diagnosed.
    (void)kill(-group, SIGSTOP);
    if (DIR* processes = opendir("/proc")) {
        while (dirent* entry = readdir(processes)) {
            char* end = nullptr;
            const long value = std::strtol(entry->d_name, &end, 10);
            if (*end != '\0' || value <= 0) { continue; }
            const pid_t pid = static_cast<pid_t>(value);
            if (getpgid(pid) == group) { DumpChildStacks(pid); }
        }
        closedir(processes);
    } else {
        std::fprintf(stderr, "[ ERROR ] cannot enumerate other-vm process group: %d\n", errno);
    }
    (void)kill(-group, SIGKILL);
    int status = 0;
    // The subreaper adopts nested scenes when their supervisor exits. Reap
    // only this invocation's group, never an unrelated test's child.
    while (waitpid(-group, &status, 0) > 0 || errno == EINTR) {}
}

inline bool WaitChildExit(pid_t child, int& status,
                          std::chrono::steady_clock::time_point deadline, bool ownsGroup = false)
{
    for (;;) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (ownsGroup && kill(-child, 0) == 0) {
                // Cleanup is independent of the direct child's result. Like
                // HotSpot TEST_OTHER_VM (unittest.hpp:98), the caller judges
                // that child's exit status plus its completion sentinel.
                TerminateChildVmGroup(child);
            }
            return true;
        }
        if (result < 0 && errno != EINTR) { return false; }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (ownsGroup) {
                TerminateChildVmGroup(child);
                return false;
            }
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
