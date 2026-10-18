// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include <cstddef>
namespace MapleRuntime {
class Mutator;
// java.cpp:375 and vmOperations.cpp:451: before-exit ownership is separate
// from the final global/per-thread termination publication.
class VM_Exit {
public:
    static bool begin_before_exit(bool (*qualify)() = nullptr);
    static void complete_before_exit(bool success);
    static bool closing();
    static bool exited();
    static bool is_shutdown_thread();
    static size_t set_vm_exited();
    static size_t native_threads_at_exit();
    static void wait_if_vm_exited();
    static void wait_for_native_entry();
    static bool is_native(const Mutator& mutator);
};
} // namespace MapleRuntime
