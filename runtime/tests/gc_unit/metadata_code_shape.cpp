// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include "Loader/ElfUnloadQuiescence.h"
#include "Common/StackType.h"
#include <cstdio>
#include <cstring>
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
// Existence/layout qualification and the consumer assertion have separate
// outputs. INVALID inputs never reach the product read or count as target red.
static int PrefixResult(const char* name, const uint32_t* pc, const void* descriptor,
                        const std::shared_ptr<const ElfUnloadQuiescence::ImageAddressMap>& image,
                        bool hole, bool continuous)
{
    if (!pc || !image) { std::fprintf(stderr, "METADATA_PREFIX_INPUT name=%s qualified=0\n", name); return 67; }
    ElfUnloadQuiescence::ReadScope reader;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pc), prefix = start - sizeof(int32_t);
    size_t sameBytes = 0;
    for (size_t b = 0; b != sizeof(int32_t); ++b) {
        const auto owner = ElfUnloadQuiescence::RegisteredImageForAddress(prefix + b);
        sameBytes += owner == image && image->Contains(prefix + b);
    }
    bool adjacent = false;
    for (size_t i = 1; i < image->ranges.size(); ++i) {
        const auto& left = image->ranges[i - 1];
        const auto& right = image->ranges[i];
        adjacent |= left.start <= prefix && prefix < left.start + left.size &&
            right.start <= start - 1 && start - 1 < right.start + right.size &&
            left.start + left.size == right.start && left.executable != right.executable;
    }
    const bool layout = hole ? (sameBytes == 2 && image->Contains(prefix) && image->Contains(start - 1) &&
        !image->Contains(prefix + 1) && !image->Contains(prefix + 2)) : sameBytes == sizeof(int32_t);
    const bool qualified = layout && image->Contains(start, true) &&
        image->Contains(reinterpret_cast<uintptr_t>(descriptor)) && (!continuous || adjacent);
    std::fprintf(stderr, "METADATA_PREFIX_INPUT name=%s qualified=%d bytes=%zu adjacent=%d\n",
                 name, qualified, sameBytes, adjacent);
    if (!qualified) { return 67; }
    // Hole bytes are readable in the existing ELF fixture's mapped pages;
    // this is an input check, never evidence of registered ownership.
    int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(prefix), sizeof(displacement));
    const bool actualDescriptor = displacement != 0 &&
        prefix + static_cast<intptr_t>(displacement) == reinterpret_cast<uintptr_t>(descriptor);
    std::fprintf(stderr, "METADATA_PREFIX_DESCRIPTOR name=%s qualified=%d\n", name, actualDescriptor);
    if (!actualDescriptor) { return 67; }
    const bool accepted = Resolve(pc, descriptor);
    std::fprintf(stderr, "METADATA_PREFIX_TARGET name=%s accepted=%d expected=%d executed=1\n",
                 name, accepted, !hole);
    return accepted == !hole ? 0 : 1;
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
    int prefixRC = 0;
#ifndef __APPLE__
    prefixRC = PrefixResult("ordinary", pc(0), descriptor(0), ownerMap, false, false);
#endif
#ifdef __linux__
    for (const auto* name : {"contiguous", "hole"}) {
        void* fixture = OpenImage(directory / (std::string("cj_metadata_") + name + suffix));
        auto entry = fixture ? reinterpret_cast<const uint32_t* (*)()>(Symbol(fixture, "A2GetContinuousPC")) : nullptr;
        auto desc = fixture ? reinterpret_cast<Descriptor>(Symbol(fixture, "A2GetOwnerDescriptor")) : nullptr;
        if (!entry || !desc) { std::fprintf(stderr, "METADATA_PREFIX_FIXTURE name=%s qualified=0\n", name); return 67; }
        const auto* input = entry(); // restores fixture page permissions before registration
        const auto registration = ElfUnloadQuiescence::LinkImage(reinterpret_cast<Uptr>(desc(0)));
        const int result = PrefixResult(name, input, desc(0), registration, std::strcmp(name, "hole") == 0,
                                        std::strcmp(name, "contiguous") == 0);
        if (result == 67) { return result; }
        if (result) { prefixRC = result; }
    }
#endif
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
    if (prefixRC == 67) { return prefixRC; }
    return prefixRC == 0 && mapOwner && same && ownsCode && ownsData && !cross && registeredForeign && distinct && !absent && !data ? 0 : 1;
}
