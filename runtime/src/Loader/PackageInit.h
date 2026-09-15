// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_PACKAGE_INIT_H
#define MRT_PACKAGE_INIT_H

#include <cstdint>
#include <memory>
#include <vector>

#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {

enum class PackageInitResult : uint32_t {
    Execute = 0, Ready = 1, Failed = 2, Reentrant = 3, Cycle = 4, Unavailable = 5
};
enum class PackageInitFailure : uint32_t { BodyException = 1, OwnerExit = 2, RuntimeFailure = 3 };

struct PackageInitState;

// One directory per BaseFile instance. No reset: completed cache/root state and
// failures belong to this image generation, including across ordinary resets.
class PackageInitTable final {
public:
    PackageInitTable();
    ~PackageInitTable();
    PackageInitTable(const PackageInitTable&) = delete;
    PackageInitTable& operator=(const PackageInitTable&) = delete;

    PackageInitResult Begin(const void* package, const void* unit, uint32_t phase, void** token,
                            std::unique_ptr<ElfUnloadQuiescence::PendingTask> admission);
    static void Complete(void* token) noexcept;
    static void Fail(void* token, uint32_t failure) noexcept;
    static void OwnerExit() noexcept;

private:
    std::vector<std::unique_ptr<PackageInitState>> units;
};

extern "C" {
uint32_t MCC_PackageInitBegin(const void* packageEntry, const void* unitEntry, uint32_t phase, void** ownerToken);
void MCC_PackageInitComplete(void* ownerToken) noexcept;
void MCC_PackageInitFail(void* ownerToken, uint32_t failureCode) noexcept;
[[noreturn]] void MCC_PackageInitAbort(const void* packageEntry, const void* unitEntry,
                                     uint32_t phase, uint32_t beginResult) noexcept;
}
} // namespace MapleRuntime
#endif
