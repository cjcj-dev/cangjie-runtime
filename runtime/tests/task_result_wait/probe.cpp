#include "Cangjie.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <condition_variable>
#include <system_error>
#include <thread>
#include <unistd.h>

extern "C" int CJ_SyscallRead(int, void*, size_t);
extern "C" void CJ_SyscallEnter();
extern "C" void CJ_SyscallExit();

// Fault injection is confined to this thread's next C++ condition-variable
// wait. Other waits forward to the exact version consumed by this runtime.
// Resolve before worker startup and avoid lazy dynamic TLS in the interposer.
static thread_local __attribute__((tls_model("initial-exec"))) bool injectWaitError = false;
static thread_local __attribute__((tls_model("initial-exec"))) bool waitErrorReached = false;
using WaitFunction = void (*)(std::condition_variable*, std::unique_lock<std::mutex>&);
static WaitFunction realWait = nullptr;
__attribute__((constructor)) static void ResolveWait()
{
    realWait = reinterpret_cast<WaitFunction>(dlvsym(RTLD_NEXT,
        "_ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE", "GLIBCXX_3.4.30"));
    if (realWait == nullptr) { std::terminate(); }
    std::fprintf(stderr, "TASK_WAIT_INTERPOSER_READY version=GLIBCXX_3.4.30\n");
}

void std::condition_variable::wait(std::unique_lock<std::mutex>& lock)
{
    if (injectWaitError) {
        injectWaitError = false;
        waitErrorReached = true;
        // Inject at the throwing C++ boundary. Returning EINVAL from pthread
        // wait is only asserted by release libstdc++ and does not throw.
        std::fprintf(stderr, "TASK_WAIT_INJECTED_EXCEPTION\n");
        throw std::system_error(EINVAL, std::generic_category(), "task wait injection");
    }
    if (realWait == nullptr) { std::terminate(); }
    realWait(this, lock);
}

static void* Child(void* data)
{
    std::fprintf(stderr, "TASK_WAIT_CHILD_ENTER\n");
    const int fd = *static_cast<int*>(data);
    char value = 0;
    return reinterpret_cast<void*>(CJ_SyscallRead(fd, &value, 1) == 1 && value == 'x' ? 42 : 43);
}

extern "C" int CheckTaskResultWait(int mode)
{
    std::fprintf(stderr, "TASK_WAIT_ENTER mode=%d\n", mode);
    int pipefd[2];
    if (pipe(pipefd) != 0) { return 10; }
    auto task = RunCJTask(Child, &pipefd[0]);
    std::fprintf(stderr, "TASK_WAIT_SUBMITTED mode=%d task=%p\n", mode, task);
    if (task == nullptr) { return 11; }
    void* result = nullptr;
    int status = -1;
    bool exceptionObserved = false;
    if (mode == 0) {
        status = GetTaskRetWithTimeout(task, &result, 1);
    } else if (mode == 1) {
        CJ_SyscallEnter();
        status = GetTaskRetWithTimeout(task, &result, 1);
        CJ_SyscallExit();
    } else if (mode == 2) {
        injectWaitError = true;
        try {
            (void)GetTaskRet(task, &result);
        } catch (const std::system_error& error) {
            exceptionObserved = error.code().value() == EINVAL;
        }
        injectWaitError = false;
        status = exceptionObserved && waitErrorReached ? E_TIMEOUT : -1;
    } else {
        // A native caller must not acquire or release a carrier transition.
        std::thread native([&] { status = GetTaskRetWithTimeout(task, &result, 1); });
        native.join();
    }
    const bool pathPassed = status == E_TIMEOUT;
    std::fprintf(stderr, "TASK_WAIT_FIRST_RETURN mode=%d status=%d\n", mode, status);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (write(pipefd[1], "x", 1) != 1) { std::terminate(); }
    });
    status = GetTaskRet(task, &result);
    release.join();
    const bool completed = status == E_OK && result == reinterpret_cast<void*>(42);
    const bool fast = GetTaskRet(task, &result) == E_OK && result == reinterpret_cast<void*>(42);
    const bool invalid = GetTaskRet(nullptr, &result) == E_ARGS;
    ReleaseHandle(task);
    close(pipefd[0]);
    close(pipefd[1]);
    std::fprintf(stderr, "TASK_WAIT_PATH mode=%d first=%d complete=%d fast=%d invalid=%d exception=%d\n",
        mode, pathPassed, completed, fast, invalid, exceptionObserved);
    return pathPassed && completed && fast && invalid ? 0 : 12;
}
