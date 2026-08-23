// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include "Loader/CjFileLoader/CjFileLoader.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using MapleRuntime::TypeInfo;
using MapleRuntime::TypeInfoManager;

namespace {
using AddressFn = void* (*)();
using SizeFn = size_t (*)();

struct Image {
    void* handle;
    TypeInfo* typeInfo;
};

[[noreturn]] void Fail(const char* operation);

class ReuseFile final : public MapleRuntime::BaseFile {
public:
    explicit ReuseFile(Image image)
        : BaseFile(MapleRuntime::CString("abastress-image")), handle(image.handle), typeInfo(image.typeInfo)
    {
    }

    void RegisterFile() override {}
    void UnregisterFile() override
    {
        if (dlclose(handle) != 0) {
            Fail("dlclose");
        }
        handle = nullptr;
    }
    bool IsAddrInCJFile(MapleRuntime::Uptr) const override { return false; }
    MapleRuntime::Uptr GetPackageInfoBase() override { return 0; }
    MapleRuntime::U32 GetPackageInfoTotalSize() override { return 0; }
    void GetGlobalInitFunc(std::vector<MapleRuntime::Uptr>&) const override {}
    MapleRuntime::Uptr GetFileMetaAddr() const override { return reinterpret_cast<MapleRuntime::Uptr>(typeInfo); }
    MapleRuntime::Uptr GetExtensionDataBase() override { return 0; }
    MapleRuntime::U32 GetExtensionDataSize() override { return 0; }
    MapleRuntime::Uptr GetInnerTypeExtensionsBase() override { return 0; }
    MapleRuntime::U32 GetInnerTypeExtensionsSize() override { return 0; }
    MapleRuntime::Uptr GetOuterTypeExtensionsBase() override { return 0; }
    MapleRuntime::U32 GetOuterTypeExtensionsSize() override { return 0; }
    MapleRuntime::Uptr GetStaticGIBase() override { return 0; }
    MapleRuntime::U32 GetStaticGISize() override { return 0; }
    MapleRuntime::Uptr GetTypeInfoBase() override { return reinterpret_cast<MapleRuntime::Uptr>(typeInfo); }
    MapleRuntime::U32 GetTypeInfoTotalSize() override { return sizeof(TypeInfo); }
    MapleRuntime::Uptr GetTypeExtBase() override { return 0; }
    MapleRuntime::U32 GetTypeExtTotalSize() override { return 0; }
    MapleRuntime::CString GetSDKVersion() const override { return MapleRuntime::CString(""); }

private:
    void* handle;
    TypeInfo* typeInfo;
};

[[noreturn]] void Fail(const char* operation)
{
    const char* error = dlerror();
    std::fprintf(stderr, "ABASTRESS_FAIL operation=%s error=%s errno=%d\n", operation,
                 error == nullptr ? "none" : error, errno);
    std::exit(2);
}

Image OpenImage(const char* path)
{
    dlerror();
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        Fail("dlopen");
    }
    auto address = reinterpret_cast<AddressFn>(dlsym(handle, "AbastressTypeInfoAddress"));
    auto storageSize = reinterpret_cast<SizeFn>(dlsym(handle, "AbastressTypeInfoStorageSize"));
    if (address == nullptr || storageSize == nullptr) {
        Fail("dlsym");
    }
    if (storageSize() != sizeof(TypeInfo)) {
        std::fprintf(stderr, "ABASTRESS_FAIL product_typeinfo_size=%zu plugin_storage_size=%zu\n",
                     sizeof(TypeInfo), storageSize());
        std::exit(2);
    }
    TypeInfo* typeInfo = static_cast<TypeInfo*>(address());
    typeInfo->SetUUID(1);
    return { handle, typeInfo };
}

void CloseImage(Image image)
{
    if (dlclose(image.handle) != 0) {
        Fail("dlclose");
    }
}

int RunReuse(const char* firstPath, const char* secondPath, size_t attempts)
{
    TypeInfoManager& manager = TypeInfoManager::GetTypeInfoManager();
    MapleRuntime::CJFileLoader loader;
    for (size_t attempt = 1; attempt <= attempts; ++attempt) {
        Image oldImage = OpenImage(firstPath);
        manager.AddTypeInfo(oldImage.typeInfo);
        if (!manager.ContainsTypeInfo(oldImage.typeInfo)) {
            std::fprintf(stderr, "ABASTRESS_FAIL registration_missing attempt=%zu\n", attempt);
            return 2;
        }
        TypeInfo* oldAddress = oldImage.typeInfo;
        auto* oldFile = new ReuseFile(oldImage);
        loader.AddLoadedFiles(oldFile);
        loader.RemoveLoadedFiles(oldFile);

        Image newImage = OpenImage(secondPath);
        if (newImage.typeInfo == oldAddress) {
            bool containsOld = manager.ContainsTypeInfo(oldAddress);
            std::printf("ADDRESS_REUSE attempt=%zu old=%p new=%p equal=1\n", attempt,
                        static_cast<void*>(oldAddress), static_cast<void*>(newImage.typeInfo));
#ifdef ABASTRESS_NEGATIVE_RUNTIME
            std::printf("NEGATIVE_ARM contains_old_after_reuse=%s expected=true\n",
                        containsOld ? "true" : "false");
            bool passed = containsOld;
#else
            std::printf("POSITIVE_ARM contains_old_after_reuse=%s expected=false\n",
                        containsOld ? "true" : "false");
            bool passed = !containsOld;
#endif
            manager.RemoveTypeInfosInRange(reinterpret_cast<uintptr_t>(oldAddress), sizeof(TypeInfo));
            CloseImage(newImage);
            return passed ? 0 : 1;
        }

        manager.RemoveTypeInfosInRange(reinterpret_cast<uintptr_t>(oldAddress), sizeof(TypeInfo));
        CloseImage(newImage);
        std::swap(firstPath, secondPath);
    }
    std::printf("ADDRESS_REUSE_UNRESOLVED attempts=%zu\n", attempts);
    return 3;
}

size_t ResidentBytes()
{
    FILE* statm = std::fopen("/proc/self/statm", "r");
    if (statm == nullptr) {
        std::perror("fopen(/proc/self/statm)");
        std::exit(2);
    }
    unsigned long totalPages = 0;
    unsigned long residentPages = 0;
    if (std::fscanf(statm, "%lu %lu", &totalPages, &residentPages) != 2) {
        std::fprintf(stderr, "ABASTRESS_FAIL operation=read_statm\n");
        std::exit(2);
    }
    std::fclose(statm);
    return residentPages * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

int RunRss(size_t entries)
{
    if (entries == 0 || entries > SIZE_MAX / sizeof(TypeInfo)) {
        return 2;
    }
    size_t bytes = entries * sizeof(TypeInfo);
    void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        std::perror("mmap");
        return 2;
    }
    TypeInfo* typeInfos = static_cast<TypeInfo*>(mapping);
    for (size_t i = 0; i < entries; ++i) {
        typeInfos[i].SetUUID(1);
    }

    TypeInfoManager& manager = TypeInfoManager::GetTypeInfoManager();
    size_t before = ResidentBytes();
    for (size_t i = 0; i < entries; ++i) {
        manager.AddTypeInfo(&typeInfos[i]);
    }
    size_t after = ResidentBytes();
    auto shape = manager.GetTypeInfoIndexShape();
    size_t delta = after >= before ? after - before : 0;
    double bytesPerEntry = static_cast<double>(delta) / static_cast<double>(shape.first);
    std::printf("RSS_INDEX entries=%zu buckets=%zu rss_before=%zu rss_after=%zu rss_delta=%zu "
                "measured_bytes_per_entry=%.3f\n",
                shape.first, shape.second, before, after, delta, bytesPerEntry);
    return shape.first == entries && delta != 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 5 && std::strcmp(argv[1], "reuse") == 0) {
        return RunReuse(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10));
    }
    if (argc == 3 && std::strcmp(argv[1], "rss") == 0) {
        return RunRss(std::strtoull(argv[2], nullptr, 10));
    }
    std::fprintf(stderr, "usage: %s reuse LIB_A LIB_B ATTEMPTS | rss ENTRIES\n", argv[0]);
    return 2;
}
