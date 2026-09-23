// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Standalone regression runner for the real other-vm harness (no runtime SO).
#include "gc_unittest.hpp"

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
    return failed == 0 ? 0 : 1;
}
