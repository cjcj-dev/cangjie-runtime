// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Minimal GC unit-test harness (HotSpot gtest shape, no third-party dep).
// Offline-friendly: no FetchContent / no apt.

#ifndef MRT_GC_UNITTEST_HPP
#define MRT_GC_UNITTEST_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace MapleRuntime {
namespace GcUnit {

struct TestCase {
    const char* suite;
    const char* name;
    void (*fn)();
    bool otherVm;
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)(), bool otherVm = false)
    {
        Registry().push_back(TestCase{ suite, name, fn, otherVm });
    }
};

struct AssertFailure : std::exception {
    explicit AssertFailure(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
    std::string msg;
};

// A throwing assertion must never turn its real failure into
// std::terminate merely because a test-local thread has not reached its
// explicit join yet. Tests still join at the point that establishes their
// synchronization contract; this guard owns the exceptional exit path.
class JoinGuard {
public:
    explicit JoinGuard(std::thread& thread) : thread(thread) {}
    ~JoinGuard()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }

    JoinGuard(const JoinGuard&) = delete;
    JoinGuard& operator=(const JoinGuard&) = delete;

private:
    std::thread& thread;
};

inline void Fail(const char* file, int line, const char* expr)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%d: EXPECT failed: %s", file, line, expr);
    throw AssertFailure(buf);
}

#define GC_EXPECT_TRUE(cond)                                                                                           \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ::MapleRuntime::GcUnit::Fail(__FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

#define GC_EXPECT_FALSE(cond) GC_EXPECT_TRUE(!(cond))

#define GC_EXPECT_EQ(a, b)                                                                                             \
    do {                                                                                                               \
        auto _ga = (a);                                                                                                \
        auto _gb = (b);                                                                                                \
        if (!(_ga == _gb)) {                                                                                           \
            char _buf[640];                                                                                            \
            std::snprintf(_buf, sizeof(_buf), "%s:%d: EXPECT_EQ failed: %s (==%llu) vs %s (==%llu)", __FILE__,          \
                          __LINE__, #a, static_cast<unsigned long long>(_ga), #b,                                      \
                          static_cast<unsigned long long>(_gb));                                                       \
            throw ::MapleRuntime::GcUnit::AssertFailure(_buf);                                                         \
        }                                                                                                              \
    } while (0)

#define GC_EXPECT_NE(a, b)                                                                                             \
    do {                                                                                                               \
        auto _ga = (a);                                                                                                \
        auto _gb = (b);                                                                                                \
        if (_ga == _gb) {                                                                                              \
            char _buf[640];                                                                                            \
            std::snprintf(_buf, sizeof(_buf), "%s:%d: EXPECT_NE failed: %s and %s both %llu", __FILE__, __LINE__, #a,  \
                          #b, static_cast<unsigned long long>(_ga));                                                   \
            throw ::MapleRuntime::GcUnit::AssertFailure(_buf);                                                         \
        }                                                                                                              \
    } while (0)

#define GC_TEST(suite, name)                                                                                           \
    static void suite##_##name();                                                                                      \
    static ::MapleRuntime::GcUnit::Registrar suite##_##name##_reg(#suite, #name, &suite##_##name);                     \
    static void suite##_##name()

// HotSpot's TEST_OTHER_VM invariant: the test body runs in a newly exec'd
// process, and success requires both exit(0) and a completion sentinel.  fork
// alone is insufficient when the parent has already initialized the runtime.
#define GC_OTHER_VM_TEST(suite, name)                                                                                  \
    static void suite##_##name();                                                                                      \
    static ::MapleRuntime::GcUnit::Registrar suite##_##name##_reg(#suite, #name, &suite##_##name, true);               \
    static void suite##_##name()

inline void RunInOtherVm(const std::string& fullName)
{
#if defined(__linux__)
    int childStderr[2];
    if (pipe(childStderr) != 0) {
        throw AssertFailure("other-vm pipe failed for " + fullName + ": " + std::strerror(errno));
    }

    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        close(childStderr[0]);
        if (dup2(childStderr[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(childStderr[1]);
        (void)setenv("GC_UNIT_OTHER_VM_CHILD", fullName.c_str(), 1);
        (void)unsetenv("GC_UNIT_FILTER");
        (void)unsetenv("GC_UNIT_TALLY_FILE");
        const std::string filterArg = "--gtest_filter=" + fullName;
        execl("/proc/self/exe", "cj_gc_unit", filterArg.c_str(), static_cast<char*>(nullptr));
        std::fprintf(stderr, "[  ERROR ] exec /proc/self/exe failed: %s\n", std::strerror(errno));
        _exit(127);
    }

    close(childStderr[1]);
    if (child < 0) {
        const int savedErrno = errno;
        close(childStderr[0]);
        throw AssertFailure("other-vm fork failed for " + fullName + ": " + std::strerror(savedErrno));
    }

    std::string transcript;
    char buffer[1024];
    bool readOk = true;
    for (;;) {
        const ssize_t count = read(childStderr[0], buffer, sizeof(buffer));
        if (count > 0) {
            transcript.append(buffer, static_cast<size_t>(count));
            (void)std::fwrite(buffer, 1, static_cast<size_t>(count), stderr);
            continue;
        }
        if (count == 0) {
            break;
        }
        if (errno != EINTR) {
            readOk = false;
            break;
        }
    }
    close(childStderr[0]);

    int status = 0;
    pid_t waitedPid;
    do {
        waitedPid = waitpid(child, &status, 0);
    } while (waitedPid < 0 && errno == EINTR);
    const bool waited = waitedPid == child;
    const std::string sentinel = "GC_UNIT_OTHER_VM_OKIDOKI " + fullName + "\n";
    const bool exitedCleanly = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!readOk || !exitedCleanly || transcript.find(sentinel) == std::string::npos) {
        throw AssertFailure("other-vm child did not exit cleanly with sentinel for " + fullName);
    }
#else
    throw AssertFailure("other-vm tests require Linux exec isolation: " + fullName);
#endif
}

inline int RunAll()
{
    int failed = 0;
    int passed = 0;
    // Exact-name filtering gives a caller one selected test and output. A typo
    // must not turn that invocation into a vacuous green run.
    const char* filter = std::getenv("GC_UNIT_FILTER");
    const char* otherVmChild = std::getenv("GC_UNIT_OTHER_VM_CHILD");
    if (otherVmChild != nullptr && (filter == nullptr || std::strcmp(filter, otherVmChild) != 0)) {
        std::fprintf(stderr, "[  ERROR ] invalid other-vm child selection: child=%s filter=%s\n",
                     otherVmChild, filter == nullptr ? "(null)" : filter);
        return 1;
    }
    bool selectedOtherVmChild = false;
    for (const auto& t : Registry()) {
        const std::string fullName = std::string(t.suite) + "." + t.name;
        if (filter != nullptr) {
            if (fullName != filter) {
                continue;
            }
        }
        const bool directOtherVmChild = otherVmChild != nullptr && fullName == otherVmChild && t.otherVm;
        selectedOtherVmChild = selectedOtherVmChild || directOtherVmChild;
        try {
            if (t.otherVm && !directOtherVmChild) {
                RunInOtherVm(fullName);
            } else {
                t.fn();
            }
            if (otherVmChild == nullptr) {
                std::printf("[  PASS  ] %s.%s\n", t.suite, t.name);
            }
            ++passed;
        } catch (const AssertFailure& e) {
            std::fprintf(otherVmChild == nullptr ? stdout : stderr,
                         "[  FAIL  ] %s.%s\n  %s\n", t.suite, t.name, e.what());
            ++failed;
        } catch (const std::exception& e) {
            std::fprintf(otherVmChild == nullptr ? stdout : stderr,
                         "[  FAIL  ] %s.%s\n  exception: %s\n", t.suite, t.name, e.what());
            ++failed;
        } catch (...) {
            std::fprintf(otherVmChild == nullptr ? stdout : stderr,
                         "[  FAIL  ] %s.%s\n  unknown exception\n", t.suite, t.name);
            ++failed;
        }
    }
    const int tests = passed + failed;
    if (otherVmChild == nullptr) {
        std::printf("[========] %d tests: %d passed, %d failed\n", tests, passed, failed);
    }
    if (filter != nullptr && tests == 0) {
        std::fprintf(stderr, "[  ERROR ] GC_UNIT_FILTER matched no test: %s\n", filter);
    }
    if (otherVmChild != nullptr &&
        (filter == nullptr || std::strcmp(filter, otherVmChild) != 0 || tests != 1 || !selectedOtherVmChild)) {
        std::fprintf(stderr, "[  ERROR ] invalid other-vm child selection: child=%s filter=%s tests=%d\n",
                     otherVmChild, filter == nullptr ? "(null)" : filter, tests);
        return 1;
    }

    // The runtime has atexit diagnostics on stderr (for example ZForwardingLife's summary).  A
    // caller that merges stdout and stderr can therefore receive a tally split by diagnostics when
    // stdio flushes its buffered stdout at process exit.  Duplicate the completion evidence into a
    // caller-provided file and close it before returning from main, so no atexit output can share
    // that channel.  Direct invocations remain unchanged when the variable is absent.
    bool tallyWritten = true;
    if (otherVmChild == nullptr) {
        if (const char* tallyPath = std::getenv("GC_UNIT_TALLY_FILE")) {
            FILE* tally = std::fopen(tallyPath, "w");
            if (tally == nullptr) {
                std::fprintf(stderr, "[  ERROR ] cannot open GC_UNIT_TALLY_FILE=%s\n", tallyPath);
                tallyWritten = false;
            } else {
                tallyWritten =
                    std::fprintf(tally, "[========] %d tests: %d passed, %d failed\n", tests, passed, failed) >= 0;
                tallyWritten = std::fclose(tally) == 0 && tallyWritten;
                if (!tallyWritten) {
                    std::fprintf(stderr, "[  ERROR ] cannot write GC_UNIT_TALLY_FILE=%s\n", tallyPath);
                }
            }
        }
    }
    if (otherVmChild != nullptr && failed == 0) {
        std::fprintf(stderr, "GC_UNIT_OTHER_VM_OKIDOKI %s\n", otherVmChild);
        std::fflush(stderr);
    }
    return failed == 0 && tallyWritten && (filter == nullptr || tests != 0) ? 0 : 1;
}

} // namespace GcUnit
} // namespace MapleRuntime

#endif // MRT_GC_UNITTEST_HPP
