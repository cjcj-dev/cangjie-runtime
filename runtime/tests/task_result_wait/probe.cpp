#include "Cangjie.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

extern "C" int CJ_SyscallRead(int, void*, size_t);
extern "C" void CJ_SyscallEnter();
extern "C" void CJ_SyscallExit();

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
    int expected = E_TIMEOUT;
    if (mode == 0) {
        status = GetTaskRetWithTimeout(task, &result, 1);
    } else if (mode == 1) {
        CJ_SyscallEnter();
        status = GetTaskRetWithTimeout(task, &result, 1);
        CJ_SyscallExit();
    } else if (mode == 2) {
        status = GetTaskRetWithTimeout(nullptr, &result, 1);
        expected = E_ARGS;
    } else {
        // A native caller must not acquire or release a carrier transition.
        std::thread native([&] { status = GetTaskRetWithTimeout(task, &result, 1); });
        native.join();
    }
    const bool pathPassed = status == expected;
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
    std::fprintf(stderr, "TASK_WAIT_PATH mode=%d first=%d complete=%d fast=%d invalid=%d\n",
        mode, pathPassed, completed, fast, invalid);
    return pathPassed && completed && fast && invalid ? 0 : 12;
}
