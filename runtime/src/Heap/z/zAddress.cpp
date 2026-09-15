// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "Heap/z/zAddress.hpp"
#include "Base/Macros.h"
#include "CangjieRuntime.h"

extern "C" {
MRT_EXPORT unsigned long g_cjLoadGoodMask;
MRT_EXPORT unsigned long g_cjLoadBadMask;
MRT_EXPORT unsigned long g_cjMarkGoodMask;
MRT_EXPORT unsigned long g_cjMarkBadMask;
MRT_EXPORT unsigned long g_cjStoreGoodMask;
MRT_EXPORT unsigned long g_cjStoreBadMask;
MRT_EXPORT size_t g_cjLoadShift;
}
namespace MapleRuntime {
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
    g_cjLoadGoodMask = ZPointer::remap_bits(ZPointerRemapped);
    g_cjMarkGoodMask = g_cjLoadGoodMask | ZPointerMarkedYoung | ZPointerMarkedOld;
    g_cjStoreGoodMask = g_cjMarkGoodMask | ZPointerRemembered;
    g_cjLoadBadMask = g_cjLoadGoodMask ^ ZPointerLoadMetadataMask;
    g_cjMarkBadMask = g_cjMarkGoodMask ^ ZPointerMarkMetadataMask;
    g_cjStoreBadMask = g_cjStoreGoodMask ^ ZPointerStoreMetadataMask;
    pd_set_good_masks();
}
void ZGlobalsPointers::pd_set_good_masks()
{
    g_cjLoadShift = ZPointer::load_shift_lookup(g_cjLoadGoodMask);
}
size_t ZGlobalsPointers::min_address_offset_request()
{
    const size_t heapBytes = CangjieRuntime::GetHeapParam().heapSize * size_t(1024);
    size_t request = 1;
    while (request < heapBytes && request < (uintptr_t(1) << 44)) { request <<= 1; }
    return request;
}
void ZGlobalsPointers::initialize()
{
    const size_t request = min_address_offset_request();
    size_t bits = 0;
    while ((uintptr_t(1) << bits) < request) { ++bits; }
    ZAddressOffsetBits = bits < 42 ? 42 : bits > 44 ? 44 : bits;
    ZAddressOffsetMax = uintptr_t(1) << ZAddressOffsetBits;
    ZAddressOffsetMask = ZAddressOffsetMax - 1;
    ZAddressHeapBaseShift = ZAddressOffsetBits;
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
