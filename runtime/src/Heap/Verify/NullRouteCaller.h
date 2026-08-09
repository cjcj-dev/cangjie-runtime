// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_NULL_ROUTE_CALLER_H
#define MRT_NULL_ROUTE_CALLER_H

// deadedge / routecaller / slotdelta: TLS tags for ForwardObject → GetRoute null-route diag.
// Gate: MRT_GCV2_NULLROUTE_DIAG=1 (same as RegionInfo::GetRoute sample).
// Default off: tags written only when diag is armed; readers print "none"/0 if unset.
//
// edgeSrc distinguishes FixMinor enumeration faces:
//   remset  — remembered-set slot walk
//   liveobj — FixMinorObjectSlots over reachableVec survivors
//   root    — FixMinorRootSlots (stack/static/export/…)
// host/slot answer 甲′/乙′: is the *slot itself* live (host marked)?
//
// slotdelta: markHost is the from-space identity used in T2 snap membership;
// walkBase is the object whose fields are being iterated (may be to-version after
// ForwardObject). fieldOffset = slot - walkBase aligns T2 (from) and T4 (to) faces.

#include <cstdint>

namespace MapleRuntime {
namespace NullRouteCaller {

inline thread_local const char* g_tag = nullptr;
inline thread_local const char* g_edgeSrc = nullptr;
inline thread_local uintptr_t g_slot = 0;
inline thread_local void* g_host = nullptr;
inline thread_local void* g_walkBase = nullptr;

class ScopedTag {
public:
    explicit ScopedTag(const char* tag) : prev_(g_tag)
    {
        g_tag = tag;
    }
    ~ScopedTag() { g_tag = prev_; }
    ScopedTag(const ScopedTag&) = delete;
    ScopedTag& operator=(const ScopedTag&) = delete;

private:
    const char* prev_;
};

// Sets edge provenance + optional host object + slot address for null-route samples.
// markHost: from-space identity for snap membership (reachableVec key).
// walkBase: object being field-iterated (to-version after forward); may equal markHost.
// slot: address of the RefField being fixed.
class ScopedEdge {
public:
    ScopedEdge(const char* edgeSrc, void* markHost, uintptr_t slot)
        : ScopedEdge(edgeSrc, markHost, markHost, slot)
    {
    }
    ScopedEdge(const char* edgeSrc, void* markHost, void* walkBase, uintptr_t slot)
        : prevSrc_(g_edgeSrc), prevHost_(g_host), prevWalk_(g_walkBase), prevSlot_(g_slot)
    {
        g_edgeSrc = edgeSrc;
        g_host = markHost;
        g_walkBase = walkBase;
        g_slot = slot;
    }
    ~ScopedEdge()
    {
        g_edgeSrc = prevSrc_;
        g_host = prevHost_;
        g_walkBase = prevWalk_;
        g_slot = prevSlot_;
    }
    ScopedEdge(const ScopedEdge&) = delete;
    ScopedEdge& operator=(const ScopedEdge&) = delete;

private:
    const char* prevSrc_;
    void* prevHost_;
    void* prevWalk_;
    uintptr_t prevSlot_;
};

inline const char* Current()
{
    return g_tag != nullptr ? g_tag : "none";
}

inline const char* EdgeSrc()
{
    return g_edgeSrc != nullptr ? g_edgeSrc : "none";
}

inline uintptr_t Slot()
{
    return g_slot;
}

inline void* Host()
{
    return g_host;
}

inline void* WalkBase()
{
    return g_walkBase;
}

// Field offset relative to walkBase (0 if unknown). Aligns T2 from-face with T4 to-face.
inline size_t FieldOffset()
{
    if (g_walkBase == nullptr || g_slot == 0) {
        return 0;
    }
    return static_cast<size_t>(g_slot - reinterpret_cast<uintptr_t>(g_walkBase));
}

} // namespace NullRouteCaller
} // namespace MapleRuntime

#endif // MRT_NULL_ROUTE_CALLER_H
