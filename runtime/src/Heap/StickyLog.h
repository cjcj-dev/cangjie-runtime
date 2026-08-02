// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_STICKY_LOG_H
#define MRT_STICKY_LOG_H

#include <cstdint>
#include <functional>

#include "Cangjie.h"
#include "Common/TypeDef.h"

namespace MapleRuntime {
extern "C" MRT_EXPORT uint8_t* __cj_sticky_logged_base;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_base;
extern "C" MRT_EXPORT uintptr_t __cj_sticky_heap_size;
extern "C" MRT_EXPORT const uint8_t __cj_sticky_line_shift;
extern "C" MRT_EXPORT void CJ_MCC_StickyLogLine(BaseObject* object);

class MemMap;

class StickyLog {
public:
    static constexpr uint8_t LINE_SHIFT = 8;
    static constexpr size_t LINE_SIZE = static_cast<size_t>(1) << LINE_SHIFT;
    static constexpr size_t DEFAULT_YOUNG_BYTES = 32 * 1024 * 1024;

    static StickyLog& Instance() noexcept;

    void Init(MAddress heapStart, size_t heapSize);
    void Fini() noexcept;
    void ConfigureMinorFromEnvironment();

    void Enable(bool value) { enabled = value; }
    bool IsEnabled() const { return enabled; }
    bool IsMinorEnabled() const { return minorEnabled; }
    bool IsMinorValidatorEnabled() const { return minorValidatorEnabled; }
    bool IsForceSlowPathEnabled() const { return forceSlowPathEnabled; }
    size_t GetYoungBytesThreshold() const { return youngBytesThreshold; }
    uint32_t GetMajorInterval() const { return majorInterval; }
    // Region promotion age: a young region with live bytes ages until
    // youngAge reaches this value, then the whole region is promoted to old
    // (RegionManager::CollectYoungGarbage). Default 1 = shipped behavior
    // (age once, promote on the second surviving minor).
    uint8_t GetPromoteAge() const { return promoteAge; }
    size_t GetEvacuationThreshold() const { return evacuationThreshold; }
    size_t GetEvacuationMaxRegions() const { return evacuationMaxRegions; }
    bool IsLoggedLine(MAddress address) const;
    bool HasLoggedLines() const;
    bool TryLogLine(MAddress address, MAddress& lineStart) const;
    void ClearUnavailableRegion(MAddress regionStart, size_t regionSize);
    void BeginEpoch();

    using LoggedLineVisitor = std::function<bool(MAddress lineStart, MAddress lineEnd)>;
    // TODO: the minor collector will consume logged lines through this interface and rescan objects in each line.
    void RescanLoggedLines(const LoggedLineVisitor& visitor);

private:
    MemMap* loggedMap = nullptr;
    MemMap* dirtyRegionMap = nullptr;
    MAddress heapStart = 0;
    size_t heapSize = 0;
    size_t loggedByteCount = 0;
    size_t dirtyRegionByteCount = 0;
    bool enabled = false;
    bool minorEnabled = false;
    bool minorValidatorEnabled = false;
    bool forceSlowPathEnabled = false;
    size_t youngBytesThreshold = DEFAULT_YOUNG_BYTES;
    uint32_t majorInterval = 8;
    uint8_t promoteAge = 1;
    size_t evacuationThreshold = 0;
    size_t evacuationMaxRegions = 8;
};
} // namespace MapleRuntime

#endif // MRT_STICKY_LOG_H
