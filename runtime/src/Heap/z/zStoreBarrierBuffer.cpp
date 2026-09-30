#include "Heap/z/zGeneration.inline.hpp"
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zStoreBarrierBuffer.hpp"

#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zGeneration.inline.hpp"
#include "Heap/z/zGenerationId.hpp"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zPage.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
StoreBarrierBuffer::StoreBarrierBuffer()
    : lastProcessedColor(::g_cjStoreGoodMask),
      lastInstalledColor(::g_cjStoreGoodMask),
      current(kBufferStoreBarriers ? BufferSizeBytes : 0) {}

void StoreBarrierBuffer::Initialize(uintptr_t color)
{
    lastProcessedColor = color;
    lastInstalledColor = color;
}

void StoreBarrierBuffer::clear()
{
    current = BufferSizeBytes;
}

StoreBarrierBuffer* StoreBarrierBuffer::buffer_for_store(bool heal)
{
    if (heal) {
        return nullptr;
    }
    if (IsGcThread() || Mutator::GetMutator() == nullptr) {
        return nullptr;
    }
    return kBufferStoreBarriers ? ThreadLocal::GetGCData().storeBarrierBuffer : nullptr;
}

void StoreBarrierBuffer::install_base_pointers_inner()
{
    ASSERT(ZPointer::remap_bits(lastInstalledColor) == ZPointer::remap_bits(lastProcessedColor));
    ASSERT((ZPointer::remap_bits(lastProcessedColor) & ZPointerRemappedYoungMask) == 0 ||
           (ZPointer::remap_bits(lastProcessedColor) & ZPointerRemappedOldMask) == 0);

    for (size_t i = Current(); i < kStoreBarrierBufferLength; ++i) {
        const StoreBarrierEntry& entry = buffer[i];
        const zaddress_unsafe pUnsafe = to_zaddress_unsafe(reinterpret_cast<MAddress>(entry.p));
        const zpointer ptr = ZAddress::color(pUnsafe, lastProcessedColor);
        ZGeneration* generation = ZBarrier::remap_generation(ptr);
        ZForwarding* forwarding = generation->forwarding(untype(pUnsafe));
        if (forwarding != nullptr) {
