// Complete linked CJ input; no runtime metadata decoder or lookup copy.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include "Loader/ElfUnloadQuiescence.h"
#ifdef _WIN64
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
namespace MapleRuntime {
class ManagedMetadataFixture {
public:
    ManagedMetadataFixture()
    {
        const char* overridePath = std::getenv("GC_MANAGED_METADATA_FIXTURE");
        std::string path;
        if (overridePath != nullptr) { path = overridePath; }
        else {
            char executable[4096] {};
#ifdef _WIN64
            GC_EXPECT_TRUE(GetModuleFileNameA(nullptr, executable, sizeof(executable)) != 0);
#elif defined(__APPLE__)
            uint32_t length = sizeof(executable);
            GC_EXPECT_EQ(_NSGetExecutablePath(executable, &length), 0);
#else
            GC_EXPECT_TRUE(readlink("/proc/self/exe", executable, sizeof(executable) - 1) > 0);
#endif
            path = executable;
            path.resize(path.find_last_of("/\\") + 1);
#ifdef _WIN64
            path += "cj_managed_metadata.dll";
#elif defined(__APPLE__)
            path += "libcj_managed_metadata.dylib";
#else
            path += "libcj_managed_metadata.so";
#endif
        }
#ifdef _WIN64
        handle = LoadLibraryA(path.c_str());
#else
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        GC_EXPECT_TRUE(handle != nullptr);
        metadata = Address("_CJMetadataStart");
        descriptors = Address("ManagedMetadataDescriptors");
        emptyMap = Address("ManagedMetadataEmptyMap");
        zeroMap = Address("ManagedMetadataZeroMap");
        std::fprintf(stderr, "METADATA_FIXTURE path=%s header=%p descriptor=%p\n", path.c_str(),
                     reinterpret_cast<void*>(metadata), reinterpret_cast<void*>(descriptors));
    }
    const uint32_t* PC(bool descriptor, bool map = false, bool zero = false) const
    {
        return reinterpret_cast<const uint32_t*>(Address(!descriptor ? "ManagedMetadataNative" :
            !map ? "ManagedMetadataNoMap" : zero ? "ManagedMetadataZeroRoots" : "ManagedMetadataEmpty"));
    }
    Uptr Descriptor(bool map = false, bool zero = false) const
    {
#ifdef __APPLE__
        constexpr size_t stride = 56;
#else
        constexpr size_t stride = 48;
#endif
        return descriptors + stride * (!map ? 0 : zero ? 2 : 1);
    }
    void Register() const
    {
        ElfUnloadQuiescence::LinkImage(metadata);
        std::fprintf(stderr, "METADATA_REGISTERED header=%p\n", reinterpret_cast<void*>(metadata));
    }
    // Separate registration for the one authorized fixture knife; all other
    // independently registered inputs retain their valid header as controls.
    void RegisterText() const
    {
        ElfUnloadQuiescence::LinkImage(metadata);
        std::fprintf(stderr, "METADATA_REGISTERED header=%p\n", reinterpret_cast<void*>(metadata));
    }
    Uptr metadata = 0, descriptors = 0, emptyMap = 0, zeroMap = 0;
private:
    Uptr Address(const char* name) const
    {
#ifdef _WIN64
        auto address = GetProcAddress(handle, name);
#else
        auto address = dlsym(handle, name);
#endif
        GC_EXPECT_TRUE(address != nullptr);
        return reinterpret_cast<Uptr>(address);
    }
#ifdef _WIN64
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
    // Deliberately keep the image loaded while the registry retains its owner.
};
}
