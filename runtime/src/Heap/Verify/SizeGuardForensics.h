// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_SIZE_GUARD_FORENSICS_H
#define MRT_HEAP_SIZE_GUARD_FORENSICS_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

class RegionInfo;
class BaseObject;

// Forensic dump at sizeguard abort path. Default off:
//   MRT_GCV2_SIZEGUARD_FORENSICS=1
// Always dumps maps/hex/dladdr/interior-window before abort when enabled.
// Also triggers B2Ring dump when MRT_GCV2_B2RING=1.
class SizeGuardForensics {
public:
    static bool Enabled();
    static void DumpBeforeAbort(const BaseObject* obj, size_t objSize, const RegionInfo* region,
                                uintptr_t regionStart, uintptr_t regionEnd);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_SIZE_GUARD_FORENSICS_H
