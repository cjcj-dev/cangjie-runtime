// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_NULL_ROUTE_CALLER_H
#define MRT_NULL_ROUTE_CALLER_H

// nullroute / iorsource / alotior: TLS tags for ForwardObject → GetRoute null-route diag.
// Gate: MRT_GCV2_NULLROUTE_DIAG=1 (same as RegionInfo::GetRoute sample + AllocPhaseDiag).
//
// ⭐ Zero cost when off: ScopedTag/ScopedEdge constructors return before any TLS write.
//    (Rejected un-gated form wrote 6 TLS slots in FixMinorEvacuatedSlot hot loops.)
//    Pattern matches idlewrite 7f37316e / MarkWhyProbe 3a1310fc: gate before work.
//
// edgeSrc distinguishes FixMinor enumeration faces:
//   remset  — remembered-set slot walk
//   liveobj — FixMinorObjectSlots over reachableVec survivors
//   root    — FixMinorRootSlots (stack/static/export/…)
// host/slot answer host-vs-slot liveness on the null-route sample line.

#include <cstdint>
#include <cstdlib>

namespace MapleRuntime {
namespace NullRouteCaller {

inline bool Enabled()
{
    return false;
}

inline thread_local const char* g_tag = nullptr;
inline thread_local const char* g_edgeSrc = nullptr;
inline thread_local uintptr_t g_slot = 0;
inline thread_local void* g_host = nullptr;

class ScopedTag {
public:
    explicit ScopedTag(const char* tag) : active_(Enabled()), prev_(nullptr)
    {
        if (!active_) {
            return;
        }
        prev_ = g_tag;
        g_tag = tag;
    }
    ~ScopedTag()
    {
        if (!active_) {
            return;
        }
        g_tag = prev_;
    }
    ScopedTag(const ScopedTag&) = delete;
    ScopedTag& operator=(const ScopedTag&) = delete;

private:
    bool active_;
    const char* prev_;
};

// Sets edge provenance + optional host object + slot address for null-route samples.
class ScopedEdge {
public:
    ScopedEdge(const char* edgeSrc, void* host, uintptr_t slot)
        : active_(Enabled()), prevSrc_(nullptr), prevHost_(nullptr), prevSlot_(0)
    {
        if (!active_) {
            return;
        }
        prevSrc_ = g_edgeSrc;
        prevHost_ = g_host;
        prevSlot_ = g_slot;
        g_edgeSrc = edgeSrc;
        g_host = host;
        g_slot = slot;
    }
    ~ScopedEdge()
    {
        if (!active_) {
            return;
        }
        g_edgeSrc = prevSrc_;
        g_host = prevHost_;
        g_slot = prevSlot_;
    }
    ScopedEdge(const ScopedEdge&) = delete;
    ScopedEdge& operator=(const ScopedEdge&) = delete;

private:
    bool active_;
    const char* prevSrc_;
    void* prevHost_;
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

} // namespace NullRouteCaller
} // namespace MapleRuntime

#endif // MRT_NULL_ROUTE_CALLER_H
