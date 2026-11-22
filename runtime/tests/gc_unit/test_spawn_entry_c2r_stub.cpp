// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Regression for "register r14 is not recorded" / "wrong stack pointer register
// info" (zStackWatermark.cpp process_frame). A managed caller spawning a thread
// keeps live stack pointers in callee-saved registers across CJ_MCC_NewCJThread.
// MutatorManager::ConstructMutator leaves its saferegion through
// ProcessSafepointIfRequested, so the stack watermark walks that caller from
// inside the spawn entry. The walk only knows where callee-saved registers were
// saved when the frame returning into the caller is a CalleeSavedRegistersStub
// (FrameType::C2R_STUB, StackInfo.cpp UpdateRegisterMap); a plain alias to the
// C++ body is a default frame that drops the register map.

#include <dlfcn.h>
#include <cstdint>

#include "gc_unittest.hpp"
#include "Common/StackType.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
bool ReturnsThroughC2RStub(const char* entry)
{
    void* address = dlsym(RTLD_DEFAULT, entry);
    if (address == nullptr) { return false; }
    // A return address strictly inside the entry body, as left by `call funcName`.
    MachineFrame frame(nullptr, reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(address) + 4));
    return frame.IsC2RStubFrame();
}
} // namespace

GC_TEST(SpawnEntryC2RStub, existing_safepoint_entry_is_c2r_stub)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    GC_EXPECT_TRUE(ReturnsThroughC2RStub("CJ_MCC_MutexLock"));
    GC_EXPECT_TRUE(ReturnsThroughC2RStub("CJ_MCC_FutureWait"));
#endif
}

GC_TEST(SpawnEntryC2RStub, new_cjthread_entries_record_callee_saved)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    GC_EXPECT_TRUE(ReturnsThroughC2RStub("CJ_MCC_NewCJThread"));
    GC_EXPECT_TRUE(ReturnsThroughC2RStub("CJ_MCC_NewCJThreadNoReturn"));
#endif
}
