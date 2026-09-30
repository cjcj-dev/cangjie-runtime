#include "gc_unittest.hpp"
#if defined(__linux__)
#include <chrono>
#include <cstddef>
#include <thread>
#include "Cangjie.h"
#include "Heap/z/zHeap.hpp"
#include "Loader/BinaryFile/CjFile/CjFileMeta.h"

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
    const uint32_t before = Heap::GetHeap().old().seqnum();
    CJ_MRT_ForceFullGC();
    const uint32_t after = Heap::GetHeap().old().seqnum();
    std::fprintf(stderr, "GLOBAL_GC_EXPLICIT_TARGET executed=1 config=%s enable=%d early=%d before=%u after=%u\n",
                 config, enable, early, before, after);
    GC_EXPECT_TRUE(after > before);
}

void TimerRequest(const char* config)
{
    InitRequests(config, true);
    LoadRequestImage(false, false);
    const uint32_t before = Heap::GetHeap().old().seqnum();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Heap::GetHeap().old().seqnum() == before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const uint32_t after = Heap::GetHeap().old().seqnum();
    std::fprintf(stderr, "GLOBAL_GC_DIRECTOR_TARGET executed=1 config=%s before=%u after=%u\n", config, before, after);
    GC_EXPECT_TRUE(after > before);
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
#endif
