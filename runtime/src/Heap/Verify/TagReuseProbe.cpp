// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TagReuseProbe.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Heap/Allocator/RegionInfo.h"

namespace MapleRuntime {
namespace {

#define STICKY_LOG(fmt, ...)                                                                                           \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][mark-bits-sticky] " fmt "\n", ##__VA_ARGS__);                                      \
        std::fflush(stderr);                                                                                           \
    } while (0)

std::atomic<uint64_t> gMarkStickyN{0};
std::atomic<uint64_t> gMarkStickyFail{0};
std::atomic<uint64_t> gMarkStickyOk{0};

} // namespace

bool TagReuseProbe::MarkBitsStickyEnabled()
{
    static const bool on = false /* pinned:MRT_GCV2_MARK_BITS_STICKY */;
    return on;
}

bool TagReuseProbe::NoteMarkBitsSticky(RegionInfo* region, size_t offset, bool /*expectMarked*/, const char* site)
{
    (void)region;
    (void)offset;
    (void)site;
    return true;
}

bool TagReuseProbe::NoteMarkBitsSticky(RegionInfo* region, size_t offset, bool /*expectMarked*/, const char* site,
                                       Generation generation)
{
    if (!MarkBitsStickyEnabled() || region == nullptr) {
        return true;
    }
    static std::atomic<bool> armedLogged{false};
    if (!armedLogged.exchange(true, std::memory_order_relaxed)) {
        STICKY_LOG("ARMED env=MRT_GCV2_MARK_BITS_STICKY=1 site=%s", site);
    }
    gMarkStickyN.fetch_add(1, std::memory_order_relaxed);
    bool nowMarked = generation == Generation::Young
        ? region->IsMarkedObject(region->GetMarkView<Generation::Young>(), offset)
        : region->IsMarkedObject(region->GetMarkView<Generation::Old>(), offset);
    if (!nowMarked) {
        gMarkStickyFail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    gMarkStickyOk.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace MapleRuntime
