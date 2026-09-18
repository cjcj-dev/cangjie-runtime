// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/z/zStoreBarrierBuffer.hpp"

#include "Heap/z/zForwardingTable.hpp"
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
    : current(kStoreBarrierBufferLength),
      lastProcessedColor(::g_cjStoreGoodMask),
      lastInstalledColor(::g_cjStoreGoodMask) {}

void StoreBarrierBuffer::Initialize(uintptr_t color)
{
    lastProcessedColor = color;
    lastInstalledColor = color;
}

void StoreBarrierBuffer::clear()
{
    current = kStoreBarrierBufferLength;
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
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        const StoreBarrierEntry& entry = buffer[i];
        const zaddress_unsafe pUnsafe = to_zaddress_unsafe(entry.p);
        const zpointer ptr = ZAddress::color(pUnsafe, lastProcessedColor);
        ZGeneration* generation = ZBarrier::remap_generation(ptr);
        const Generation gen =
            (generation == &Heap::GetHeap().GetZGeneration(ZGenerationId::young))
                ? Generation::Young
                : Generation::Old;
        ZForwarding* forwarding = (entry.p == 0) ? nullptr : generation_forwarding_table(gen).get(entry.p);
        if (forwarding != nullptr && forwarding->page() != nullptr) {
            basePointers[i] = to_zaddress_unsafe(forwarding->page()->find_base(entry.p));
        } else {
            basePointers[i] = zaddress_unsafe::null;
        }
    }
}

void StoreBarrierBuffer::install_base_pointers()
{
    if (!kBufferStoreBarriers) {
        return;
    }
    std::lock_guard<std::mutex> locker(basePointerLock);
    if (ZPointer::remap_bits(lastInstalledColor) != ZPointerRemapped) {
        install_base_pointers_inner();
    }
    lastInstalledColor = ::g_cjStoreGoodMask;
}

static MAddress RemapBufferedField(MAddress p, zaddress_unsafe pBase, uintptr_t color)
{
    const uintptr_t offset = p - untype(pBase);
    const zpointer colored = ZAddress::color(pBase, color);
    const zaddress remapped = ZBarrier::make_load_good(colored);
    return untype(remapped) + offset;
}

void StoreBarrierBuffer::on_new_phase_relocate(size_t i)
{
    if (ZPointer::remap_bits(lastProcessedColor) == ZPointerRemapped) {
        return;
    }
    const zaddress_unsafe pBase = basePointers[i];
    if (is_null(pBase)) {
        return;
    }
    buffer[i].p = RemapBufferedField(buffer[i].p, pBase, lastProcessedColor);
}

void StoreBarrierBuffer::on_new_phase_remember(size_t i)
{
    const MAddress p = buffer[i].p;
    if (!Heap::IsHeapAddress(p) || Heap::page(p)->IsYoungRegion()) {
        return;
    }
    const uintptr_t lastMarkYoung = lastProcessedColor & (ZPointerMarkedYoung0 | ZPointerMarkedYoung1);
    if (lastMarkYoung != ZPointerMarkedYoung) {
        (void)ZBarrier::RemsetBarrierOnOopField(HeapSlotAt<>(p));
    } else {
        ZBarrier::remember(reinterpret_cast<volatile zpointer*>(p));
    }
}

bool StoreBarrierBuffer::is_old_mark() const
{
    return Heap::GetHeap().GetZGeneration(ZGenerationId::old).IsPhaseMark();
}

bool StoreBarrierBuffer::stored_during_old_mark() const
{
    const uintptr_t lastMarkOld = lastProcessedColor & (ZPointerMarkedOld0 | ZPointerMarkedOld1);
    return lastMarkOld == ZPointerMarkedOld;
}

void StoreBarrierBuffer::on_new_phase_mark(size_t i)
{
    const StoreBarrierEntry& entry = buffer[i];
    if (is_null_any(entry.prev)) {
        return;
    }
    const MAddress p = entry.p;
    if (is_old_mark() && stored_during_old_mark() && Heap::IsHeapAddress(p) &&
        !Heap::page(p)->IsYoungRegion()) {
        const zaddress addr = ZBarrier::make_load_good(entry.prev);
        Heap::GetHeap().MarkObjectIfActive(to_object(addr));
    }
}

void StoreBarrierBuffer::on_new_phase()
{
    if (!kBufferStoreBarriers) {
        return;
    }
    install_base_pointers();
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        on_new_phase_relocate(i);
        on_new_phase_remember(i);
        on_new_phase_mark(i);
    }
    clear();
    lastProcessedColor = ::g_cjStoreGoodMask;
}

void StoreBarrierBuffer::Flush()
{
    Collector& collector = Heap::GetHeap().GetCollector();
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        const StoreBarrierEntry& entry = buffer[i];
        const zaddress addr = ZBarrier::make_load_good(entry.prev);
        if (!is_null(addr)) {
            collector.MarkObjectIfActive(to_object(addr));
        }
        ZBarrier::remember(reinterpret_cast<volatile zpointer*>(entry.p));
        buffer[i] = {};
    }
    clear();
}

bool StoreBarrierBuffer::is_in(MAddress p) const
{
    const uintptr_t lastRemap = ZPointer::remap_bits(lastProcessedColor);
    const bool needsRemap = lastRemap != ZPointerRemapped;
    for (size_t i = current; i < kStoreBarrierBufferLength; ++i) {
        MAddress entryP = buffer[i].p;
        if (needsRemap && !is_null(basePointers[i])) {
            entryP = RemapBufferedField(entryP, basePointers[i], lastProcessedColor);
        }
        if (entryP == p) {
            return true;
        }
    }
    return false;
}

} // namespace MapleRuntime

#include "Heap/z/zStoreBarrierBuffer.inline.hpp"

namespace MapleRuntime {
bool StoreBarrierBuffer::IsEmpty() const { return current == kStoreBarrierBufferLength; }
}
