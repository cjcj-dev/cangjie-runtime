// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Cangjie.h"
#include "Interpreter/RuntimeTypes.h"
#include "TypeInfoManager.h"
#include "ObjectModel/MClass.inline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <new>
#include <thread>

using namespace MapleRuntime;

namespace MapleRuntime {
class TypeInfoManagerTestAccess {
public:
    static size_t DescriptorSize() { return sizeof(TypeInfoManager::GenericTiDesc); }
};
}

extern "C" TypeInfo* CJ_MCC_GetOrCreateTypeInfo(TypeTemplate*, U32, TypeInfo**);

namespace {
// ELF allocation interposition observes the product's real new/delete calls.
// No allocator or descriptor algorithm is copied into this consumer. Only the
// two creator threads participate, and each pauses on its first descriptor.
thread_local bool creator = false;
thread_local int vectorSlot = -1;
std::atomic<unsigned> prepared { 0 };
std::atomic<unsigned> descFreed { 0 };
std::atomic<unsigned> argsFreed { 0 };
std::atomic<bool> rendezvousFailed { false };
std::atomic<void*> descriptors[2];
std::atomic<void*> argumentVectors[2];
size_t descriptorSize;

void Check(bool condition, const char* label)
{
    std::printf("ASSERT %s %s\n", label, condition ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!condition) { std::_Exit(1); }
}

bool Await(const std::atomic<unsigned>& value, unsigned target)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (value.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void NoteDelete(void* ptr)
{
    if (ptr == nullptr) { return; }
    for (unsigned i = 0; i < 2; ++i) {
        void* expected = ptr;
        if (descriptors[i].compare_exchange_strong(expected, nullptr)) { ++descFreed; }
        expected = ptr;
        if (argumentVectors[i].compare_exchange_strong(expected, nullptr)) { ++argsFreed; }
    }
}

// Compiler metadata record; TypeTemplate deliberately has no C++ constructor.
struct TemplateRecord {
    const char* name;
    I8 type;
    I8 flag;
    U16 fieldNum;
    U16 typeArgsNum;
    std::atomic<U16> uuid;
    const GenericFunc* fieldFns;
    GenericFunc superFn;
    FuncRef finalizerMethod;
    ReflectInfo* reflectInfo;
    ExtensionData** extensions;
    U16 validInheritNum;
};
static_assert(sizeof(TemplateRecord) == sizeof(TypeTemplate));
static_assert(alignof(TemplateRecord) == alignof(TypeTemplate));
}

void* operator new(size_t size)
{
    void* ptr = std::malloc(size ? size : 1);
    if (!ptr) { throw std::bad_alloc(); }
    if (vectorSlot >= 0) {
        argumentVectors[vectorSlot].store(ptr);
        vectorSlot = -1;
    }
    return ptr;
}

void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    void* ptr = std::malloc(size ? size : 1);
    if (ptr && creator && size == descriptorSize) {
        creator = false;
        const unsigned index = prepared.fetch_add(1, std::memory_order_acq_rel);
        if (index < 2) {
            descriptors[index].store(ptr);
            // GenericTiDesc's first allocating operation owns its argument copy.
            vectorSlot = static_cast<int>(index);
            if (!Await(prepared, 2)) { rendezvousFailed.store(true); }
        }
    }
    return ptr;
}

void operator delete(void* ptr) noexcept { NoteDelete(ptr); std::free(ptr); }
void operator delete(void* ptr, size_t) noexcept { NoteDelete(ptr); std::free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { NoteDelete(ptr); std::free(ptr); }

int main(int argc, char** argv)
{
    const bool control = argc == 2 && std::strcmp(argv[1], "control") == 0;
    Dl_info library {};
    Check(dladdr(reinterpret_cast<void*>(&CJ_MCC_GetOrCreateTypeInfo), &library) != 0, "product-entry-loaded");
    std::printf("PRODUCT_SO=%s\n", library.dli_fname);
    RuntimeParam param {};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    Check(InitCJRuntime(&param) == E_OK, "runtime-init");

    DYN_TypeInfo argument {};
    argument.typeInfoName = "test:Int64";
    argument.type = TypeKind::TYPE_KIND_INT64;
    argument.instanceSize = sizeof(int64_t);
    argument.align = sizeof(int64_t);
    auto* arg = reinterpret_cast<TypeInfo*>(&argument);
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    manager.AddTypeInfo(arg);
    TypeInfo* args[] = { arg, arg };
    TemplateRecord record {};
    record.name = control ? "test:RawArray" : "test:CFunc";
    record.type = control ? TypeKind::TYPE_KIND_RAWARRAY : TypeKind::TYPE_KIND_CFUNC;
    record.typeArgsNum = control ? 1 : 2;
    auto* tt = reinterpret_cast<TypeTemplate*>(&record);
    manager.GetTypeTemplateUUID(tt);
    const U32 argCount = control ? 1 : 2;
    descriptorSize = TypeInfoManagerTestAccess::DescriptorSize();
    std::atomic<unsigned> done { 0 };
    TypeInfo* result[2] {};
    std::thread first([&] {
        creator = !control;
        result[0] = CJ_MCC_GetOrCreateTypeInfo(tt, argCount, args);
        done.fetch_add(1, std::memory_order_release);
    });
    std::thread second([&] {
        creator = !control;
        result[1] = CJ_MCC_GetOrCreateTypeInfo(tt, argCount, args);
        done.fetch_add(1, std::memory_order_release);
    });
    Check(Await(done, 2), "two-native-callers-complete");
    first.join();
    second.join();
    Check(result[0] && result[0] == result[1], "one-canonical-ti");
    // GetUUID can lazily register a TI; this check must observe publication
    // without repairing the very condition under test.
    Check(!result[0]->IsInitialUUID(), "published-uuid-nonzero");
    Check(manager.ContainsTypeInfo(result[0]), "published-ti-registered");
    Check((reinterpret_cast<uintptr_t>(result[0]) & 7U) == 0, "arena-alignment");
    Check(result[0]->GetInstanceSize() == sizeof(void*), "ti-pointer-size");
    Check(result[0]->GetTypeArgNum() == argCount, "ti-arguments");
    Check(CJ_MCC_GetOrCreateTypeInfo(tt, argCount, args) == result[0], "warm-cache-canonical");
    if (!control) {
        std::printf("PREPARED=%u DESC_FREED=%u ARGS_FREED=%u\n",
            prepared.load(), descFreed.load(), argsFreed.load());
        Check(!rendezvousFailed.load() && prepared.load() == 2, "prepare-outside-bucket-lock");
        Check(descFreed.load() == 1 && argsFreed.load() == 1, "loser-and-owned-args-freed");
    }

    // The loader invokes this invalidation after reader drain. Both native
    // callers are joined, so this isolated fixture has no readers in flight.
    const U32 priorUUID = result[0]->GetUUID();
    manager.RemoveTypeInfosInRange(reinterpret_cast<uintptr_t>(tt), sizeof(record));
    Check(!manager.ContainsTypeInfo(result[0]), "unload-removes-ti");
    if (!control) {
        Check(descFreed.load() == 2 && argsFreed.load() == 2, "unload-frees-winning-descriptor");
    }
    TypeInfo* recreated = CJ_MCC_GetOrCreateTypeInfo(tt, argCount, args);
    Check(recreated && recreated != result[0], "unload-invalidates-fast-map");
    Check(!recreated->IsInitialUUID() && recreated->GetUUID() != priorUUID, "recreated-uuid-fresh");
    Check(manager.ContainsTypeInfo(recreated), "recreated-ti-registered");
    Check(FiniCJRuntime() == E_OK, "runtime-fini");
    std::puts(control ? "CONTROL_PASS" : "CFUNC_DESCRIPTOR_PASS");
    return 0;
}
