// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Interpreter/RuntimeTypes.h"
#include "ObjectModel/MClass.inline.h"
#include "ObjectModel/ExtensionData.h"

using namespace MapleRuntime;

// The interpreter's supported external metadata representation feeds the real
// compiler-call entry. No MTableDesc allocator/publication algorithm is copied.
struct Fixture {
    DYN_TypeInfo types[2] {};
    DYN_TypeInfo itf {};
    DYN_ExtensionData extensions[2] {};
    DYN_ExtensionData* lists[2][1] {};
    DYN_FuncPtr functions[2][1] {};
};
static_assert(sizeof(DYN_TypeInfo) == sizeof(TypeInfo));
static_assert(sizeof(DYN_ExtensionData) == sizeof(ExtensionData));
static int Answer() { return 42; }
static void Initialize(Fixture& f)
{
    f.itf.typeInfoName = "mtablearena:I";
    f.itf.type = TypeKind::TYPE_KIND_INTERFACE;
    for (unsigned i = 0; i < 2; ++i) {
        auto& ti = f.types[i];
        ti.typeInfoName = i == 0 ? "mtablearena:A" : "mtablearena:B";
        ti.type = TypeKind::TYPE_KIND_STRUCT;
        ti.instanceSize = 8;
        ti.align = 8;
        ti.vExtensionDataStart = f.lists[i];
        ti.mTableDesc = reinterpret_cast<DYN_MTableDesc*>(SIGN_BIT | 1U);
        auto& ed = f.extensions[i];
        ed.isInterfaceTypeInfo = 1;
        ed.flag = 6; // function table is already updated
        ed.funcTableSize = 1;
        ed.ti = &ti;
        ed.interfaceTypeInfo = &f.itf;
        ed.funcTable = f.functions[i];
        f.functions[i][0] = reinterpret_cast<void*>(&Answer);
        f.lists[i][0] = &ed;
    }
}

#ifdef MTABLE_ARENA_PLUGIN
extern "C" __attribute__((visibility("default"))) Fixture* MTableArenaFixture()
{
    static Fixture fixture;
    Initialize(fixture);
    return &fixture;
}
#else
#include "Cangjie.h"
#include "TypeInfoManager.h"
#include "Loader/CjFileLoader/CjFileLoader.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <thread>

extern "C" FuncPtr* CJ_MCC_GetMTable(TypeInfo*, TypeInfo*);
namespace {
std::atomic<void*> snapshots[4];
std::atomic<unsigned> destroyed { 0 };
void Check(bool value, const char* name)
{
    std::printf("ASSERT %s %s\n", name, value ? "PASS" : "FAIL");
    std::fflush(stdout);
    if (!value) { std::_Exit(1); }
}
void NoteDelete(void* pointer)
{
    if (!pointer) { return; }
    for (auto& snapshot : snapshots) {
        void* expected = pointer;
        if (snapshot.compare_exchange_strong(expected, nullptr)) { ++destroyed; }
    }
}

// Existing native loader fixture pattern (typeinfo_reuse_harness.cpp). The
// image is a real dlopen ELF; product RemoveLoadedFiles performs reader drain,
// image dependency purge, descriptor destruction, then this destructor dlcloses.
class FixtureFile final : public BaseFile {
public:
    FixtureFile(void* h, Fixture* f) : BaseFile(CString("mtable-arena.so")), handle(h), fixture(f) {}
    ~FixtureFile() override { Check(dlclose(handle) == 0, "plugin-dlclose"); }
    void RegisterFile() override {}
    void UnregisterFile() override {}
    bool IsAddrInCJFile(Uptr p) const override
    { return p >= GetFileMetaAddr() && p - GetFileMetaAddr() < sizeof(Fixture); }
    Uptr GetPackageInfoBase() override { return 0; }
    U32 GetPackageInfoTotalSize() override { return 0; }
    void GetGlobalInitFunc(std::vector<Uptr>&) const override {}
    Uptr GetFileMetaAddr() const override { return reinterpret_cast<Uptr>(fixture); }
    Uptr GetExtensionDataBase() override { return 0; }
    U32 GetExtensionDataSize() override { return 0; }
    Uptr GetInnerTypeExtensionsBase() override { return 0; }
    U32 GetInnerTypeExtensionsSize() override { return 0; }
    Uptr GetOuterTypeExtensionsBase() override { return 0; }
    U32 GetOuterTypeExtensionsSize() override { return 0; }
    Uptr GetStaticGIBase() override { return 0; }
    U32 GetStaticGISize() override { return 0; }
    Uptr GetTypeInfoBase() override { return reinterpret_cast<Uptr>(fixture->types); }
    U32 GetTypeInfoTotalSize() override { return sizeof(fixture->types) + sizeof(fixture->itf); }
    Uptr GetTypeExtBase() override { return 0; }
    U32 GetTypeExtTotalSize() override { return 0; }
    CString GetSDKVersion() const override { return CString(""); }
private:
    void* handle;
    Fixture* fixture;
};

void CreateAndConsume(Fixture& f, unsigned firstSnapshot)
{
    auto& manager = TypeInfoManager::GetTypeInfoManager();
    auto* itf = reinterpret_cast<TypeInfo*>(&f.itf);
    manager.AddTypeInfo(itf);
    std::atomic<unsigned> ready { 0 };
    FuncPtr* tables[2] {};
    std::thread threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        threads[i] = std::thread([&, i] {
            ++ready;
            while (ready.load() != 2) { std::this_thread::yield(); }
            tables[i] = CJ_MCC_GetMTable(reinterpret_cast<TypeInfo*>(&f.types[i]), itf);
        });
    }
    for (auto& thread : threads) { thread.join(); }
    MTableDesc* desc[2];
    for (unsigned i = 0; i < 2; ++i) {
        auto* ti = reinterpret_cast<TypeInfo*>(&f.types[i]);
        desc[i] = ti->GetMTableDesc();
        auto address = reinterpret_cast<uintptr_t>(desc[i]);
        Check(desc[i] && !ti->IsMTableDescUnInitialized(), "published-descriptor");
        Check(manager.ContainsAddress(address), "descriptor-in-product-arena");
        Check(address % alignof(MTableDesc) == 0 && address >> 48 == 0, "descriptor-alignment-low48");
        Check(desc[i]->mTableBitmap.tag == (SIGN_BIT | 1U), "short-bitmap-preserved");
        Check(tables[i] && reinterpret_cast<int(*)()>(tables[i][0])() == 42, "product-dispatch");
        Check(CJ_MCC_GetMTable(ti, itf) == tables[i], "stable-dispatch");
        auto* snapshot = desc[i]->stableTable.load();
        Check(snapshot != nullptr && snapshot->size() == 1, "stable-snapshot-created");
        snapshots[firstSnapshot + i].store(snapshot);
    }
    auto a = reinterpret_cast<uintptr_t>(desc[0]), b = reinterpret_cast<uintptr_t>(desc[1]);
    Check(a + sizeof(MTableDesc) <= b || b + sizeof(MTableDesc) <= a, "distinct-descriptors-do-not-overlap");
}
}
void operator delete(void* p) noexcept { NoteDelete(p); std::free(p); }
void operator delete(void* p, size_t) noexcept { NoteDelete(p); std::free(p); }

int main(int argc, char** argv)
{
    Check(argc == 2, "arguments");
    Dl_info library {};
    Check(dladdr(reinterpret_cast<void*>(&CJ_MCC_GetMTable), &library) != 0, "product-entry-loaded");
    std::printf("PRODUCT_SO=%s\n", library.dli_fname);
    RuntimeParam param {};
    param.coParam.processorNum = 1;
    param.heapParam.heapSize = 32 * 1024;
    Check(InitCJRuntime(&param) == E_OK, "runtime-init");
    if (std::strcmp(argv[1], "control") == 0) {
        DYN_TypeInfo integer {};
        integer.typeInfoName = "mtablecontrol:Int64";
        integer.type = TypeKind::TYPE_KIND_INT64;
        auto* ti = reinterpret_cast<TypeInfo*>(&integer);
        auto& manager = TypeInfoManager::GetTypeInfoManager();
        manager.AddTypeInfo(ti);
        Check(!ti->IsInitialUUID() && manager.ContainsTypeInfo(ti), "ordinary-ti-registration");
        Check(FiniCJRuntime() == E_OK, "runtime-fini");
        std::puts("CONTROL_PASS");
        return 0;
    }
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    Check(handle != nullptr, "plugin-dlopen");
    auto getFixture = reinterpret_cast<Fixture*(*)()>(dlsym(handle, "MTableArenaFixture"));
    Check(getFixture != nullptr, "plugin-symbol");
    Fixture* fixture = getFixture();
    CJFileLoader loader;
    auto* file = new FixtureFile(handle, fixture);
    loader.AddLoadedFiles(file);
    CreateAndConsume(*fixture, 0);
    loader.RemoveLoadedFiles(file);
    Check(destroyed.load() == 2, "unload-destroys-both-stable-snapshots");
    // A separate host-resident fixture survives until real runtime finalization.
    Fixture remaining;
    Initialize(remaining);
    CreateAndConsume(remaining, 2);
    Check(FiniCJRuntime() == E_OK, "runtime-fini");
    Check(destroyed.load() == 4, "fini-destroys-remaining-stable-snapshots");
    std::puts("MTABLE_ARENA_PASS");
    return 0;
}
#endif
