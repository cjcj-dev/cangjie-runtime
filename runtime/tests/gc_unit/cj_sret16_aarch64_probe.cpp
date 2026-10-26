// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Freestanding machine-code ABI probe for the same entry used by test_zPage.
// This is not an ARM runtime/GC or managed-language consumer.
#include "cj_sret16_fixture.hpp"

using Word = __UINTPTR_TYPE__;
static Word control[4] = {91, 0, 0, 92};
static Word result[4] = {93, 0, 0, 94};
static Word decoy[4] = {95, 0, 0, 96};

extern "C" __attribute__((noinline)) void GcUnitCJSRet16Body(void* output)
{
    auto* words = static_cast<Word*>(output);
    words[0] = 0x12345678;
    words[1] = 584;
}

// Same sret handoff as ApplyCangjieMethodStub: its fifth native argument x4
// becomes x8; ordinary argument x0 deliberately names a distinct valid buffer.
extern "C" void InvokeCJSRet16(void*, void*, void*, void*, void*);
asm(".text\n.p2align 2\n.globl InvokeCJSRet16\n"
    ".type InvokeCJSRet16, %function\n"
    "InvokeCJSRet16:\n.cfi_startproc\n"
    "mov x8, x4\n"
    "b GcUnitCJSRet16Entry\n.cfi_endproc\n"
    ".size InvokeCJSRet16, .-InvokeCJSRet16\n");

static void Write(const char* text, Word size)
{
    register long fd asm("x0") = 1;
    register const char* data asm("x1") = text;
    register Word length asm("x2") = size;
    register long number asm("x8") = 64;
    asm volatile("svc 0" : "+r"(fd) : "r"(data), "r"(length), "r"(number) : "memory");
}

static int Check()
{
    GcUnitCJSRet16Body(control + 1);
    if (control[0] != 91 || control[1] != 0x12345678 || control[2] != 584 || control[3] != 92) { return 31; }
    constexpr char controlOk[] = "SRET16_DIRECT_CONTROL_PASS\n";
    Write(controlOk, sizeof(controlOk) - 1);
    InvokeCJSRet16(decoy + 1, nullptr, nullptr, nullptr, result + 1);
    if (result[1] != 0x12345678 || result[2] != 584) {
        constexpr char targetFail[] = "SRET16_CJ_TARGET_FAIL\n";
        Write(targetFail, sizeof(targetFail) - 1);
        return 41;
    }
    if (result[0] != 93 || result[3] != 94 || decoy[0] != 95 || decoy[1] != 0 ||
        decoy[2] != 0 || decoy[3] != 96) { return 42; }
    constexpr char targetOk[] = "SRET16_CJ_TARGET_PASS\n";
    Write(targetOk, sizeof(targetOk) - 1);
    return 0;
}

extern "C" void _start()
{
    const int result = Check();
    register long status asm("x0") = result;
    register long number asm("x8") = 93;
    asm volatile("svc 0" : : "r"(status), "r"(number) : "memory");
    __builtin_unreachable();
}
