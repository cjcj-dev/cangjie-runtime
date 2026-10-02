// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Loader/ElfUnloadQuiescence.h"
#include "Common/StackType.h"
#include <cstdio>
#include <filesystem>
#ifdef _WIN64
#include <windows.h>
#else
#include <dlfcn.h>
#endif
using namespace MapleRuntime;
static void* OpenImage(const std::filesystem::path& path)
{
#ifdef _WIN64
    return LoadLibraryW(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
static void* Symbol(void* image, const char* name)
{
#ifdef _WIN64
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(image), name));
#else
    return dlsym(image, name);
#endif
}
static bool Resolve(const uint32_t* pc, const void* descriptor)
{
    struct Input { uintptr_t descriptor; ArchUInt start; FrameAddress frame; } input {};
    input.descriptor = reinterpret_cast<uintptr_t>(descriptor);
#if defined(__x86_64__)
    input.start = reinterpret_cast<ArchUInt>(pc) + 9;
#elif defined(__arm__)
    input.start = reinterpret_cast<ArchUInt>(pc) + 12;
#else
    input.start = reinterpret_cast<ArchUInt>(pc);
#endif
    FrameInfo frame(pc);
    frame.mFrame.SetFA(&input.frame);
    frame.mFrame.SetIP(pc + 1);
    return frame.ResolveProcInfo();
}
int main(int argc, char** argv)
{
    if (argc != 1) { return 64; }
    const auto directory = std::filesystem::absolute(argv[0]).parent_path();
#ifdef _WIN64
    const char* suffix = ".dll";
#elif defined(__APPLE__)
    const char* suffix = ".dylib";
#else
    const char* suffix = ".so";
#endif
    void* owner = OpenImage(directory / (std::string("cj_metadata_owner") + suffix));
    void* foreign = OpenImage(directory / (std::string("cj_metadata_foreign") + suffix));
    if (!owner || !foreign) { std::fprintf(stderr, "METADATA_OWNER_INPUT_MISSING\n"); return 65; }
    using PC = const uint32_t* (*)(size_t);
    using DataPC = const uint32_t* (*)();
    using Descriptor = const void* (*)(size_t);
    using ForeignDescriptor = const void* (*)();
    using Setter = bool (*)(const void*);
    auto pc = reinterpret_cast<PC>(Symbol(owner, "A2GetOwnerPC"));
    auto dataPC = reinterpret_cast<DataPC>(Symbol(owner, "A2GetDataPC"));
    auto descriptor = reinterpret_cast<Descriptor>(Symbol(owner, "A2GetOwnerDescriptor"));
    auto foreignDescriptor = reinterpret_cast<ForeignDescriptor>(Symbol(foreign, "A2GetForeignDescriptor"));
    auto setter = reinterpret_cast<Setter>(Symbol(owner, "A2SetForeignDescriptor"));
    if (!pc || !dataPC || !descriptor || !foreignDescriptor || !setter || !setter(foreignDescriptor())) {
        std::fprintf(stderr, "METADATA_OWNER_INPUT_NOT_ADMITTED\n"); return 66;
    }
    const auto ownerMap = ElfUnloadQuiescence::LinkImage(reinterpret_cast<Uptr>(descriptor(0)));
    const auto foreignMap = ElfUnloadQuiescence::LinkImage(reinterpret_cast<Uptr>(foreignDescriptor()));
    ElfUnloadQuiescence::ReadScope reader;
    const bool same = Resolve(pc(0), descriptor(0));
    const bool cross = Resolve(pc(1), foreignDescriptor());
    const bool absent = Resolve(pc(2), nullptr);
    const bool data = Resolve(dataPC(), descriptor(0));
    const bool ownsCode = ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<Uptr>(pc(0)), true);
    const bool ownsData = ElfUnloadQuiescence::IsLinkedAddress(reinterpret_cast<Uptr>(dataPC()));
    const bool registeredForeign = foreignMap->Contains(reinterpret_cast<Uptr>(foreignDescriptor()));
    const bool distinct = ownerMap->identity != foreignMap->identity;
    std::fprintf(stderr, "METADATA_SAME_OWNER_TARGET same=%d code=%d data_member=%d executed=1\n", same, ownsCode, ownsData);
    std::fprintf(stderr, "METADATA_CROSS_OWNER_TARGET cross=%d foreign_registered=%d distinct=%d executed=1\n",
                 cross, registeredForeign, distinct);
    std::fprintf(stderr, "METADATA_ABSENT_TARGET absent=%d executed=1\n", absent);
    std::fprintf(stderr, "METADATA_CODE_ONLY_TARGET data=%d executed=1\n", data);
    bool mapOwner = true;
#ifdef __APPLE__
    const auto mappedSame = ElfUnloadQuiescence::FindFunctionDescriptor(reinterpret_cast<Uptr>(pc(0)));
    const auto mappedCross = ElfUnloadQuiescence::FindFunctionDescriptor(reinterpret_cast<Uptr>(pc(1)));
    const auto mappedData = ElfUnloadQuiescence::FindFunctionDescriptor(reinterpret_cast<Uptr>(dataPC()));
    std::fprintf(stderr, "METADATA_MAP_PC_OWNER pass=%d executed=1\n", mappedData == 0);
    std::fprintf(stderr, "METADATA_MAP_DESC_OWNER pass=%d executed=1\n", mappedCross == 0);
    std::fprintf(stderr, "METADATA_MAP_SAME_OWNER pass=%d executed=1\n", mappedSame == reinterpret_cast<Uptr>(descriptor(0)));
    mapOwner = mappedData == 0 && mappedCross == 0 && mappedSame == reinterpret_cast<Uptr>(descriptor(0));
#endif
    return mapOwner && same && ownsCode && ownsData && !cross && registeredForeign && distinct && !absent && !data ? 0 : 1;
}
