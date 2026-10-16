#include "Cangjie.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

extern "C" int CJ_SyscallRead(int descriptor, void* buffer, size_t count);
std::atomic<bool> blockingStarted{false};
std::atomic<bool> blockingCompleted{false};
int pipeDescriptors[2];

extern "C" void* OrdinaryBlockingTask(void*)
{
    char value = 0;
    blockingStarted.store(true);
    const int result = CJ_SyscallRead(pipeDescriptors[0], &value, 1);
    blockingCompleted.store(result == 1 && value == 'x');
    return reinterpret_cast<void*>(result == 1 && value == 'x' ? 0 : 1);
}

extern "C" void* OrdinaryHelperTask(void*)
{
    return nullptr;
}

extern "C" __attribute__((noinline)) void ReleasePipe()
{
    if (write(pipeDescriptors[1], "x", 1) != 1) {
        std::_Exit(13);
    }
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "asleep";
    RuntimeParam parameters{};
    parameters.heapParam.heapSize = 512 * 1024;
    parameters.coParam.processorNum = 1;
    if (pipe(pipeDescriptors) != 0 || InitCJRuntime(&parameters) != E_OK) {
        return 10;
    }
    if (std::strcmp(mode, "ordinary") == 0) {
        auto task = RunCJTask(OrdinaryHelperTask, nullptr);
        void* result = nullptr;
        if (task == nullptr || GetTaskRet(task, &result) != E_OK || result != nullptr) {
            return 11;
        }
    } else {
        const bool fast = std::strcmp(mode, "fast") == 0;
        if (fast) {
            ReleasePipe();
        }
        auto blocking = RunCJTask(OrdinaryBlockingTask, nullptr);
        if (blocking == nullptr) {
            return 11;
        }
        while (!blockingStarted.load()) {
            std::this_thread::yield();
        }
        if (!fast) {
            auto helper = RunCJTask(OrdinaryHelperTask, nullptr);
            void* helperResult = nullptr;
            if (helper == nullptr || GetTaskRet(helper, &helperResult) != E_OK) {
                return 12;
            }
            ReleasePipe();
        }
        void* blockingResult = nullptr;
        if (GetTaskRet(blocking, &blockingResult) != E_OK || blockingResult != nullptr ||
            !blockingCompleted.load()) {
            std::fprintf(stderr, "ASSERT ordinary_task_completion_after_pipe_release FAIL result\n");
            std::_Exit(14);
        }
    }
    std::fprintf(stderr, "ASSERT ordinary_task_completion_after_pipe_release PASS mode=%s\n", mode);
    std::fflush(stderr);
    std::_Exit(0);
}
