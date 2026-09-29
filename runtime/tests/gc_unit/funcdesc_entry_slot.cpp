// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Consume a real paired-llc input through the product return-root collector.
// Usage: funcdesc_entry_slot <linked-llc-output.so> <leaf|neighbor>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "CangjieRuntime.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/StackFrameCursor.h"

using namespace MapleRuntime;
int main(int argc, char** argv)
{
    if (argc != 3 || (std::strcmp(argv[2], "leaf") != 0 && std::strcmp(argv[2], "neighbor") != 0)) { return 2; }
    const bool leaf = std::strcmp(argv[2], "leaf") == 0;
    void* image = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (image == nullptr) { std::fprintf(stderr, "INPUT_LOAD_FAILURE %s\n", dlerror()); return 2; }
    const Uptr pc = reinterpret_cast<Uptr>(dlsym(image, leaf ? "slot_leaf" : "slot_neighbor"));
    if (pc == 0 || ElfUnloadQuiescence::LinkImage(pc) == nullptr) { return 2; }
#if defined(__x86_64__)
    // Find the real emitted return-poll PC, independently of the metadata
    // decoder under test. dlsym on the following exported sentinel bounds code.
    const Uptr end = reinterpret_cast<Uptr>(dlsym(image, leaf ? "leaf_end" : "neighbor_end"));
    Uptr site = 0;
    const unsigned char poll[] = {0x49, 0x3b, 0x67, 0x30}; // cmp 48(%r15),%rsp
    for (Uptr p = pc; end > pc && p + sizeof(poll) <= end; ++p) {
        if (std::memcmp(reinterpret_cast<void*>(p), poll, sizeof(poll)) == 0) { site = p; break; }
    }
    if (site == 0) { std::fprintf(stderr, "INPUT_RETURN_POLL_MISSING\n"); return 2; }
    int resultPipe[2];
    if (pipe(resultPipe) != 0) { return 2; }
    const pid_t child = fork();
    if (child < 0) { return 2; }
    if (child == 0) {
        close(resultPipe[0]);
        CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
        alignas(16) uintptr_t storage[64] {};
        auto* fp = &storage[56];
        fp[-1] = 0x10000; // rax return oop; no object dereference by this collector
        fp[-10] = pc;
        fp[-11] = site;
        MachineFrame machine;
        machine.SetFA(reinterpret_cast<FrameAddress*>(fp));
        machine.SetSP(reinterpret_cast<uintptr_t>(storage));
        const FrameInfo frame(machine, FrameType::RETURN_SAFEPOINT);
        std::vector<StackFrameCursor::ReturnRegisterRoot> roots;
        StackFrameCursor::CollectReturnRegisterRoots(frame, roots);
        const bool target = roots.size() == 1 &&
            reinterpret_cast<uintptr_t>(roots[0].slot) == reinterpret_cast<uintptr_t>(fp - 1) &&
            reinterpret_cast<uintptr_t>(roots[0].object) == 0x10000;
        const unsigned result = target ? 1 : 0;
        const auto bytes = write(resultPipe[1], &result, sizeof(result));
        _exit(bytes == sizeof(result) ? 0 : 2);
    }
    close(resultPipe[1]);
    unsigned result = 0;
    const auto bytes = read(resultPipe[0], &result, sizeof(result));
    close(resultPipe[0]);
    int status = 0;
    const auto waited = waitpid(child, &status, 0);
    const bool target = bytes == sizeof(result) && waited == child && status == 0 && result == 1;
    std::fprintf(stderr, "RETURN_ROOT_TARGET leaf=%d one_root_at_rax=%d bytes=%zd status=%d assertion-executed\n",
                 leaf, target, bytes, status);
    return target ? 0 : 1;
#else
    return 125;
#endif
}
