// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Standalone regression runner for the real other-vm harness (no runtime SO).
#include "gc_unittest.hpp"
#include <sys/times.h>

using namespace MapleRuntime::GcUnit;

static void LeaveGroupMember()
{
    // CompleteTestRun has already flushed OKIDOKI. Synchronize the descendant's
    // closed output descriptors before returning from this exit callback.
    int ready[2];
    if (pipe(ready) != 0) { _exit(125); }
    const pid_t descendant = fork();
    if (descendant < 0) { _exit(125); }
    if (descendant == 0) {
        close(ready[0]);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        const char token = 'R';
        if (write(ready[1], &token, 1) != 1) { _exit(125); }
        close(ready[1]);
        for (;;) { pause(); }
    }
    close(ready[1]);
    char token = 0;
    if (read(ready[0], &token, 1) != 1 || token != 'R') { _exit(125); }
    close(ready[0]);
}

GC_OTHER_VM_TEST(OtherVmExit, GroupMemberAfterSentinel)
{
    if (std::atexit(LeaveGroupMember) != 0) { throw AssertFailure("atexit failed"); }
}
GC_OTHER_VM_TEST(OtherVmExit, Clean) {}
GC_OTHER_VM_TEST(OtherVmExit, NonzeroAfterSentinel)
{
    if (std::atexit([] { _exit(7); }) != 0) { throw AssertFailure("atexit failed"); }
}
GC_OTHER_VM_TEST(OtherVmExit, NonzeroWithGroupMember)
{
    if (std::atexit([] { LeaveGroupMember(); _exit(7); }) != 0) {
        throw AssertFailure("atexit failed");
    }
}
GC_OTHER_VM_TEST(OtherVmExit, MissingSentinel) { _exit(0); }
GC_OTHER_VM_TEST(OtherVmExit, SignalAfterSentinel)
{
    if (std::atexit([] { raise(SIGKILL); }) != 0) { throw AssertFailure("atexit failed"); }
}

GC_OTHER_VM_TEST(OtherVmExit, ExitStatus3)
{
    std::fprintf(stderr, "exit-three-stderr\n");
    _exit(3);
}
GC_OTHER_VM_TEST(OtherVmExit, Signal11)
{
    // Exercise tail truncation, with a recognizable final diagnostic.
    const std::string output(5000, 'x');
    std::fprintf(stderr, "%s\nsignal-eleven-stderr\n", output.c_str());
    std::fflush(stderr);
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
    _exit(125);
}

GC_OTHER_VM_TEST(OtherVmExit, BodyStallBeforeSentinel)
{
    std::this_thread::sleep_for(std::chrono::seconds(65));
}
GC_OTHER_VM_TEST(OtherVmExit, ExitStallAfterSentinel)
{
    if (std::atexit([] { for (;;) { pause(); } }) != 0) { throw AssertFailure("atexit failed"); }
}

static bool CheckPhaseTiming(const std::string& diagnostic, bool bodyStall, bool exitStall)
{
    const auto duration = [&](const char* key) -> long long {
        const size_t start = diagnostic.find(key);
        if (start == std::string::npos) { return -1; }
        const char* value = diagnostic.c_str() + start + std::strlen(key);
        char* end = nullptr;
        const long long result = std::strtoll(value, &end, 10);
        return end != value && *end == ';' && result >= 0 ? result : -1;
    };
    // Each of the three independently truncated millisecond intervals can lose <1ms.
    const long long exec = duration("; exec_ms=");
    const long long body = duration("; body_ms=");
    const long long exit = duration("; exit_ms=");
    if (exec < 0 || body < 0) { return false; }
    if (bodyStall) {
        return body > 0 && exec + body >= 59998 &&
            diagnostic.find("; exit_ms=not-started; pending_phase=body") != std::string::npos &&
            diagnostic.find("; sentinel=missing") != std::string::npos;
    }
    if (exitStall) {
        return exit > 0 && exec + body + exit >= 59998 &&
            diagnostic.find("; pending_phase=exit") != std::string::npos &&
            diagnostic.find("; sentinel=seen") != std::string::npos;
    }
    return exit >= 0 && diagnostic.find("; pending_phase=none") != std::string::npos;
}

static bool CheckProcFallback()
{
    int output[2];
    if (pipe(output) != 0) { return false; }
    const pid_t child = fork();
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(125); }
        close(output[1]);
        // Exercise the actual execlp failure, with no debugger dependency.
        if (setenv("PATH", "/nonexistent-gc-unit-debugger", 1) != 0) { _exit(125); }
        if (prctl(PR_SET_NAME, "gc ) proc test") != 0) { _exit(125); }
        // Give both stat counters a nonzero positive control. Bound this
        // diagnostic-only fixture independently of the other-vm budget.
        tms cpu {};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            for (int i = 0; i < 10000; ++i) { (void)getpid(); }
            if (times(&cpu) == static_cast<clock_t>(-1)) { _exit(125); }
            if (std::chrono::steady_clock::now() >= deadline) { _exit(125); }
        } while (cpu.tms_utime == 0 || cpu.tms_stime == 0);
        std::fprintf(stderr, "EXPECTED_PROC utime=%llu stime=%llu\n",
                     static_cast<unsigned long long>(cpu.tms_utime),
                     static_cast<unsigned long long>(cpu.tms_stime));
        DumpChildStacks(getpid());
        _exit(0);
    }
    close(output[1]);
    if (child < 0) { close(output[0]); return false; }
    std::string diagnostic;
    char buffer[1024];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) != 0) {
        if (count > 0) { diagnostic.append(buffer, static_cast<size_t>(count)); }
        else if (errno != EINTR) { break; }
    }
    close(output[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    std::printf("OBSERVED_PROC %s", diagnostic.c_str());
    const size_t line = diagnostic.find("[ PROC ] pid=");
    int reportedPid = 0;
    char wchan[256] = {};
    unsigned long long utime = 0, stime = 0;
    unsigned long long expectedUser = 0, expectedSystem = 0;
    const size_t expected = diagnostic.find("EXPECTED_PROC utime=");
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        line != std::string::npos && expected != std::string::npos &&
        std::sscanf(diagnostic.c_str() + expected, "EXPECTED_PROC utime=%llu stime=%llu",
                    &expectedUser, &expectedSystem) == 2 && expectedUser > 0 && expectedSystem > 0 &&
        std::sscanf(diagnostic.c_str() + line,
                    "[ PROC ] pid=%d wchan=%255s stat.utime=%llu stat.stime=%llu",
                    &reportedPid, wchan, &utime, &stime) == 4 && reportedPid == child &&
        utime >= expectedUser && stime >= expectedSystem && std::strcmp(wchan, "unavailable") != 0;
}

int main(int argc, char** argv)
{
    if (std::getenv("GC_UNIT_OTHER_VM_CHILD") != nullptr) {
        if (argc != 2 || std::strncmp(argv[1], "--gtest_filter=", 15) != 0) { return 125; }
        setenv("GC_UNIT_FILTER", argv[1] + 15, 1);
        return CompleteTestRun(RunAll());
    }
    int failed = 0;
    for (const auto& test : Registry()) {
        const std::string name = std::string(test.suite) + "." + test.name;
        const bool expected = std::strcmp(test.name, "Clean") == 0 ||
            std::strcmp(test.name, "GroupMemberAfterSentinel") == 0;
        bool accepted = true;
        std::string diagnostic;
        try { RunInOtherVm(name); }
        catch (const AssertFailure& failure) {
            accepted = false;
            diagnostic = failure.what();
            std::fprintf(stderr, "OBSERVED %s: %s\n", name.c_str(), failure.what());
        }
        // RunInOtherVm must consume its descendants before relinquishing its
        // subreaper scope. No unrelated children are started by this runner.
        int status = 0;
        errno = 0;
        const pid_t remaining = waitpid(-1, &status, WNOHANG);
        const bool reaped = remaining == -1 && errno == ECHILD;
        bool diagnosticOk = true;
        const bool exitThree = std::strcmp(test.name, "ExitStatus3") == 0;
        const bool signalEleven = std::strcmp(test.name, "Signal11") == 0;
        if (exitThree || signalEleven) {
            const std::string marker = "Child stderr tail (<=4096 bytes):\n";
            const size_t tail = diagnostic.find(marker);
            diagnosticOk = diagnostic.find(exitThree ? "Exited with exit status 3" :
                "Terminated by signal 11") != std::string::npos &&
                diagnostic.find("sentinel=missing") != std::string::npos &&
                diagnostic.find(exitThree ? "exit-three-stderr\n" : "signal-eleven-stderr\n") != std::string::npos &&
                tail != std::string::npos && diagnostic.size() - tail - marker.size() <= 4096;
            if (signalEleven) {
                diagnosticOk = diagnosticOk && diagnostic.size() - tail - marker.size() == 4096 &&
                    diagnostic.find("(core ") != std::string::npos;
            }
            std::printf("ASSERT_DIAGNOSTIC %s %s\n", name.c_str(), diagnosticOk ? "PASS" : "FAIL");
        }
        const bool bodyStall = std::strcmp(test.name, "BodyStallBeforeSentinel") == 0;
        const bool exitStall = std::strcmp(test.name, "ExitStallAfterSentinel") == 0;
        const bool nonzeroExit = std::strcmp(test.name, "NonzeroAfterSentinel") == 0;
        if (bodyStall || exitStall || nonzeroExit) {
            const bool timingOk = CheckPhaseTiming(diagnostic, bodyStall, exitStall);
            std::printf("ASSERT_PHASE %s %s\n", name.c_str(), timingOk ? "PASS" : "FAIL");
            failed += !timingOk;
        }
        const bool pass = accepted == expected && reaped && diagnosticOk;
        std::printf("ASSERT %s accepted=%d expected=%d reaped=%d %s\n",
                    name.c_str(), accepted, expected, reaped, pass ? "PASS" : "FAIL");
        failed += !pass;
        // A cleanup cut must leave evidence, but must not leave processes on
        // the worker after the regression runner has recorded its assertion.
        if (!reaped) {
            const std::string children = "/proc/self/task/" + std::to_string(getpid()) + "/children";
            if (FILE* list = std::fopen(children.c_str(), "r")) {
                int pid = 0;
                while (std::fscanf(list, "%d", &pid) == 1) { kill(pid, SIGKILL); }
                std::fclose(list);
            }
            while (waitpid(-1, &status, 0) > 0 || errno == EINTR) {}
        }
    }
    const bool procFallback = CheckProcFallback();
    std::printf("ASSERT_PROC_FALLBACK %s\n", procFallback ? "PASS" : "FAIL");
    failed += !procFallback;
    return failed == 0 ? 0 : 1;
}
