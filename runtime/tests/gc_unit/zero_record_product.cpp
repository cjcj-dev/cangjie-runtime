// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Real LLVM metadata enters the shipped trace/GC/return consumers. No decoder
// is compiled into this executable. HotSpot oopMap.cpp:85-91 guards count.
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "CangjieRuntime.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "Mutator/Mutator.h"
#include "UnwindStack/StackFrameCursor.h"
#include "UnwindStack/StackInfo.h"
using namespace MapleRuntime;

static int Run(void* image, const char* target)
{
    const bool line = std::strcmp(target, "line") == 0;
    const bool root = std::strcmp(target, "root") == 0;
    const bool zeroRoot = std::strcmp(target, "zero-root") == 0;
    const bool gcEmpty = std::strcmp(target, "gc-empty") == 0;
    const char* name = line ? "line_record" : root ? "root_record" : zeroRoot ? "zero_root_record" : "zero_record";
    const Uptr entry = reinterpret_cast<Uptr>(dlsym(image, name));
    const Uptr metadataStart = reinterpret_cast<Uptr>(dlsym(image, "_CJMetadataStart"));
    if (!entry || !metadataStart || !ElfUnloadQuiescence::LinkImage(metadataStart)) { return 2; }
    ElfUnloadQuiescence::ReadScope reader;
    const U16 kind = root || zeroRoot ? 3 : line ? 1 : 0;
    Uptr site = 0;
    for (Uptr offset = 0; offset < 256; ++offset) {
        const auto metadata = ElfUnloadQuiescence::FindFrameMetadata(entry + offset, kind);
        if (metadata.entry == entry && metadata.descriptor &&
            (kind == 3 ? metadata.match == ElfUnloadQuiescence::QualificationMatch::SAVED_SITE :
                         (metadata.bits & 3) == 3 && metadata.match != ElfUnloadQuiescence::QualificationMatch::NONE)) {
            site = entry + offset;
            break;
        }
    }
    if (!site) { std::fprintf(stderr, "INPUT_QUALIFICATION_FAILURE target=%s\n", target); return 2; }
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
    alignas(16) Uptr storage[64] {};
    auto* fp = storage + 56;
    fp[-1] = entry + 9;
    MachineFrame machine;
    machine.SetFA(reinterpret_cast<FrameAddress*>(fp));
    machine.SetSP(reinterpret_cast<Uptr>(storage));
    machine.SetIP(reinterpret_cast<const uint32_t*>(site));
    if (root || zeroRoot) {
        fp[-1] = root ? 0x10000 : 0;
        fp[-10] = entry;
        fp[-11] = site;
        const FrameInfo frame(machine, FrameType::RETURN_SAFEPOINT);
        std::vector<StackFrameCursor::ReturnRegisterRoot> roots;
        std::fprintf(stderr, "PRODUCT_TARGET_ENTER target=%s entry=%p site=%p\n", target, (void*)entry, (void*)site);
        StackFrameCursor::CollectReturnRegisterRoots(frame, roots);
        const bool result = root ? roots.size() == 1 && roots[0].slot == reinterpret_cast<ObjectRef*>(fp - 1) &&
            reinterpret_cast<Uptr>(roots[0].object) == 0x10000 : roots.empty();
        std::fprintf(stderr, "PRODUCT_RESULT target=%s roots=%zu assertion-executed result=%d\n", target, roots.size(), result);
        return result ? 0 : 1;
    }
    FrameInfo frame(machine, FrameType::MANAGED);
    if (!frame.ResolveProcInfo(kind)) { return 2; }
    std::fprintf(stderr, "PRODUCT_TARGET_ENTER target=%s entry=%p site=%p qualified=1\n", target, (void*)entry, (void*)site);
    if (gcEmpty) {
        Mutator mutator;
        RegSlotsMap registers;
        RootVisitor visitor = [](RootSlot&) {};
        DerivedPtrVisitor derived = [](zaddress_unsafe, DerivedSlot&) {};
        StackFrameCursor::ProcessManagedFrame(visitor, &derived, registers, frame, mutator);
        std::fprintf(stderr, "PRODUCT_GC_UNEXPECTED_RETURN\n");
        return 1;
    }
    const auto raw = StackInfo::CaptureRawFrame(frame);
    const U32 expected = line ? 37 : 0;
    const bool result = raw.lineNumber == expected;
    std::fprintf(stderr, "PRODUCT_RESULT target=%s line=%lld expected=%u assertion-executed result=%d\n", target, static_cast<long long>(raw.lineNumber), expected, result);
    return result ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc != 3) { return 2; }
    const std::string target = argv[2];
    if (target != "empty" && target != "line" && target != "root" && target != "zero-root" &&
        target != "gc-empty" && target != "truncated") { return 2; }
    void* image = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!image) { std::fprintf(stderr, "INPUT_LOAD_FAILURE %s\n", dlerror()); return 2; }
    Dl_info product {};
    if (!dladdr(reinterpret_cast<void*>(&StackInfo::CaptureRawFrame), &product) || !product.dli_fname) { return 2; }
    std::fprintf(stderr, "PRODUCT_LOADED path=%s fixture=%s target=%s\n", product.dli_fname, argv[1], argv[2]);
    int pipes[2];
    if (pipe(pipes) != 0) { return 2; }
    const pid_t child = fork();
    if (child < 0) { return 2; }
    if (child == 0) {
        close(pipes[0]);
        if (dup2(pipes[1], STDERR_FILENO) < 0) { _exit(2); }
        close(pipes[1]);
        signal(SIGABRT, SIG_DFL);
        _exit(Run(image, argv[2]));
    }
    close(pipes[1]);
    std::string transcript;
    char buffer[1024];
    ssize_t count;
    while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0) { transcript.append(buffer, count); }
    close(pipes[0]);
    int status = 0;
    if (waitpid(child, &status, 0) != child) { return 2; }
    std::fwrite(transcript.data(), 1, transcript.size(), stderr);
    bool result = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (target == "gc-empty" || target == "truncated") {
        const char* message = target == "gc-empty" ? "managed frame missing exact root map" : "stackmap read exceeds function payload";
        result = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT && transcript.find(message) != std::string::npos;
    }
    result = result && transcript.find("PRODUCT_TARGET_ENTER") != std::string::npos;
    std::fprintf(stderr, "TARGET_ASSERT target=%s status=%d assertion-executed result=%d\n", argv[2], status, result);
    return result ? 0 : 1;
}
