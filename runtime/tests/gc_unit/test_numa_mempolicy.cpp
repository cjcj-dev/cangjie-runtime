// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ZGC os/linux/gc/z/zNUMA_linux.cpp populate_id_mappings uses
// os::numa_get_leaf_groups (os_linux.cpp:3252), i.e. the nodes this process may
// allocate from. NumaTopology::SealProcessTopology (Heap/z/zNUMA_linux.cpp)
// reads that set with get_mempolicy(nullptr, &mask, 64, nullptr,
// MPOL_F_MEMS_ALLOWED). Regression: the flag was 2 (MPOL_F_ADDR), so every
// query failed and the topology always fell back to node 0.

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/mempolicy.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Heap/z/zNUMA.inline.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;

namespace {
constexpr unsigned long kNumaMaskBits = sizeof(unsigned long) * 8;
constexpr int kSeccompUnavailable = 77;
}

// The sealed topology is exactly the allowed node mask. Only an environment
// denial of get_mempolicy itself (EPERM from seccomp/containers, ENOSYS) may
// leave the node-0 fallback in place.
GC_TEST(NumaMempolicy, ProcessTopologyIsAllowedNodeMask)
{
    unsigned long mask = 0;
    errno = 0;
    const long rc = syscall(SYS_get_mempolicy, nullptr, &mask, kNumaMaskBits, nullptr,
                            static_cast<unsigned long>(MPOL_F_MEMS_ALLOWED));
    const int err = rc == 0 ? 0 : errno;
    const NumaTopology topology = NumaTopology::SealProcessTopology();
    std::fprintf(stderr, "NUMA_MEMS_ALLOWED rc=%ld errno=%d(%s) mask=%#lx count=%zu\n", rc, err,
                 err == 0 ? "none" : std::strerror(err), mask, topology.Count());
    GC_EXPECT_TRUE(topology.IsSealed());
    if (rc != 0) {
        GC_EXPECT_TRUE(err == EPERM || err == ENOSYS);
        GC_EXPECT_EQ(topology.Count(), 1U);
        GC_EXPECT_EQ(topology.NodeAt(0), 0U);
        return;
    }
    GC_EXPECT_NE(mask, 0UL);
    size_t expected = 0;
    for (uint32_t node = 0; node < kNumaMaskBits; ++node) {
        if ((mask & (1UL << node)) == 0) {
            continue;
        }
        GC_EXPECT_TRUE(expected < topology.Count());
        GC_EXPECT_EQ(topology.NodeAt(expected), node);
        ++expected;
    }
    GC_EXPECT_EQ(topology.Count(), expected);
}

#if defined(__x86_64__) || defined(__aarch64__)
// A single-node host gives node 0 either way, so also pin the query itself:
// in a child, seccomp kills the process on any get_mempolicy whose 64-bit
// flags argument is not MPOL_F_MEMS_ALLOWED. The product call must survive.
GC_TEST(NumaMempolicy, ProcessTopologyQueriesMemsAllowed)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
#if defined(__x86_64__)
        constexpr uint32_t arch = AUDIT_ARCH_X86_64;
#else
        constexpr uint32_t arch = AUDIT_ARCH_AARCH64;
#endif
        constexpr uint32_t flagsLow = offsetof(struct seccomp_data, args[4]);
        struct sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_get_mempolicy, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, flagsLow),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, MPOL_F_MEMS_ALLOWED, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, flagsLow + 4),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog program = { static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
            prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
            std::fprintf(stderr, "NUMA_SECCOMP_UNAVAILABLE errno=%d(%s)\n", errno, std::strerror(errno));
            _exit(kSeccompUnavailable);
        }
        const NumaTopology topology = NumaTopology::SealProcessTopology();
        _exit(topology.IsSealed() ? 0 : 3);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    std::fprintf(stderr, "NUMA_QUERY_FLAG_CHILD status=%#x exited=%d code=%d signaled=%d sig=%d\n", status,
                 WIFEXITED(status) ? 1 : 0, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                 WIFSIGNALED(status) ? 1 : 0, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == kSeccompUnavailable) {
        // The environment refused the filter; the flag is not observable here.
        return;
    }
    GC_EXPECT_FALSE(WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS);
    GC_EXPECT_TRUE(WIFEXITED(status));
    GC_EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif
