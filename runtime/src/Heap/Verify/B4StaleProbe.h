// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B4_STALE_PROBE_H
#define MRT_HEAP_B4_STALE_PROBE_H

namespace MapleRuntime {

// Tests the "stale unrepaired base became interior after region reuse" hypothesis
// for B-4 (lane b4stale).
// Gate (default off): MRT_GCV2_B4STALE=1
// Dump cap: MRT_GCV2_B4STALE_DUMP_MAX=<N> (default 64)
//
// T0: for each interior value P seen at scan points, was P a legal object base
//     in a prior pre-evacuate / post-evacuate snapshot?
//   B4S_STALE_CONFIRMED  — P ∈ prior base set
//   B4S_NEVER_VALID_BASE — P never observed as a base
// T1: offset histogram + host object size at interior site
//   B4S_OFFSET_布局相关 | B4S_OFFSET_恒定
class B4StaleProbe {
public:
    static bool Enabled();

    // STW: snapshot all ForEachObj bases (pre-evacuate / post-evacuate).
    static void SnapshotBases(const char* point);

    // STW: walk all live objects' ref fields; classify interiors vs prior bases.
    static void ScanInteriors(const char* point);

    static void FlushSummary(const char* site);
};

} // namespace MapleRuntime

#endif // MRT_HEAP_B4_STALE_PROBE_H
