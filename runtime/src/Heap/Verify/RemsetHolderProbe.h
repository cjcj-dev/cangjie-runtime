// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_REMSET_HOLDER_PROBE_H
#define MRT_HEAP_REMSET_HOLDER_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Resolve remset slot → holder object H and classify the edge (甲/乙/丙).
// Gate (default off): MRT_GCV2_REMSET_HOLDER=1
// Optional dump cap:  MRT_GCV2_REMSET_HOLDER_DUMP_MAX=<N> (default 64)
//
// Called from RescanRememberedSet immediately before PushYoungObject(..., "remset").
// Does not relax IsValidObject / sizeguard; diagnostics only.
class RemsetHolderProbe {
public:
    static bool Enabled();

    // slot = &field recorded in remset; value = object address about to be pushed.
    static void NoteRemsetEdge(uintptr_t slot, void* value);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_REMSET_HOLDER_PROBE_H
