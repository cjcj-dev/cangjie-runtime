#include "Cangjie.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "SignalManager.h"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <sys/stat.h>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

extern "C" void CJ_MCC_AddSignalHandler(int, SignalAction*);

std::atomic<bool> shutdownCallbackEntered{false};
std::atomic<bool> shutdownCallbackReturned{false};
std::atomic<int> shutdownCallbackCount{0};
std::atomic<bool> shutdownWorkerEntered{false};
std::mutex shutdownMutex;
std::condition_variable shutdownCondition;
bool shutdownReleaseCallback = false;
bool shutdownReleaseWorker = false;
bool shutdownHoldCallback = true;
int shutdownExitSignal = _NSIG;
using ShutdownManagedCallback = bool (*)(int, siginfo_t*, void*);
ShutdownManagedCallback shutdownManagedCallback = nullptr;
ShutdownManagedCallback shutdownManagedGcCallback = nullptr;
CJTaskFunc shutdownManagedDumpCaller = nullptr;
std::atomic<int> shutdownDumpSignalResult{-1};
std::atomic<int> shutdownManagedStage{0};
std::atomic<bool> shutdownNativeReturning{false};
std::atomic<long long> shutdownManagedGcDone{0};
std::atomic<long long> shutdownManagedBad{-1};
int shutdownObservationDone = 0;

extern "C" void ShutdownInstallCallback(ShutdownManagedCallback callback)
{
    shutdownManagedCallback = callback;
}

extern "C" void ShutdownInstallGcCallback(ShutdownManagedCallback callback)
{
    shutdownManagedGcCallback = callback;
}

extern "C" void ShutdownInstallDumpCaller(CJTaskFunc caller)
{
    shutdownManagedDumpCaller = caller;
}

extern "C" int ShutdownRaiseDumpSignal()
{
    const int result = kill(getpid(), SIGUSR1);
    shutdownDumpSignalResult.store(result, std::memory_order_release);
    std::fprintf(stderr, "SHUTDOWN_MANAGED_DUMP_SIGNAL sent=%d\n", result == 0);
    return result;
}

extern "C" long long ShutdownManagedAwaitGc()
{
    return shutdownManagedGcDone.load(std::memory_order_acquire);
}

extern "C" void ShutdownManagedReport(long long bad)
{
    shutdownManagedBad.store(bad, std::memory_order_release);
    std::fprintf(stderr, "SHUTDOWN_MANAGED_REPORT bad=%lld\n", bad);
}

extern "C" void ShutdownManagedProgress(int stage)
{
    shutdownManagedStage.store(stage, std::memory_order_release);
    std::fprintf(stderr, "SHUTDOWN_MANAGED_PROGRESS stage=%d\n", stage);
}

extern "C" void ShutdownNativeBlock()
{
    shutdownCallbackEntered.store(true, std::memory_order_release);
    shutdownCondition.notify_all();
    std::unique_lock<std::mutex> lock(shutdownMutex);
    shutdownCondition.wait(lock, [] { return shutdownReleaseCallback; });
    shutdownNativeReturning.store(true, std::memory_order_release);
    std::fprintf(stderr, "SHUTDOWN_NATIVE_RETURNING\n");
}

extern "C" __attribute__((noinline)) void ShutdownCheckpoint(int result)
{
    std::fprintf(stderr, "SHUTDOWN_CHECKPOINT fini_rc=%d entered=%d returned=%d\n", result,
                 shutdownCallbackEntered.load(), shutdownCallbackReturned.load());
}

static bool ShutdownCallback(int, siginfo_t*, void*)
{
    const int count = shutdownCallbackCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (count != 1) {
        std::fprintf(stderr, "SHUTDOWN_REENTRY_BODY count=%d\n", count);
        return true;
    }
    shutdownCallbackEntered.store(true, std::memory_order_release);
    shutdownCondition.notify_all();
    if (shutdownHoldCallback) {
        // Preserve the f0f fixture's called-side blocked state. This scope only
        // publishes SAFE; it does not create a C2N RISKY/native transition.
        // Keep this arm distinct from the real managed @C -> ShutdownNativeBlock
        // arm when qualifying the scheduler's native-retention predicate.
        MapleRuntime::ScopedEnterSaferegion blockedInVM(false);
        std::unique_lock<std::mutex> lock(shutdownMutex);
        shutdownCondition.wait(lock, [] { return shutdownReleaseCallback; });
        // Publish the callback body return while still blocked-in-VM. The
        // saferegion destructor must then encounter the final VM barrier.
        // This is distinct from the managed arm's actual C2N return.
        shutdownCallbackReturned.store(true, std::memory_order_release);
        shutdownCondition.notify_all();
        return true;
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

// Managed handler, live managed frame, one real collection in the middle.
// os.cpp:434-441 calls the handler in the state the signal thread already has,
// so this frame is a normal mutator frame while the collection runs.
static int RunManagedGcArm()
{
    SignalAction action{};
    action.saSignalAction = shutdownManagedGcCallback;
    action.scFlags = SA_SIGINFO;
    sigemptyset(&action.scMask);
    CJ_MCC_AddSignalHandler(SIGUSR1, &action);
    if (kill(getpid(), SIGUSR1) != 0) { return 5; }
    std::fprintf(stderr, "SHUTDOWN_MANAGED_GC_SIGNAL_SENT\n");
    fflush(stderr);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (shutdownManagedStage.load(std::memory_order_acquire) < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool allocated = shutdownManagedStage.load(std::memory_order_acquire) >= 1;
    std::fprintf(stderr, "SHUTDOWN_MANAGED_GC_ALLOCATED allocated=%d\n", allocated);
    if (!allocated) { return 11; }
    // Real collection while the managed handler frame is live and runnable.
    const auto before = MapleRuntime::ZCollectedHeap::heap()->total_collections();
    CJ_MRT_ForceFullGC();
    const auto after = MapleRuntime::ZCollectedHeap::heap()->total_collections();
    std::fprintf(stderr, "SHUTDOWN_MANAGED_GC_COLLECTED before=%u after=%u\n", before, after);
    shutdownManagedGcDone.store(1, std::memory_order_release);
    while (shutdownManagedBad.load(std::memory_order_acquire) < 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const long long bad = shutdownManagedBad.load(std::memory_order_acquire);
    std::fprintf(stderr, "SHUTDOWN_MANAGED_GC_TARGET reported=%d bad=%lld expected=0\n", bad >= 0, bad);
    const int fini = FiniCJRuntime();
    std::fprintf(stderr, "SHUTDOWN_MANAGED_GC_FINI fini_rc=%d\n", fini);
    return (bad == 0 && after > before && fini == E_OK) ? 0 : 12;
}

// R1: a real compiled language task raises SIGUSR1. Keep the product's default
// handler, dispatcher and typed heap operation; no fixture handler replacement.
static int RunManagedDumpArm()
{
    struct stat information {};
    if (stat("item_data.dat", &information) == 0 || errno != ENOENT) { return 13; }
    auto task = RunCJTask(shutdownManagedDumpCaller, nullptr);
    if (task == nullptr) { return 14; }
    void* result = nullptr;
    if (GetTaskRet(task, &result) != E_OK) { return 15; }
    ReleaseHandle(task);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool dumped = false;
    do {
        dumped = stat("item_data.dat", &information) == 0 && information.st_size > 32;
        if (!dumped) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    } while (!dumped && std::chrono::steady_clock::now() < deadline);
    constexpr char header[] = "CANGJIE PROFILE 1.0.2";
    char actual[sizeof(header)] {};
    FILE* output = dumped ? std::fopen("item_data.dat", "rb") : nullptr;
    const bool valid = output != nullptr && std::fread(actual, sizeof(actual), 1, output) == 1 &&
                       std::memcmp(actual, header, sizeof(header)) == 0;
    if (output != nullptr) { std::fclose(output); }
    const int fini = FiniCJRuntime();
    const bool sent = shutdownDumpSignalResult.load(std::memory_order_acquire) == 0 &&
                      shutdownManagedStage.load(std::memory_order_acquire) == 41;
    std::fprintf(stderr, "SHUTDOWN_MANAGED_DUMP_TARGET executed=1 sent=%d dump=%d header=%d fini=%d\n",
                 sent, dumped, valid, fini);
    return sent && dumped && valid && fini == E_OK ? 0 : 16;
}

int main(int argc, char** argv)
{
    if (argc < 2) { return 2; }
    const bool singleProcessor = std::strcmp(argv[1], "single-p") == 0;
    const bool managed = std::strcmp(argv[1], "managed") == 0;
    const bool managedGc = std::strcmp(argv[1], "managed-gc") == 0;
    const bool managedDump = std::strcmp(argv[1], "managed-dump") == 0;
    const bool reentry = std::strcmp(argv[1], "reentry") == 0;
    shutdownHoldCallback = !singleProcessor;
    RuntimeParam parameters{};
    parameters.heapParam.heapSize = 64 * 1024;
    parameters.coParam.processorNum = 1;
    if (InitCJRuntime(&parameters) != E_OK) { return 3; }
    if ((managed || managedGc || managedDump) && (argc != 3 || LoadCJLibraryWithInit(argv[2]) != E_OK ||
                                   shutdownManagedCallback == nullptr || shutdownManagedGcCallback == nullptr ||
                                   shutdownManagedDumpCaller == nullptr)) {
        return 8;
    }
    if (managedGc) { return RunManagedGcArm(); }
    if (managedDump) { return RunManagedDumpArm(); }
    SignalAction action{};
    action.saSignalAction = managed ? shutdownManagedCallback : ShutdownCallback;
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
    if (reentry && kill(getpid(), SIGUSR1) != 0) { return 10; }
    const bool inFlight = !shutdownCallbackReturned.load(std::memory_order_acquire);
    std::fprintf(stderr, "SHUTDOWN_RETURN_TARGET fini_rc=%d callback_in_flight=%d expected=%d\n",
                 result, inFlight, !singleProcessor);
    {
        std::lock_guard<std::mutex> lock(shutdownMutex);
        shutdownReleaseCallback = true;
    }
    shutdownCondition.notify_all();
    if (managed) {
        std::unique_lock<std::mutex> lock(shutdownMutex);
        shutdownCondition.wait(lock, [] { return false; });
    }
    const bool returned = WaitFor(shutdownCallbackReturned);
    std::fprintf(stderr, "SHUTDOWN_NATIVE_RETURN_TARGET returned=%d\n", returned);
    if (std::getenv("SHUTDOWN_OBSERVE_EXIT") != nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (__atomic_load_n(&shutdownObservationDone, __ATOMIC_ACQUIRE) == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (!shutdownObservationDone) { return 9; }
    }
    return result == E_OK && returned && inFlight == !singleProcessor ? 0 : 7;
}
