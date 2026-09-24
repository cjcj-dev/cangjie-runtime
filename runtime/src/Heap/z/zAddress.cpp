#include "Heap/z/zHeuristics.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "Heap/z/zAddress.hpp"
#include "Heap/z/zThreadLocalData.hpp"
#include "Base/Macros.h"
#include "CangjieRuntime.h"
#include "Heap/z/zGlobals.hpp"
#include "Heap/z/zNUMA.inline.hpp"
#include <algorithm>
#include <limits>
#if defined(__aarch64__) && defined(__linux__)
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
MRT_EXPORT uintptr_t g_cjLoadGoodMask;
MRT_EXPORT uintptr_t g_cjLoadBadMask;
MRT_EXPORT uintptr_t g_cjMarkGoodMask;
MRT_EXPORT uintptr_t g_cjMarkBadMask;
MRT_EXPORT uintptr_t g_cjStoreGoodMask;
MRT_EXPORT uintptr_t g_cjStoreBadMask;
MRT_EXPORT size_t g_cjLoadShift;
MRT_EXPORT uintptr_t g_cjHeapStart;
MRT_EXPORT uintptr_t g_cjHeapEnd;
MRT_EXPORT uintptr_t g_cjHeapRangeCount;
MRT_EXPORT uintptr_t g_cjHeapRangeStart[kCjHeapRangeCap];
MRT_EXPORT uintptr_t g_cjHeapRangeEnd[kCjHeapRangeCap];
}
namespace MapleRuntime {
static_assert(kCjHeapRangeCap == ZMaxVirtualReservations, "compiler reservation table must cover the reserver");
uintptr_t ZAddressHeapBase;
uintptr_t ZAddressHeapBaseShift;
size_t ZAddressOffsetBits;
uintptr_t ZAddressOffsetMask;
size_t ZAddressOffsetMax;
size_t ZBackingOffsetMax;
uint32_t ZBackingIndexMax;
uintptr_t ZPointerRemapped;
uintptr_t ZPointerRemappedYoungMask;
uintptr_t ZPointerRemappedOldMask;
uintptr_t ZPointerMarkedYoung;
uintptr_t ZPointerMarkedOld;
uintptr_t ZPointerFinalizable;
uintptr_t ZPointerRemembered;

static uint32_t* ZPointerCalculateStoreGoodMaskLowOrderBitsAddr()
{
    auto* address = reinterpret_cast<unsigned char*>(&g_cjStoreGoodMask);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    address += sizeof(uintptr_t) - sizeof(uint32_t);
#endif
    return reinterpret_cast<uint32_t*>(address);
}
uint32_t* ZPointerStoreGoodMaskLowOrderBitsAddr = ZPointerCalculateStoreGoodMaskLowOrderBitsAddr();

// ZGC zAddress.cpp:78-94: one publication point for every mask.
void ZGlobalsPointers::set_good_masks()
{
    ZPointerRemapped = ZPointerRemappedOldMask & ZPointerRemappedYoungMask;
    ZPointerLoadGoodMask = ZPointer::remap_bits(ZPointerRemapped);
    ZPointerMarkGoodMask = ZPointerLoadGoodMask | ZPointerMarkedYoung | ZPointerMarkedOld;
    ZPointerStoreGoodMask = ZPointerMarkGoodMask | ZPointerRemembered;
    ZPointerLoadBadMask = ZPointerLoadGoodMask ^ ZPointerLoadMetadataMask;
    ZPointerMarkBadMask = ZPointerMarkGoodMask ^ ZPointerMarkMetadataMask;
    ZPointerStoreBadMask = ZPointerStoreGoodMask ^ ZPointerStoreMetadataMask;
    pd_set_good_masks();
    ThreadGCData::PublishMasks({ZPointerLoadGoodMask, ZPointerLoadBadMask,
                               ZPointerMarkBadMask, ZPointerStoreGoodMask, ZPointerStoreBadMask});
}
void ZGlobalsPointers::pd_set_good_masks()
{
    g_cjLoadShift = ZPointer::load_shift_lookup(ZPointerLoadGoodMask);
}
#ifdef __aarch64__
// ZGC zAddress_aarch64.cpp:41-91.
static const size_t DEFAULT_MAX_ADDRESS_BIT = 46;
static const size_t MINIMUM_MAX_ADDRESS_BIT = 36;
static size_t probe_valid_max_address_bit()
{
#ifdef __linux__
    size_t maxAddressBit = 0;
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    for (size_t bit = DEFAULT_MAX_ADDRESS_BIT; bit > MINIMUM_MAX_ADDRESS_BIT; --bit) {
        const uintptr_t base = uintptr_t(1) << bit;
        if (msync(reinterpret_cast<void*>(base), pageSize, MS_ASYNC) == 0) {
            maxAddressBit = bit;
            break;
        }
        if (errno != ENOMEM) {
            assert(errno == ENOMEM);
            continue;
        }
        void* result = mmap(reinterpret_cast<void*>(base), pageSize, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (result != MAP_FAILED) { munmap(result, pageSize); }
        if (reinterpret_cast<uintptr_t>(result) == base) {
            maxAddressBit = bit;
            break;
        }
    }
    if (maxAddressBit == 0) {
        void* result = mmap(reinterpret_cast<void*>(uintptr_t(1) << DEFAULT_MAX_ADDRESS_BIT),
                            pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (result != MAP_FAILED) {
            maxAddressBit = 63 - __builtin_clzll(reinterpret_cast<uintptr_t>(result));
            munmap(result, pageSize);
        }
    }
    return std::max(maxAddressBit, MINIMUM_MAX_ADDRESS_BIT);
#else
    return DEFAULT_MAX_ADDRESS_BIT;
#endif
}
#endif

size_t ZPlatformAddressOffsetBits()
{
#if defined(CANGJIE_ASAN_SUPPORT) && !defined(__aarch64__)
    return 44;
#else
#ifdef __aarch64__
    static const size_t validMaxAddressBits = probe_valid_max_address_bit() + 1;
    const size_t maxBits = validMaxAddressBits - 3;
    const size_t minBits = maxBits - 2;
#else
    const size_t minBits = 42;
    const size_t maxBits = 44;
#endif
    const size_t request = ZGlobalsPointers::min_address_offset_request();
    const size_t bits = 63 - __builtin_clzll(request);
    return std::min(std::max(bits, minBits), maxBits);
#endif
}
size_t ZPlatformAddressHeapBaseShift()
{
    return ZPlatformAddressOffsetBits();
}
size_t ZGlobalsPointers::min_address_offset_request()
{
    // A standalone native fixture has no Runtime instance; its configuration
    // starts at the same minimum address domain as an empty heap request.
    const size_t heapBytes = Runtime::CurrentRef() == nullptr ? 0
        : ZHeuristics::max_heap_size();
    const size_t multiplier = ZVirtualToPhysicalRatio *
        (NumaTopology::SealProcessTopology().Count() > 1 ? 2 : 1);
    CHECK(heapBytes <= (std::numeric_limits<size_t>::max() >> 1) / multiplier);
    const size_t desired = heapBytes * multiplier;
    size_t request = 1;
    while (request < desired) { request <<= 1; }
    return request;
}
void ZGlobalsPointers::initialize()
{
    ZAddressOffsetBits = ZPlatformAddressOffsetBits();
    ZAddressOffsetMax = uintptr_t(1) << ZAddressOffsetBits;
    ZAddressOffsetMask = ZAddressOffsetMax - 1;
    ZAddressHeapBaseShift = ZPlatformAddressHeapBaseShift();
    ZAddressHeapBase = uintptr_t(1) << ZAddressHeapBaseShift;
    ZPointerRemappedYoungMask = ZPointerRemapped10 | ZPointerRemapped00;
    ZPointerRemappedOldMask = ZPointerRemapped01 | ZPointerRemapped00;
    ZPointerMarkedYoung = ZPointerMarkedYoung0;
    ZPointerMarkedOld = ZPointerMarkedOld0;
    ZPointerFinalizable = ZPointerFinalizable0;
    ZPointerRemembered = ZPointerRemembered0;
    set_good_masks();
}
void ZGlobalsPointers::flip_young_mark_start()
{
    ZPointerMarkedYoung ^= ZPointerMarkedYoung0 | ZPointerMarkedYoung1;
    ZPointerRemembered ^= ZPointerRemembered0 | ZPointerRemembered1;
    set_good_masks();
}
void ZGlobalsPointers::flip_young_relocate_start()
{
    ZPointerRemappedYoungMask ^= ZPointerRemappedMask;
    set_good_masks();
}
void ZGlobalsPointers::flip_old_mark_start()
{
    ZPointerMarkedOld ^= ZPointerMarkedOld0 | ZPointerMarkedOld1;
    ZPointerFinalizable ^= ZPointerFinalizable0 | ZPointerFinalizable1;
    set_good_masks();
}
void ZGlobalsPointers::flip_old_relocate_start()
{
    ZPointerRemappedOldMask ^= ZPointerRemappedMask;
    set_good_masks();
}
} // namespace MapleRuntime
