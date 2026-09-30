#include "gc_unittest.hpp"
#if defined(__linux__)
#include <chrono>
#include <cstddef>
#include <thread>
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zDriver.hpp"
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"
#include "Loader/CjFileLoader/CjFileLoader.h"
#include "LoaderManager.h"

using namespace MapleRuntime;
extern "C" void MRT_LibraryOnLoad(uint64_t address, bool enableGC);
extern "C" void CJ_MRT_ForceFullGC();

namespace {
struct RequestMetadata {
    CJFileHeader header {};
    CJGCFlagsTable flags {1, 1, 0};
    const char* version = "0.0.1";
};
RequestMetadata requestMetadata;

void LoadRequestImage(bool enable, bool early)
{
    requestMetadata.header.cJFileSize = sizeof(requestMetadata);
    requestMetadata.header.cJSDKVersionOffset = offsetof(RequestMetadata, version);
    requestMetadata.header.tables[GC_FLAGS_TABLE] = {
        offsetof(RequestMetadata, flags), sizeof(requestMetadata.flags)};
    std::fprintf(stderr, "GLOBAL_GC_LOAD early=%d enable=%d\n", early, enable);
    MRT_LibraryOnLoad(reinterpret_cast<uint64_t>(&requestMetadata), enable);
}

void InitRequests(const char* config, bool timer)
{
    setenv("cjEnableGC", config, 1);
    setenv("cjUncommit", "0", 1);
    RuntimeParam params {};
    params.heapParam.heapSize = 32 * 1024;
    params.coParam.processorNum = 1;
    params.gcParam.backupGCInterval = timer ? 1 : 0;
    GC_EXPECT_EQ(InitCJRuntime(&params), E_OK);
}

void ExplicitRequest(const char* config, bool enable, bool early)
{
    setenv("cjEnableGC", config, 1);
    if (early) LoadRequestImage(enable, true);
    InitRequests(config, false);
    if (!early) LoadRequestImage(enable, false);
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    const auto* image = loader->GetBaseFileByMetaAddr(reinterpret_cast<Uptr>(&requestMetadata));
    const bool registered = image != nullptr && image->IsRegistered();
    const uint32_t before = Heap::GetHeap().old().seqnum();
    CJ_MRT_ForceFullGC();
    const uint32_t after = Heap::GetHeap().old().seqnum();
    std::fprintf(stderr, "GLOBAL_GC_EXPLICIT_TARGET executed=1 config=%s enable=%d early=%d registered=%d before=%u after=%u\n",
                 config, enable, early, registered, before, after);
    GC_EXPECT_TRUE(registered && after > before);
}

void TimerRequest(const char* config)
{
    InitRequests(config, true);
    LoadRequestImage(false, false);
    const uint32_t before = Heap::GetHeap().old().seqnum();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((Heap::GetHeap().old().seqnum() == before || ZDriver::major()->is_busy()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const uint32_t after = Heap::GetHeap().old().seqnum();
    const bool completed = !ZDriver::major()->is_busy();
    std::fprintf(stderr, "GLOBAL_GC_DIRECTOR_TARGET executed=1 config=%s before=%u after=%u completed=%d\n",
                 config, before, after, completed);
    GC_EXPECT_TRUE(after > before && completed);
}

void RejectMetadata(const char* config, bool missingBarrier)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) _exit(126);
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        requestMetadata.flags = missingBarrier ? CJGCFlagsTable{1, 0, 0} : CJGCFlagsTable{0, 1, 0};
        setenv("cjEnableGC", config, 1);
        LoadRequestImage(false, true);
        InitRequests(config, false);
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) transcript.append(buffer, count);
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    const bool rejected = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
        transcript.find("no safepoint or barrier defined in file") != std::string::npos;
    std::fprintf(stderr, "GLOBAL_GC_METADATA_TARGET executed=1 config=%s barrier=%d status=%d rejected=%d\n%s",
                 config, missingBarrier, status, rejected, transcript.c_str());
    GC_EXPECT_TRUE(rejected);
}
}

GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, ZeroLateFalse) { ExplicitRequest("0", false, false); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, OneLateFalse) { ExplicitRequest("1", false, false); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, ZeroEarlyFalse) { ExplicitRequest("0", false, true); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, OneEarlyTrue) { ExplicitRequest("1", true, true); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, ZeroLateTrue) { ExplicitRequest("0", true, false); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, OneLateTrue) { ExplicitRequest("1", true, false); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, ZeroDirector) { TimerRequest("0"); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, OneDirector) { TimerRequest("1"); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, ZeroMissingBarrier) { RejectMetadata("0", true); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, OneMissingBarrier) { RejectMetadata("1", true); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, ZeroMissingSafepoint) { RejectMetadata("0", false); }
GC_RUNTIME_OTHER_VM_TEST(GlobalGCRequests, OneMissingSafepoint) { RejectMetadata("1", false); }
#endif
