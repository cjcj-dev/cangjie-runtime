// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// HotSpot safepoint.cpp:777-778 and codeBlob.cpp:224: managed frames have
// metadata; an empty root set is distinct from a missing map.
#include <cstdio>
#include <csignal>
#include <string>
#include "CangjieRuntime.h"
#include "Common/Runtime.h"
#include "Mutator/MutatorManager.h"
#include "StackManager.h"
#include "Exception/Exception.h"
#include "Exception/EhFrameInfo.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"
#include "UnwindStack/StackInfo.h"
#include "gc_unittest.hpp"
#include "managed_metadata_fixture.hpp"
#if defined(__linux__) || (defined(__APPLE__) && defined(__aarch64__))
#include <sys/wait.h>
#include <unistd.h>
#include <sys/syscall.h>

// CompilerCalls.h also defines alias bodies, so declare the existing C ABI.
extern "C" bool MCC_StartCpuProfiling();
extern "C" bool MCC_StopCpuProfiling(int fd);
using namespace MapleRuntime;
namespace {
// The profiler thread visits an empty manager; the stack/profile entry points
// and their serialized result are supplied by the linked product SO.
class ProfileRuntime final : public Runtime {
public:
    explicit ProfileRuntime(MutatorManager& manager) { mutatorManager = &manager; runtime = this; }
    ~ProfileRuntime() override { runtime = nullptr; }
    RuntimeParam GetRuntimeParam() const override { return RuntimeParam {}; }
    void SetGCThreshold(uint64_t) override {}
};
enum class Entry { ROOTS, HEAD, PROLOGUE, EH, RETURN, CALLER_SP, PROFILE, PROFILE_EMPTY };

void CheckMetadata(Entry entry, bool descriptorPresent, bool stackmapPresent, const char* message,
                   bool zeroRootRow = false, bool miss = false)
{
    int output[2];
    GC_EXPECT_EQ(pipe(output), 0);
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        close(output[0]);
        if (dup2(output[1], STDERR_FILENO) < 0) { _exit(126); }
        close(output[1]);
        signal(SIGABRT, SIG_DFL);
        ManagedMetadataFixture fixture;
        const uint32_t* code = fixture.PC(descriptorPresent, stackmapPresent, zeroRootRow);
        const Uptr map = zeroRootRow ? fixture.zeroMap : fixture.emptyMap;
        const Uptr pc = reinterpret_cast<Uptr>(code);
        fixture.Register();
        if (entry == Entry::ROOTS) {
            FrameAddress address {};
            FrameInfo frame(code);
            frame.SetFrameType(FrameType::MANAGED);
            frame.mFrame.SetFA(&address);
            frame.mFrame.SetIP(code);
            const std::vector<FrameInfo> recorded {frame};
            StackFrameStream frames(recorded);
            frames.Start();
            std::fprintf(stderr, "ROOTS_RECORDED_CONSUMER_TARGET returned=1 done=%d type=%d executed=1\n",
                         frames.IsDone(), int(frames.Current().GetFrameType()));
            _exit(!frames.IsDone() && frames.Current().GetFrameType() == FrameType::MANAGED ? 0 : 3);
        }
        if (entry == Entry::CALLER_SP) {
            std::fprintf(stderr, "METADATA_INPUT startPC=%p ip=%p\n", code, code + 1);
        }
        if (entry == Entry::HEAD) {
            const auto head = CompressedStackMapHead::GetStackMapHead(pc, nullptr, pc + 4);
            if (head.GetInvalidReason(pc, pc + 4) != StackMapInvalidReason::ZERO_ENTRIES) { _exit(3); }
        } else if (entry == Entry::PROLOGUE) {
            const FramePrologue prologue(stackmapPresent ? reinterpret_cast<Uptr*>(map) : nullptr);
            if (prologue.GetFrameSize() != 0 || prologue.GetSavedRegisterCount() != 0) { _exit(3); }
        } else if (entry == Entry::EH) {
            FrameInfo frame(code);
            frame.mFrame.SetIP(code + 1);
            FrameAddress address {};
            frame.mFrame.SetFA(&address);
            ExceptionWrapper exception;
            EHFrameInfo eh(frame, exception);
            CalleeSavedRegisterContext context {};
            eh.RestoreToCallerContext(context);
        } else if (entry == Entry::PROFILE || entry == Entry::PROFILE_EMPTY) {
#if defined(__x86_64__)
            struct { uintptr_t start; FrameAddress frame; FrameAddress anchor; } stack {};
            stack.start = pc + 9; // compiler frame ABI: startPC at fa[-1] minus 9
            stack.frame.callerFrameAddress = &stack.anchor;
            UnwindContext context;
            context.frameInfo.mFrame.SetFA(&stack.frame);
            context.frameInfo.mFrame.SetIP(code);
            context.anchorFA = reinterpret_cast<uint32_t*>(entry == Entry::PROFILE ? &stack.anchor : &stack.frame);
            MutatorManager manager;
            ProfileRuntime runtime(manager);
            const int fd = syscall(SYS_memfd_create, "metadata-profile", 0);
            if (fd < 0 || !MCC_StartCpuProfiling()) { _exit(4); }
            StackManager::PrintStackTraceForCpuProfile(&context, 1266);
            if (!MCC_StopCpuProfiling(fd)) { _exit(4); }
            char json[4096] {};
            const auto n = pread(fd, json, sizeof(json) - 1, 0);
            close(fd);
            std::fprintf(stderr, "PROFILE_RESULT bytes=%zd %s\n", n, json);
            // The public serializer must expose the posted empty sample, not
            // merely successful return from the producer or an untouched queue.
            if (n <= 0 || std::string(json).find("\"samples\":[3]") == std::string::npos) { _exit(3); }
#else
            _exit(125);
#endif
        } else if (entry == Entry::CALLER_SP) {
            FrameInfo frame(code);
            frame.SetFrameType(FrameType::MANAGED);
            frame.mFrame.SetIP(code + 1);
            FrameAddress address {};
            frame.mFrame.SetFA(&address);
            const auto actual = frame.CallerSP();
            const auto expected = descriptorPresent ? reinterpret_cast<Uptr>(&address) + sizeof(FrameAddress) : 0;
            std::fprintf(stderr, "CALLER_SP actual=%p expected=%p assertion-executed\n",
                         reinterpret_cast<void*>(actual), reinterpret_cast<void*>(expected));
            if (actual != expected) { _exit(3); }
        } else {
#if defined(__x86_64__)
            CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
            alignas(16) uintptr_t storage[64] {};
            auto* fp = &storage[56];
            fp[-10] = pc; // r10=startPC in the real return stub ABI
            fp[-11] = pc + (miss ? 4 : 0); // r11=sitePC
            MachineFrame machine;
            machine.SetFA(reinterpret_cast<FrameAddress*>(fp));
            machine.SetSP(reinterpret_cast<uintptr_t>(storage));
            FrameInfo frame(machine, FrameType::RETURN_SAFEPOINT);
            std::vector<StackFrameCursor::ReturnRegisterRoot> roots;
            StackFrameCursor::CollectReturnRegisterRoots(frame, roots);
            if (!roots.empty()) { _exit(3); }
#else
            _exit(125);
#endif
        }
        _exit(0);
    }
    close(output[1]);
    std::string transcript;
    char buffer[512];
    ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(output[0]);
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    bool target = message == nullptr ? status == 0 :
        WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && transcript.find(message) != std::string::npos;
    if (message != nullptr && entry != Entry::PROLOGUE) {
        target = target && transcript.find("startPC=") != std::string::npos && transcript.find("ip=") != std::string::npos;
    }
    std::fprintf(stderr, "METADATA_TARGET entry=%d desc=%d map=%d status=%d target=%d assertion-executed\n",
                 int(entry), descriptorPresent, stackmapPresent, status, target);
    GC_EXPECT_TRUE(target);
}
}
#if defined(__linux__)
GC_TEST(ManagedMetadata, TextDescriptorIsRegistered)
{
    ManagedMetadataFixture fixture;
    const uint32_t* pc = fixture.PC(true);
    fixture.RegisterText();
    ElfUnloadQuiescence::ReadScope reader;
    const auto result = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(pc));
    const bool code = ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<Uptr>(pc), true);
    const bool data = ElfUnloadQuiescence::IsLinkedAddress(
        fixture.Descriptor());
    const bool owned = result == reinterpret_cast<FuncDescRef>(fixture.Descriptor());
    std::fprintf(stderr, "METADATA_TEXT_PC_TARGET code=%d data=%d owned=%d executed=1\n",
                 code, data, owned);
    GC_EXPECT_TRUE(code && data && owned);
}

GC_TEST(ManagedMetadata, ExecutableWithoutDescriptorIsNative)
{
    ManagedMetadataFixture fixture;
    const uint32_t* pc = fixture.PC(false);
    fixture.Register();
    struct FrameInput {
        ArchUInt start;
        FrameAddress frame;
    } input {};
#if defined(__x86_64__)
    input.start = reinterpret_cast<ArchUInt>(pc) + 9;
#elif defined(__arm__)
    input.start = reinterpret_cast<ArchUInt>(pc) + 12;
#else
    input.start = reinterpret_cast<ArchUInt>(pc);
#endif
    UnwindContext context;
    context.frameInfo.mFrame.SetIP(pc + 1);
    context.frameInfo.mFrame.SetFA(&input.frame);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    StackFrameStream frames(&context);
    frames.Start();
    const bool native = !frames.IsDone() && frames.Current().GetFrameType() == FrameType::NATIVE;
    std::fprintf(stderr, "METADATA_EXECUTABLE_CLASSIFICATION_TARGET native=%d executed=1\n", native);
    GC_EXPECT_TRUE(native);
}

GC_TEST(ManagedMetadata, UnregisteredPCIsNative)
{
    const Uptr pc = 0x10000;
    struct Input { ArchUInt start; FrameAddress frame; } input {};
    UnwindContext context;
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(pc));
    context.frameInfo.mFrame.SetFA(&input.frame);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    StackFrameStream frames(&context);
    frames.Start();
    const bool registered = ElfUnloadQuiescence::IsLinkedAddress(pc);
    const bool native = !frames.IsDone() && frames.Current().GetFrameType() == FrameType::NATIVE;
    std::fprintf(stderr, "METADATA_UNREGISTERED_ROOTS_TARGET pc=%p registered=%d native=%d executed=1\n",
                 reinterpret_cast<void*>(pc), registered, native);
    GC_EXPECT_TRUE(!registered && native);
}

GC_TEST(ManagedMetadata, DataAddressIsNotCode)
{
    ManagedMetadataFixture fixture;
    const Uptr pc = fixture.Descriptor();
    fixture.Register();
    ElfUnloadQuiescence::ReadScope reader;
    const bool registeredData = ElfUnloadQuiescence::IsLinkedAddress(pc);
    const bool code = ElfUnloadQuiescence::IsLinkedAddress(pc, true);
    const auto descriptor = MFuncDesc::GetFuncDesc(pc);
    struct Input { ArchUInt start; FrameAddress frame; } input {};
#if defined(__x86_64__)
    input.start = pc + 9;
#elif defined(__arm__)
    input.start = pc + 12;
#else
    input.start = pc;
#endif
    UnwindContext context;
    context.frameInfo.mFrame.SetFA(&input.frame);
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(pc) + 1);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    StackFrameStream frames(&context);
    frames.Start();
    const bool native = !frames.IsDone() && frames.Current().GetFrameType() == FrameType::NATIVE;
    std::fprintf(stderr, "METADATA_DATA_ROOTS_TARGET native=%d executed=1\n", native);
    GC_EXPECT_TRUE(native);
    std::fprintf(stderr, "METADATA_DATA_PC_TARGET registered=%d code=%d descriptor=%p executed=1\n",
                 registeredData, code, descriptor);
    GC_EXPECT_TRUE(registeredData && !code && descriptor == nullptr);
}

// These exercise CheckRegisterRoots, never AnalyseAndSetFrameType. Removing
// its descriptor guard may hit the downstream lookup's fatal assertion; that
// alone is not evidence of the guard's read-before-map boundary.
// These two independent cases stay in the observed process. Existing forked
// rejection tests remain unchanged; no child result is substituted for a probe.
extern "C" __attribute__((noinline)) void A2ObserveRoots(StackFrameStream* frames, Uptr pc, Uptr map)
{
    std::fprintf(stderr, "A2_ROOTS_INPUT pc=%p map=%p\n", reinterpret_cast<void*>(pc), reinterpret_cast<void*>(map));
    frames->Start(); // real public recordedFrames -> CheckRegisterRoots
    std::fprintf(stderr, "A2_ROOTS_RESULT done=%d type=%d executed=1\n",
                 frames->IsDone(), int(frames->Current().GetFrameType()));
}
static void CheckRootsQualification(bool present)
{
    ManagedMetadataFixture fixture;
    const uint32_t* code = fixture.PC(present, present, present);
    const Uptr pc = reinterpret_cast<Uptr>(code);
    fixture.Register();
    FrameAddress address {};
    FrameInfo frame(code);
    frame.SetFrameType(FrameType::MANAGED);
    frame.mFrame.SetFA(&address);
    frame.mFrame.SetIP(code);
    const std::vector<FrameInfo> recorded {frame};
    StackFrameStream frames(recorded);
    A2ObserveRoots(&frames, pc, present ? fixture.zeroMap : 0);
    GC_EXPECT_TRUE(!frames.IsDone() && frames.Current().GetFrameType() == FrameType::MANAGED);
}
GC_TEST(ManagedMetadata, RootsMissingQualification) { CheckRootsQualification(false); }
GC_TEST(ManagedMetadata, RootsZeroQualification) { CheckRootsQualification(true); }

GC_TEST(ManagedMetadata, RootsRecordedDescriptorRejected)
{ CheckMetadata(Entry::ROOTS, false, false, nullptr); }
GC_TEST(ManagedMetadata, RootsRecordedZeroRoots)
{ CheckMetadata(Entry::ROOTS, true, true, nullptr, true); }

GC_TEST(ManagedMetadata, HeadAbsentDescriptor) { CheckMetadata(Entry::HEAD, false, false, "managed frame missing funcdesc"); }
GC_TEST(ManagedMetadata, HeadAbsentStackMap) { CheckMetadata(Entry::HEAD, true, false, "managed frame missing stackmap"); }
GC_TEST(ManagedMetadata, HeadPresent) { CheckMetadata(Entry::HEAD, true, true, nullptr); }
GC_TEST(ManagedMetadata, PrologueAbsent) { CheckMetadata(Entry::PROLOGUE, true, false, "FramePrologue missing stackmap"); }
GC_TEST(ManagedMetadata, ProloguePresent) { CheckMetadata(Entry::PROLOGUE, true, true, nullptr); }
GC_TEST(ManagedMetadata, EhAbsentDescriptor) { CheckMetadata(Entry::EH, false, false, "managed frame missing funcdesc"); }
GC_TEST(ManagedMetadata, EhAbsentStackMap) { CheckMetadata(Entry::EH, true, false, "managed frame missing stackmap"); }
GC_TEST(ManagedMetadata, EhPresent) { CheckMetadata(Entry::EH, true, true, nullptr); }
#if defined(__x86_64__)
GC_TEST(ManagedMetadata, ProfileAbsentDescriptor) { CheckMetadata(Entry::PROFILE, false, false, nullptr); }
GC_TEST(ManagedMetadata, ProfileEmpty) { CheckMetadata(Entry::PROFILE_EMPTY, false, false, nullptr); }
GC_TEST(ManagedMetadata, ReturnAbsentDescriptor) { CheckMetadata(Entry::RETURN, false, false, "return frame missing funcdesc"); }
GC_TEST(ManagedMetadata, ReturnZeroEntries) { CheckMetadata(Entry::RETURN, true, true, "return frame missing stackmap entry"); }
GC_TEST(ManagedMetadata, ReturnPcMiss) { CheckMetadata(Entry::RETURN, true, true, "return frame missing stackmap entry", true, true); }
GC_TEST(ManagedMetadata, ReturnZeroRoots) { CheckMetadata(Entry::RETURN, true, true, nullptr, true); }
#endif
#endif // Linux consumers
#if defined(__aarch64__)
GC_TEST(ManagedMetadata, CallerSpAbsentStackMap) { CheckMetadata(Entry::CALLER_SP, true, false, "managed frame missing stackmap"); }
GC_TEST(ManagedMetadata, CallerSpPresent) { CheckMetadata(Entry::CALLER_SP, true, true, nullptr); }
GC_TEST(ManagedMetadata, CallerSpNative) { CheckMetadata(Entry::CALLER_SP, false, false, nullptr); }
#endif
#endif

#if defined(_WIN64)
#include "os/Windows/UnwindWin.h"
#include <windows.h>
#include <cstdlib>
using namespace MapleRuntime;
namespace {
void CheckWindowsMetadata(bool caller, int kind)
{
    // These are real PE functions with .pdata/.xdata. Never execute their
    // synthetic managed code: the product's PE lookup consumes it as input.
    ManagedMetadataFixture fixture;
    const Uptr pc = reinterpret_cast<Uptr>(fixture.PC(kind != 0, kind == 2));
    const Uptr ip = pc + 1;
    WinModuleManager modules;
    modules.Init();
    fixture.Register();
    alignas(16) Uptr storage[16] {};
    FrameAddress callerFrame {};
    FrameAddress currentFrame {};
    currentFrame.returnAddress = reinterpret_cast<uint32_t*>(ip);
    currentFrame.callerFrameAddress = &callerFrame;
    MachineFrame machine;
    machine.SetFA(&currentFrame);
    machine.SetIP(reinterpret_cast<uint32_t*>(ip));
    storage[0] = reinterpret_cast<Uptr>(&callerFrame);
    std::fprintf(stderr, "METADATA_INPUT startPC=%p ip=%p\n",
                 reinterpret_cast<void*>(pc), reinterpret_cast<void*>(ip));
    std::fflush(stderr);
    FrameInfo result;
    if (caller) {
        auto status = UnwindContextStatus::RELIABLE;
        result = GetCallerFrameInfo(modules, machine, status);
    } else {
        result = GetCurFrameInfo(modules, ip, reinterpret_cast<Uptr>(&storage[2]));
    }
    std::fprintf(stderr, "WINDOWS_FRAME actual=%p expected=%p assertion-executed\n",
                 result.mFrame.GetFA(), &callerFrame);
    GC_EXPECT_TRUE(result.mFrame.GetFA() == &callerFrame);
}
}
GC_COMPONENT_TEST(ManagedMetadata, WinCurrentAbsentDescriptor) { CheckWindowsMetadata(false, 0); }
GC_COMPONENT_TEST(ManagedMetadata, WinCurrentAbsentStackMap) { CheckWindowsMetadata(false, 1); }
GC_COMPONENT_TEST(ManagedMetadata, WinCurrentPresent) { CheckWindowsMetadata(false, 2); }
GC_COMPONENT_TEST(ManagedMetadata, WinCallerAbsentDescriptor) { CheckWindowsMetadata(true, 0); }
GC_COMPONENT_TEST(ManagedMetadata, WinCallerAbsentStackMap) { CheckWindowsMetadata(true, 1); }
GC_COMPONENT_TEST(ManagedMetadata, WinCallerPresent) { CheckWindowsMetadata(true, 2); }
#endif
