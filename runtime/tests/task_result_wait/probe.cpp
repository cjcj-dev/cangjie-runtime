#include "Cangjie.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <pthread.h>
#include <system_error>
#include <thread>
#include <unistd.h>

extern "C" int CJ_SyscallRead(int, void*, size_t);
extern "C" void CJ_SyscallEnter();
extern "C" void CJ_SyscallExit();

// Fault injection is confined to the calling thread and one condvar wait.
// Other runtime waits always call libc. No product check is bypassed.
static thread_local bool injectWaitError = false;
static thread_local bool waitErrorReached = false;
extern "C" int pthread_cond_wait(pthread_cond_t* cv, pthread_mutex_t* mutex)
{
    if (injectWaitError) {
        injectWaitError = false;
        waitErrorReached = true;
        return EINVAL;
    }
    static auto realWait = reinterpret_cast<int (*)(pthread_cond_t*, pthread_mutex_t*)>(
        dlsym(RTLD_NEXT, "pthread_cond_wait"));
    if (realWait == nullptr) { std::terminate(); }
    return realWait(cv, mutex);
}

static void* Child(void* data)
{
    const int fd = *static_cast<int*>(data);
    char value = 0;
    return reinterpret_cast<void*>(CJ_SyscallRead(fd, &value, 1) == 1 && value == 'x' ? 42 : 43);
}

extern "C" int CheckTaskResultWait(int mode)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) { return 10; }
    auto task = RunCJTask(Child, &pipefd[0]);
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
