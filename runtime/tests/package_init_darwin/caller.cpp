// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#define MRT_USE_CJTHREAD_RENAME 1
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Cangjie.h"
#include "Common/Runtime.h"
#include "Loader/BinaryFile/CjFile/CjFile.h"
#include "Loader/CjFileLoader/CjFileLoader.h"
#include "LoaderManager.h"
#include "Mutator/Mutator.h"
#include "Loader/PackageInit.h"

using namespace MapleRuntime;
extern "C" {
bool MRT_NewForeignCJThread();
uint32_t InvokeBegin(const void*, const void*, uint32_t, void**);
void InvokeComplete(void*);
void InvokeFail(void*, uint32_t);
void InvokeAbort(const void*, const void*, uint32_t, uint32_t);
}
namespace {
void Package() {}
void Unit() {}
struct Metadata {
    CJFileHeader header {};
    U32 size { sizeof(Metadata) };
    U64 sdk {};
    U64 addresses[C_FILE_MAX] {};
    U32 sizes[C_FILE_MAX] {};
    CJGCFlagsTable flags { 1, 1, 0 };
    Uptr entry { reinterpret_cast<Uptr>(&Package) };
} metadata;
void Check(const char* name, bool ok)
{
    std::fprintf(stderr, "PACKAGE_INIT_TARGET %s executed=1 pass=%d\n", name, ok);
    std::fflush(stderr);
    if (!ok) { std::_Exit(1); }
}
void Init()
{
    RuntimeParam param {};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    Check("runtime-init", InitCJRuntime(&param) == E_OK);
    metadata.header.cJFileSize = &metadata.size;
    metadata.header.cJSDKVersionPtr = &metadata.sdk;
    for (unsigned i = 0; i < C_FILE_MAX; ++i) {
        metadata.header.tables[i] = { &metadata.addresses[i], &metadata.sizes[i] };
    }
    metadata.addresses[GC_FLAGS_TABLE] = reinterpret_cast<Uptr>(&metadata.flags);
    metadata.sizes[GC_FLAGS_TABLE] = sizeof(metadata.flags);
    metadata.addresses[GLOBAL_INIT_FUNC_TABLE] = reinterpret_cast<Uptr>(&metadata.entry);
    metadata.sizes[GLOBAL_INIT_FUNC_TABLE] = sizeof(metadata.entry);
    auto* loader = static_cast<CJFileLoader*>(LoaderManager::GetInstance()->GetLoader());
    loader->AddLoadedFiles(new CJFile(CString("darwin-package-init-caller"), reinterpret_cast<Uptr>(&metadata)));
    loader->RegisterLoadFile(reinterpret_cast<Uptr>(&metadata));
    Check("foreign-attach", MRT_NewForeignCJThread());
    Mutator::GetMutator()->SetManagedContext(false);
}
}
int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const void* package = reinterpret_cast<const void*>(&Package);
    const void* unit = reinterpret_cast<const void*>(&Unit);
    if (std::strcmp(argv[1], "Abort") == 0) {
        InvokeAbort(package, unit, 1, 4);
        Check("Abort", false); // Correct product exits 70 and prints phase/result.
    }
    Init();
    void* token = nullptr;
    if (std::strcmp(argv[1], "Begin") == 0) {
        const uint32_t result = InvokeBegin(package, unit, 0, &token);
        Check("Begin", result == 0 && token != nullptr);
        MCC_PackageInitComplete(token);
    } else {
        Check("setup-owner", MCC_PackageInitBegin(package, unit, 0, &token) == 0 && token != nullptr);
        uint32_t expected;
        if (std::strcmp(argv[1], "Complete") == 0) {
            InvokeComplete(token);
            expected = 1;
        } else if (std::strcmp(argv[1], "Fail") == 0) {
            InvokeFail(token, 1);
            expected = 2;
        } else { return 2; }
        void* next = reinterpret_cast<void*>(1);
        const uint32_t result = MCC_PackageInitBegin(package, unit, 0, &next);
        Check(argv[1], result == expected && next == nullptr);
    }
    // Isolated process: avoid making runtime teardown part of this ABI assertion.
    std::_Exit(0);
}
