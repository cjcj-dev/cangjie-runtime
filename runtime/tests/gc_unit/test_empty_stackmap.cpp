// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// HotSpot safepoint.cpp:777-778 and codeBlob.cpp:224: managed frames have
// metadata; an empty root set is distinct from a missing map.
#include <cstdio>
#include <csignal>
#include <string>
#include "CangjieRuntime.h"
#include "Exception/EhFrameInfo.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"
#include "gc_unittest.hpp"
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>

using namespace MapleRuntime;
namespace {
struct Metadata {
    int32_t slot = 0;
    uint32_t code[4] = {};
    int32_t descriptor[8] = {};
    alignas(Uptr) uint8_t stackmap[64] = {};
};
enum class Entry { HEAD, PROLOGUE, EH, RETURN, CALLER_SP };

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
        static Metadata image;
        if (descriptorPresent) {
            image.slot = reinterpret_cast<char*>(image.descriptor) - reinterpret_cast<char*>(&image.slot);
        }
        if (stackmapPresent) {
            image.descriptor[0] = reinterpret_cast<char*>(image.stackmap) - reinterpret_cast<char*>(image.descriptor);
        }
        if (zeroRootRow) {
            // Three prologue varints, then one PC=0 row with four zero indices.
            // All small varints occupy four bits. Register/slot/line tables are empty.
            image.stackmap[1] = 0x10; // record count = 1
            image.stackmap[2] = 0x11; // reg/slot index widths = 1
            image.stackmap[3] = 0x11; // line/derived index widths = 1
        }
        const Uptr pc = reinterpret_cast<Uptr>(image.code);
        ElfUnloadQuiescence::LinkImage(pc);
        if (entry == Entry::HEAD) {
            const auto head = CompressedStackMapHead::GetStackMapHead(pc, nullptr, pc + 4);
            if (head.GetInvalidReason(pc, pc + 4) != StackMapInvalidReason::ZERO_ENTRIES) { _exit(3); }
        } else if (entry == Entry::PROLOGUE) {
            const FramePrologue prologue(stackmapPresent ? reinterpret_cast<Uptr*>(image.stackmap) : nullptr);
            if (prologue.GetFrameSize() != 0 || prologue.GetSavedRegisterCount() != 0) { _exit(3); }
        } else if (entry == Entry::EH) {
            FrameInfo frame(image.code);
            frame.mFrame.SetIP(image.code + 1);
            FrameAddress address {};
            frame.mFrame.SetFA(&address);
            ExceptionWrapper exception;
            EHFrameInfo eh(frame, exception);
            CalleeSavedRegisterContext context {};
            eh.RestoreToCallerContext(context);
        } else if (entry == Entry::CALLER_SP) {
            FrameInfo frame(image.code);
            frame.SetFrameType(FrameType::MANAGED);
            frame.mFrame.SetIP(image.code + 1);
            (void)frame.CallerSP();
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
GC_TEST(ManagedMetadata, HeadAbsentDescriptor) { CheckMetadata(Entry::HEAD, false, false, "managed frame missing funcdesc"); }
GC_TEST(ManagedMetadata, HeadAbsentStackMap) { CheckMetadata(Entry::HEAD, true, false, "managed frame missing stackmap"); }
GC_TEST(ManagedMetadata, HeadPresent) { CheckMetadata(Entry::HEAD, true, true, nullptr); }
GC_TEST(ManagedMetadata, PrologueAbsent) { CheckMetadata(Entry::PROLOGUE, true, false, "FramePrologue missing stackmap"); }
GC_TEST(ManagedMetadata, ProloguePresent) { CheckMetadata(Entry::PROLOGUE, true, true, nullptr); }
GC_TEST(ManagedMetadata, EhAbsentDescriptor) { CheckMetadata(Entry::EH, false, false, "managed frame missing funcdesc"); }
GC_TEST(ManagedMetadata, EhAbsentStackMap) { CheckMetadata(Entry::EH, true, false, "managed frame missing stackmap"); }
GC_TEST(ManagedMetadata, EhPresent) { CheckMetadata(Entry::EH, true, true, nullptr); }
#if defined(__x86_64__)
GC_TEST(ManagedMetadata, ReturnAbsentDescriptor) { CheckMetadata(Entry::RETURN, false, false, "return frame missing funcdesc"); }
GC_TEST(ManagedMetadata, ReturnZeroEntries) { CheckMetadata(Entry::RETURN, true, true, "return frame missing stackmap entry"); }
GC_TEST(ManagedMetadata, ReturnPcMiss) { CheckMetadata(Entry::RETURN, true, true, "return frame missing stackmap entry", true, true); }
GC_TEST(ManagedMetadata, ReturnZeroRoots) { CheckMetadata(Entry::RETURN, true, true, nullptr, true); }
#endif
#if defined(__aarch64__)
GC_TEST(ManagedMetadata, CallerSpAbsentStackMap) { CheckMetadata(Entry::CALLER_SP, true, false, "managed frame missing stackmap"); }
GC_TEST(ManagedMetadata, CallerSpNative) { CheckMetadata(Entry::CALLER_SP, false, false, nullptr); }
#endif
#endif
