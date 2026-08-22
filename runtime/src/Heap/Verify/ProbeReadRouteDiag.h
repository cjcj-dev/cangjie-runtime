// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_PROBE_READ_ROUTE_DIAG_H
#define MRT_PROBE_READ_ROUTE_DIAG_H

#include <cstdint>

#include "Common/TypeDef.h"

namespace MapleRuntime {
namespace ProbeReadRouteDiag {
struct Snapshot {
    bool armed { false };
    MAddress expectedFrom { 0 };
    uint64_t queries { 0 };
    uint64_t answers { 0 };
    MAddress from { 0 };
    MAddress to { 0 };
};

inline Snapshot& Current()
{
    static thread_local Snapshot snapshot;
    return snapshot;
}

inline void Arm()
{
    Current() = {};
    Current().armed = true;
}

inline void NoteRetiredLookup(MAddress from, MAddress to)
{
    Snapshot& snapshot = Current();
    if (!snapshot.armed || snapshot.expectedFrom == 0 || from != snapshot.expectedFrom) {
        return;
    }
    ++snapshot.queries;
    if (to != 0) {
        ++snapshot.answers;
        if (snapshot.to == 0) {
            snapshot.from = from;
            snapshot.to = to;
        }
    }
}

inline void ExpectFrom(MAddress from)
{
    Current().expectedFrom = from;
}

inline Snapshot Take()
{
    Snapshot snapshot = Current();
    Current() = {};
    return snapshot;
}
} // namespace ProbeReadRouteDiag
} // namespace MapleRuntime

#endif // MRT_PROBE_READ_ROUTE_DIAG_H
