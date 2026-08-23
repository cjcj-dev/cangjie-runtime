// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_REMEMBERED_HOLDER_POLICY_H
#define MRT_REMEMBERED_HOLDER_POLICY_H

namespace MapleRuntime {

// An old liveness snapshot is normally authoritative for pruning remembered
// holders. A root observed by this minor is newer evidence and must win.
inline bool KeepRememberedHolder(bool retainedSnapshotSaysLive, bool isCurrentMinorRoot)
{
    return retainedSnapshotSaysLive || isCurrentMinorRoot;
}

} // namespace MapleRuntime

#endif // MRT_REMEMBERED_HOLDER_POLICY_H
