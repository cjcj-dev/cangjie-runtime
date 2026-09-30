#include "Cangjie.h"
#include "SignalManager.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

extern "C" void CJ_MCC_AddSignalHandler(int, SignalAction*);

std::atomic<bool> shutdownCallbackEntered{false};
std::atomic<bool> shutdownCallbackReturned{false};
std::atomic<bool> shutdownWorkerEntered{false};
std::mutex shutdownMutex;
std::condition_variable shutdownCondition;
bool shutdownReleaseCallback = false;
bool shutdownReleaseWorker = false;
bool shutdownHoldCallback = true;
int shutdownExitSignal = _NSIG;

extern "C" __attribute__((noinline)) void ShutdownCheckpoint(int result)
{
    std::fprintf(stderr, "SHUTDOWN_CHECKPOINT fini_rc=%d entered=%d returned=%d\n", result,
                 shutdownCallbackEntered.load(), shutdownCallbackReturned.load());
}

static bool ShutdownCallback(int, siginfo_t*, void*)
{
    shutdownCallbackEntered.store(true, std::memory_order_release);
    shutdownCondition.notify_all();
    if (shutdownHoldCallback) {
        std::unique_lock<std::mutex> lock(shutdownMutex);
        shutdownCondition.wait(lock, [] { return shutdownReleaseCallback; });
    }
    shutdownCallbackReturned.store(true, std::memory_order_release);
    shutdownCondition.notify_all();
    return true;
}

static void* HoldProcessor(void*)
{
    shutdownWorkerEntered.store(true, std::memory_order_release);
    shutdownCondition.notify_all();
    std::unique_lock<std::mutex> lock(shutdownMutex);
    shutdownCondition.wait(lock, [] { return shutdownReleaseWorker; });
    return nullptr;
}

static bool WaitFor(const std::atomic<bool>& flag)
{
    std::unique_lock<std::mutex> lock(shutdownMutex);
    return shutdownCondition.wait_for(lock, std::chrono::seconds(10), [&] {
        return flag.load(std::memory_order_acquire);
    });
}

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const bool singleProcessor = std::strcmp(argv[1], "single-p") == 0;
    shutdownHoldCallback = !singleProcessor;
    RuntimeParam parameters{};
    parameters.heapParam.heapSize = 64 * 1024;
    parameters.coParam.processorNum = 1;
    if (InitCJRuntime(&parameters) != E_OK) { return 3; }
    SignalAction action{};
    action.saSignalAction = ShutdownCallback;
    action.scFlags = SA_SIGINFO;
    sigemptyset(&action.scMask);
    CJ_MCC_AddSignalHandler(SIGUSR1, &action);
    CJThreadHandle worker = nullptr;
    if (singleProcessor) {
        worker = RunCJTask(HoldProcessor, nullptr);
        if (worker == nullptr || !WaitFor(shutdownWorkerEntered)) { return 4; }
    }
    if (kill(getpid(), SIGUSR1) != 0 || !WaitFor(shutdownCallbackEntered)) { return 5; }
    if (singleProcessor) {
        const bool delivered = WaitFor(shutdownCallbackReturned);
        std::fprintf(stderr, "SHUTDOWN_SINGLE_P_TARGET delivered=%d worker_held=1\n", delivered);
        {
            std::lock_guard<std::mutex> lock(shutdownMutex);
            shutdownReleaseWorker = true;
        }
        shutdownCondition.notify_all();
        void* result = nullptr;
        GetTaskRet(worker, &result);
        ReleaseHandle(worker);
        if (!delivered) { return 6; }
    }
    const int result = FiniCJRuntime();
    ShutdownCheckpoint(result);
    const bool inFlight = !shutdownCallbackReturned.load(std::memory_order_acquire);
    std::fprintf(stderr, "SHUTDOWN_RETURN_TARGET fini_rc=%d callback_in_flight=%d expected=%d\n",
                 result, inFlight, !singleProcessor);
    {
        std::lock_guard<std::mutex> lock(shutdownMutex);
        shutdownReleaseCallback = true;
    }
    shutdownCondition.notify_all();
    const bool returned = WaitFor(shutdownCallbackReturned);
    std::fprintf(stderr, "SHUTDOWN_NATIVE_RETURN_TARGET returned=%d\n", returned);
    return result == E_OK && returned && inFlight == !singleProcessor ? 0 : 7;
}
