// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "Concurrency/ConcurrencyModel.h"
#include "Common/Runtime.h"
#include "TypeInfoManager.h"

using namespace MapleRuntime;
extern "C" void* CJ_MCC_NewCJThreadNoReturn(void*, void*, void*, void*);

namespace {
// Native pointer-transport witness only: this callback does not interpret a
// generic TypeInfo or emulate Future<T>.executeClosure. Managed O2 is separate.
// TypeInfo is a compiler metadata record with a deleted C++ constructor.
// Use the same aligned image storage as the existing native GC fixture.
alignas(TypeInfo) unsigned char environmentImage[sizeof(TypeInfo)]{};
TypeInfo* const environment = reinterpret_cast<TypeInfo*>(environmentImage);
std::atomic<bool> reached{false};
std::atomic<void*> observed{nullptr};
bool control = false;
bool copied = false;
unsigned rootMask = 0;
unsigned nativeRoots = 0;

#if defined(__aarch64__)
void Consume(void*, TypeInfo* owner)
#else
void Consume(void*, void*, TypeInfo* owner)
#endif
{
    observed.store(owner, std::memory_order_relaxed);
    reached.store(true, std::memory_order_release);
}
int32_t NativeMain()
{
    if (control) {
        std::fprintf(stderr, "C05_CONTROL main=1\n");
        return 0;
    }
    const bool entered = MRT_EnterSaferegion(false);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!reached.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) {
        // Yield the CJThread, not only the carrier OS thread: this fixture
        // must also make progress when the bootstrap carrier is the only one.
        CJThreadResched();
    }
    if (entered) { MRT_LeaveSaferegion(); }
    const bool delivered = reached.load(std::memory_order_acquire) && observed.load() == environment;
    const bool ok = copied && rootMask == 7 && nativeRoots == 0 && delivered;
    std::fprintf(stderr, "C05_PAYLOAD_ASSERT bytes=%zu copied=%d roots=%u nativeRoots=%u reached=%d ti=%d ok=%d\n",
        sizeof(LWTData), copied, rootMask, nativeRoots, reached.load(), delivered, ok);
    return ok ? 0 : 6;
}
}

int main(int argc, char** argv)
{
    control = argc == 2 && std::strcmp(argv[1], "control") == 0;
    MRT_CjRuntimeInit();
    environment->SetType(TypeKind::TYPE_KIND_CLASS);
    environment->SetInstanceSize(sizeof(void*));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(environmentImage), sizeof(environmentImage));
    if (!control) {
        // Use the existing public four-argument alias, allocation, validation,
        // copy and queued-thread producer; no test-only runtime entry.
        auto* thread = CJ_MCC_NewCJThreadNoReturn(reinterpret_cast<void*>(&Consume), nullptr, nullptr, environment);
        if (thread == nullptr) { std::fprintf(stderr, "C05_CREATE_ASSERT handle=null\n"); return 60; }
        const auto previous = CJThreadGetHandle();
        ThreadLocal::SetCJThread(thread);
        auto* data = static_cast<LWTData*>(CJThreadGetArg());
        ThreadLocal::SetCJThread(previous);
        copied = data != nullptr && data->fn == reinterpret_cast<void*>(&Consume) &&
            data->futureTi == environment && data->execute == nullptr &&
            data->obj == nullptr && data->threadObject == nullptr;
        RootVisitor visitor = [&](RootSlot& slot) {
            const void* address = &slot;
            if (address == &data->execute) { rootMask |= 1; }
            if (address == &data->obj) { rootMask |= 2; }
            if (address == &data->threadObject) { rootMask |= 4; }
            if (address == &data->fn || address == &data->futureTi) { ++nativeRoots; }
        };
        Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor);
    }
    MRT_CjRuntimeStart(reinterpret_cast<void*>(&NativeMain));
    return 61; // Product main must supply the actual process result.
}
