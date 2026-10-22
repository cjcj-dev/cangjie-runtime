// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "gc_verify_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Heap/z/zForwarding.inline.hpp"
#include "Heap/z/zForwardingAllocator.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Mutator/Mutator.h"
#include "ObjectModel/RefField.inline.h"
#include <cstdio>
using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
void SetRootWord(RootSlot& slot, uintptr_t word) { std::memcpy(&slot, &word, sizeof(word)); }

void ProcessUncolored(ObjectRef& slot, uintptr_t color)
{
    ZUncoloredRootProcessOopClosure closure(color);
    static_cast<OopClosure&>(closure).do_oop(&HeapSlotAt<>(static_cast<void*>(&slot)));
}

uintptr_t LoadBadColor()
{
    const uintptr_t saved = ZPointerLoadGoodMask;
    const auto colored = [&]() { return ZAddress::color(zaddress::null, saved); };
    if (ZPointer::is_load_good(colored())) {
        ZGlobalsPointers::flip_young_relocate_start();
    }
    if (ZPointer::is_load_good(colored())) {
        ZGlobalsPointers::flip_old_relocate_start();
    }
    GC_EXPECT_TRUE(!ZPointer::is_load_good(colored()));
    GC_EXPECT_FALSE(is_null(colored()));
    return saved;
}

struct PageForwarding {
    ZForwardingAllocator allocator;
    ZGeneration* generation = nullptr;
    ZForwarding* forwarding = nullptr;
    bool inserted = false;
    PageForwarding() : allocator(static_cast<size_t>(1) << 20) {}
    ~PageForwarding()
    {
        if (inserted) {
            generation->forwarding_table().remove(forwarding);
        }
        if (forwarding != nullptr) {
            forwarding->~ZForwarding();
        }
    }
    PageForwarding(const PageForwarding&) = delete;
    PageForwarding& operator=(const PageForwarding&) = delete;
};

BaseObject* InstallInPlace(PageForwarding& installed, GcVerifyFixture& fixture, uintptr_t color, bool selfMap)
{
    (*ZGeneration::young()).set_phase(ZGenerationPhase::Relocate);
    (*ZGeneration::old()).set_phase(ZGenerationPhase::Relocate);
    ZPage* page = fixture.region0();
    page->inc_live(2, 2 * page->object_alignment());
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const MAddress to = selfMap ? from : from + page->object_alignment();
    BaseObject* target = fixture.obj0;
    if (!selfMap) {
        GC_EXPECT_NE(from, to);
        target = fixture.PlaceObject(to);
    }
    installed.forwarding = ZForwarding::alloc(&installed.allocator, page, page->age());
    GC_EXPECT_TRUE(installed.forwarding != nullptr);
    if (!selfMap) {
        GC_EXPECT_NE(installed.forwarding->index(from), installed.forwarding->index(to));
    }
    ForwardingCursor cursor;
    GC_EXPECT_EQ(installed.forwarding->find(from, &cursor), MAddress(0));
    GC_EXPECT_EQ(installed.forwarding->insert(from, to, &cursor), to);
    if (selfMap) {
        GC_EXPECT_EQ(installed.forwarding->find(from), from);
    } else {
        GC_EXPECT_EQ(installed.forwarding->find(to), MAddress(0));
    }
    installed.forwarding->release_page();
    const zpointer colorPtr = ZAddress::color(zaddress::null, color);
    installed.generation = ZBarrier::remap_generation(colorPtr);
    installed.generation->forwarding_table().insert(installed.forwarding);
    installed.inserted = true;
    return target;
}

void RunDerivedThenBase(GcVerifyFixture& fixture, BaseObject* from, BaseObject* to, uintptr_t color, const char* tag)
{
    alignas(16) uintptr_t storage[8] {};
    auto* object = reinterpret_cast<BaseObject*>(&storage[2]);
    object->SetClassInfo(fixture.typeInfo);
    RootSlot& field = RootSlotAt(static_cast<void*>(&storage[3]));
    StorePlain(field, from_object(from));
    Mutator mutator;
    mutator.SetStackTopAddr(reinterpret_cast<uintptr_t>(storage));
    mutator.SetStackSize(sizeof(storage));
    GC_EXPECT_TRUE(mutator.IsStackAddr(reinterpret_cast<uintptr_t>(object)));
    DerivedSlot derived;
    RootSlot baseWord;
    SetRootWord(baseWord, reinterpret_cast<uintptr_t>(object));
    RebaseDerived(derived, baseWord, sizeof(void*));
    RootVisitor roots = [&](ObjectRef& root) {
        mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) { ProcessUncolored(slot, color); });
    };
    auto derivedVisitor = Mutator::MakeDerivedRootVisitor(roots, mutator);
    derivedVisitor(to_zaddress_unsafe(reinterpret_cast<uintptr_t>(object)), derived);
    const uintptr_t first = raw(field.LoadPlain());
    RootSlot baseRoot;
    SetRootWord(baseRoot, reinterpret_cast<uintptr_t>(object));
    mutator.VisitHeapRootSlots(baseRoot, [&](ObjectRef& slot) { ProcessUncolored(slot, color); });
    const uintptr_t second = raw(field.LoadPlain());
    const uintptr_t restored = raw(derived.LoadDerived());
    std::fprintf(stderr,
                 "%s first=%#llx second=%#llx from=%#llx to=%#llx restored=%#llx\n", tag,
                 static_cast<unsigned long long>(first), static_cast<unsigned long long>(second),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(from)),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(to)),
                 static_cast<unsigned long long>(restored));
    GC_EXPECT_EQ(first, reinterpret_cast<uintptr_t>(from));
    GC_EXPECT_EQ(second, reinterpret_cast<uintptr_t>(to));
    GC_EXPECT_EQ(restored, reinterpret_cast<uintptr_t>(object) + sizeof(void*));
}
} // namespace

// A stack base keeps its offset and is not expanded. The following base root
// is the only visit, so an in-place F→T entry ends at T instead of null.
GC_OTHER_VM_TEST(DerivedBaseOnce, InPlaceSecondVisitKeepsTo)
{
    GcVerifyFixture fixture;
    PageForwarding installed;
    const uintptr_t color = LoadBadColor();
    BaseObject* to = InstallInPlace(installed, fixture, color, false);
    RunDerivedThenBase(fixture, fixture.obj0, to, color, "DERIVED_INPLACE");
}

GC_OTHER_VM_TEST(DerivedBaseOnce, SelfMapStaysFrom)
{
    GcVerifyFixture fixture;
    PageForwarding installed;
    const uintptr_t color = LoadBadColor();
    BaseObject* to = InstallInPlace(installed, fixture, color, true);
    RunDerivedThenBase(fixture, fixture.obj0, to, color, "DERIVED_SELFMAP");
}

GC_OTHER_VM_TEST(DerivedBaseOnce, HeapBaseVisitsOnce)
{
    GcVerifyFixture fixture;
    const uintptr_t color = LoadBadColor();
    (*ZGeneration::young()).set_phase(ZGenerationPhase::Relocate);
    (*ZGeneration::old()).set_phase(ZGenerationPhase::Relocate);
    alignas(16) uintptr_t storage[8] {};
    Mutator mutator;
    mutator.SetStackTopAddr(reinterpret_cast<uintptr_t>(storage));
    mutator.SetStackSize(sizeof(storage));
    GC_EXPECT_FALSE(mutator.IsStackAddr(reinterpret_cast<uintptr_t>(fixture.obj0)));
    size_t visits = 0;
    RootVisitor roots = [&](ObjectRef& root) {
        mutator.VisitHeapRootSlots(root, [&](ObjectRef& slot) {
            ++visits;
            ProcessUncolored(slot, color);
        });
    };
    DerivedSlot derived;
    RootSlot baseWord;
    StorePlain(baseWord, from_object(fixture.obj0));
    RebaseDerived(derived, baseWord, sizeof(void*));
    auto derivedVisitor = Mutator::MakeDerivedRootVisitor(roots, mutator);
    derivedVisitor(to_zaddress_unsafe(reinterpret_cast<uintptr_t>(fixture.obj0)), derived);
    const uintptr_t restored = raw(derived.LoadDerived());
    std::fprintf(stderr, "DERIVED_HEAP_BASE visits=%zu restored=%#llx base=%#llx\n", visits,
                 static_cast<unsigned long long>(restored),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fixture.obj0)));
    GC_EXPECT_EQ(visits, size_t(1));
    GC_EXPECT_EQ(restored, reinterpret_cast<uintptr_t>(fixture.obj0) + sizeof(void*));
}
