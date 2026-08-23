// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Minimal GC unit-test harness (HotSpot gtest shape, no third-party dep).
// Offline-friendly: no FetchContent / no apt. <200 lines by design.

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

namespace MapleRuntime {
namespace GcUnit {

struct TestCase {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)())
    {
        Registry().push_back(TestCase{ suite, name, fn });
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

inline int RunAll()
{
    int failed = 0;
    int passed = 0;
    for (const auto& t : Registry()) {
        try {
            t.fn();
            std::printf("[  PASS  ] %s.%s\n", t.suite, t.name);
            ++passed;
        } catch (const AssertFailure& e) {
            std::printf("[  FAIL  ] %s.%s\n  %s\n", t.suite, t.name, e.what());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("[  FAIL  ] %s.%s\n  exception: %s\n", t.suite, t.name, e.what());
            ++failed;
        } catch (...) {
            std::printf("[  FAIL  ] %s.%s\n  unknown exception\n", t.suite, t.name);
            ++failed;
        }
    }
    const int tests = passed + failed;
    std::printf("[========] %d tests: %d passed, %d failed\n", tests, passed, failed);

    // The runtime has atexit diagnostics on stderr (for example ZForwardingLife's summary).  A
    // caller that merges stdout and stderr can therefore receive a tally split by diagnostics when
    // stdio flushes its buffered stdout at process exit.  Duplicate the completion evidence into a
    // caller-provided file and close it before returning from main, so no atexit output can share
    // that channel.  Direct invocations remain unchanged when the variable is absent.
    bool tallyWritten = true;
    if (const char* tallyPath = std::getenv("GC_UNIT_TALLY_FILE")) {
        FILE* tally = std::fopen(tallyPath, "w");
        if (tally == nullptr) {
            std::fprintf(stderr, "[  ERROR ] cannot open GC_UNIT_TALLY_FILE=%s\n", tallyPath);
            tallyWritten = false;
        } else {
            tallyWritten = std::fprintf(tally, "[========] %d tests: %d passed, %d failed\n", tests, passed, failed) >=
                0;
            tallyWritten = std::fclose(tally) == 0 && tallyWritten;
            if (!tallyWritten) {
                std::fprintf(stderr, "[  ERROR ] cannot write GC_UNIT_TALLY_FILE=%s\n", tallyPath);
            }
        }
    }
    return failed == 0 && tallyWritten ? 0 : 1;
}

} // namespace GcUnit
} // namespace MapleRuntime

#endif // MRT_GC_UNITTEST_HPP
