// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// stackWatermark.cpp:205-228 plus zStackWatermark.cpp:209. A callee heap root
// that names a caller stack record must keep that caller inside the same
// processing hold, and the record field is fixed once.

#include "gc_unittest.hpp"
#include "gc_verify_fixture.hpp"
#include "CangjieRuntime.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zForwarding.inline.hpp"
#include "Heap/z/zForwardingAllocator.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MFuncdesc.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "StackMap/StackMap.h"
#include <cstdio>
#include <cstring>

#if defined(__linux__) && defined(__x86_64__)

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

struct SlotFuncDesc {
    int32_t descriptorOffset;
    uint32_t pc[4];
    int32_t stackMapOffset;
    uint32_t rest[6];
    uint32_t returnPollFlag;
    uint8_t bits[256];
};

SlotFuncDesc gDesc;
bool gLinked = false;

struct Bits {
    uint8_t* data;
    uint32_t bit = 0;
    void Put(uint32_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            data[bit / 8] |= static_cast<uint8_t>(((value >> i) & 1u) << (bit % 8));
        }
    }
    void Var(uint32_t value)
    {
        if (value <= 11) {
            Put(value, 4);
        } else {
            Put(12, 4);
            Put(value, 8);
        }
    }
};

// Grow-off map with STACKMAP_OOP_SLOT_BIT. pc 16 names no register and no GC
// slot. The FA-16 bit lives only in the oop-slot table. StackPtrMap::VisitSlot
// walks GC slots, so it must not raise stackTarget; HeapReferenceMap does.
void BuildSlotBits(uint8_t* data)
{
    std::memset(data, 0, 256);
    Bits b { data };
    b.Var(0);
    b.Var(2);
    b.Var(0);
    b.Var(2);
    b.Var(4);
    b.Var(1);
    b.Var(1);
    b.Var(1);
    b.Var(1);
    b.Var(1);
    b.Var(0);
    b.Put(0, 32);
    b.Put(0, 4);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(16, 32);
    b.Put(0, 4);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(0, 1);
    b.Put(1, 1);
    b.Var(0);
    b.Var(16);
    b.Var(1);
    b.Var(8);
    b.Var(8);
    b.Put(0, 8);
    b.Put(0x4, 8);
    b.Var(0);
    b.Var(0);
    b.Var(0);
}

uintptr_t SlotStartPC()
{
    if (!gLinked) {
        std::memset(&gDesc, 0, sizeof(gDesc));
        gDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&gDesc.stackMapOffset) -
            reinterpret_cast<char*>(&gDesc.descriptorOffset));
        gDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(gDesc.bits) -
            reinterpret_cast<char*>(&gDesc.stackMapOffset));
        gDesc.returnPollFlag = 1;
        BuildSlotBits(gDesc.bits);
        ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(gDesc.pc));
        gLinked = true;
    }
    return reinterpret_cast<uintptr_t>(gDesc.pc);
}

struct RawFrame {
    uint64_t slot;
    uint64_t startWord;
    FrameAddress fa;
};

struct PageForwarding {
    ZForwardingAllocator allocator;
    ZGeneration* generation = nullptr;
    ZForwarding* forwarding = nullptr;
    bool inserted = false;
    PageForwarding() : allocator(static_cast<size_t>(1) << 20) {}
    ~PageForwarding()
    {
        if (inserted) { generation->forwarding_table().remove(forwarding); }
        if (forwarding != nullptr) { forwarding->~ZForwarding(); }
    }
    PageForwarding(const PageForwarding&) = delete;
    PageForwarding& operator=(const PageForwarding&) = delete;
};

BaseObject* InstallInPlace(PageForwarding& installed, GcVerifyFixture& fixture, uintptr_t color)
{
    (*ZGeneration::young()).set_phase(ZGenerationPhase::Relocate);
    (*ZGeneration::old()).set_phase(ZGenerationPhase::Relocate);
    ZPage* page = fixture.region0();
    page->inc_live(2, 2 * page->object_alignment());
    const MAddress from = reinterpret_cast<MAddress>(fixture.obj0);
    const MAddress to = from + page->object_alignment();
    GC_EXPECT_NE(from, to);
    BaseObject* target = fixture.PlaceObject(to);
    installed.forwarding = ZForwarding::alloc(&installed.allocator, page, page->age());
    GC_EXPECT_TRUE(installed.forwarding != nullptr);
    GC_EXPECT_NE(installed.forwarding->index(from), installed.forwarding->index(to));
    ForwardingCursor cursor;
    GC_EXPECT_EQ(installed.forwarding->find(from, &cursor), MAddress(0));
    GC_EXPECT_EQ(installed.forwarding->insert(from, to, &cursor), to);
    GC_EXPECT_EQ(installed.forwarding->find(to), MAddress(0));
    installed.forwarding->release_page();
    const zpointer colorPtr = ZAddress::color(zaddress::null, color);
    installed.generation = ZBarrier::remap_generation(colorPtr);
    installed.generation->forwarding_table().insert(installed.forwarding);
    installed.inserted = true;
    return target;
}

} // namespace

// Two public process_one calls. The first hold covers the callee that names S
// and, when the heap root raises stackTarget, S's own frame. The second hold
// must not relocate the already forwarded field back to null.
GC_OTHER_VM_TEST(HeapRootStackTarget, CrossLockFieldStaysForwarded)
{
    const StackGrowConfig savedGrow = CangjieRuntime::stackGrowConfig;
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
    struct GrowRestore {
        StackGrowConfig* slot;
        StackGrowConfig value;
        ~GrowRestore() { *slot = value; }
    } growRestore { &CangjieRuntime::stackGrowConfig, savedGrow };

    const uintptr_t start = SlotStartPC();
    const uintptr_t site = start + 16;
    FuncDescRef descriptor = MFuncDesc::GetFuncDesc(start);
    const bool returnPoll = descriptor != nullptr && descriptor->HasReturnPoll();
    std::fprintf(stderr, "HEAP_ROOT_POLL desc=%d poll=%d\n", descriptor != nullptr ? 1 : 0, returnPoll ? 1 : 0);
    GC_EXPECT_TRUE(returnPoll);
    HeapReferenceMap decoded = StackMapBuilder(start, site, 0).Build<HeapReferenceMap>();
    const StackMapRootCounts counts = decoded.CountRootSlots();
    std::fprintf(stderr, "HEAP_ROOT_MAP valid=%d slots=%zu regs=%zu\n", decoded.IsValid() ? 1 : 0,
        counts.baseSlots, counts.baseRegs);
    GC_EXPECT_TRUE(decoded.IsValid());
    GC_EXPECT_EQ(counts.baseSlots, size_t { 0 });
    GC_EXPECT_EQ(counts.baseRegs, size_t { 0 });
    GC_EXPECT_FALSE(decoded.HasRegisterRoots());

    GcVerifyFixture fixture;
    Mutator owner;
    ThreadLocalData* tls = ThreadLocal::GetThreadLocalData();
    Mutator* savedMutator = tls->mutator;
    struct TlsRestore {
        ThreadLocalData* tls;
        Mutator* saved;
        ~TlsRestore() { tls->SetMutator(saved); }
    } tlsRestore { tls, savedMutator };
    tls->SetMutator(&owner);

    const uintptr_t epoch = owner.GetStackWatermark().GetEpoch();
    const auto colored = [&]() { return ZAddress::color(zaddress::null, epoch); };
    if (ZPointer::is_load_good(colored())) { ZGlobalsPointers::flip_young_relocate_start(); }
    if (ZPointer::is_load_good(colored())) { ZGlobalsPointers::flip_old_relocate_start(); }
    const bool started = owner.GetStackWatermark().processing_started();
    const bool loadGood = ZPointer::is_load_good(colored());
    std::fprintf(stderr, "HEAP_ROOT_EPOCH epoch=%llx started=%d load_good=%d\n",
        static_cast<unsigned long long>(epoch), started ? 1 : 0, loadGood ? 1 : 0);
    GC_EXPECT_FALSE(started);
    GC_EXPECT_TRUE(!loadGood);

    PageForwarding installed;
    BaseObject* to = InstallInPlace(installed, fixture, epoch);
    BaseObject* from = fixture.obj0;

    alignas(16) RawFrame frames[4] {};
    for (int i = 0; i < 4; ++i) {
        frames[i].startWord = start + 9;
        frames[i].fa.returnAddress = reinterpret_cast<uint32_t*>(site);
        frames[i].fa.callerFrameAddress = i == 3 ? nullptr : &frames[i + 1].fa;
    }
    uint64_t* field = &frames[3].slot;
    StorePlain(RootSlotAt(static_cast<void*>(field)), from_object(from));
    frames[2].slot = reinterpret_cast<uint64_t>(field);
    const uintptr_t senderSP = reinterpret_cast<uintptr_t>(&frames[2].fa) + sizeof(FrameAddress);
    GC_EXPECT_TRUE(reinterpret_cast<uintptr_t>(field) >= senderSP);

    uintptr_t decodedSlot = 0;
    size_t decodedVisits = 0;
    RootVisitor locate = [&](ObjectRef& slot) {
        ++decodedVisits;
        decodedSlot = reinterpret_cast<uintptr_t>(&slot);
    };
    HeapReferenceMap placed = StackMapBuilder(start, site, reinterpret_cast<uintptr_t>(&frames[3].fa))
        .Build<HeapReferenceMap>();
    placed.VisitSlotRoots(locate, nullptr);
    size_t ptrVisits = 0;
    StackPtrMap pointers = StackMapBuilder(start, site, reinterpret_cast<uintptr_t>(&frames[3].fa))
        .Build<StackPtrMap>();
    pointers.VisitSlot([&](ObjectRef&) { ++ptrVisits; }, [&](ObjectRef&) { ++ptrVisits; }, nullptr);
    std::fprintf(stderr, "HEAP_ROOT_SLOT visits=%zu slot=%llx field=%llx ptr_visits=%zu ptr_valid=%d\n",
        decodedVisits, static_cast<unsigned long long>(decodedSlot),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(field)), ptrVisits,
        pointers.IsValid() ? 1 : 0);
    GC_EXPECT_EQ(decodedVisits, size_t { 1 });
    GC_EXPECT_EQ(decodedSlot, reinterpret_cast<uintptr_t>(field));
    GC_EXPECT_TRUE(pointers.IsValid());
    GC_EXPECT_EQ(ptrVisits, size_t { 0 });

    owner.SetStackTopAddr(reinterpret_cast<uintptr_t>(&frames[0]) - 8);
    owner.SetStackSize(sizeof(frames) + 16);
    GC_EXPECT_TRUE(owner.IsStackAddr(reinterpret_cast<uintptr_t>(field)));
    GC_EXPECT_TRUE(owner.IsStackAddr(frames[2].slot));
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&frames[3].fa);
    context.frameInfo.mFrame.SetFA(&frames[0].fa);
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(site));
    context.frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(&frames[0]));
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    owner.SetManagedContext(true);

    owner.GetStackWatermark().process_one();
    const uintptr_t afterFirst = raw(RootSlotAt(static_cast<void*>(field)).LoadPlain());
    owner.GetStackWatermark().process_one();
    const uintptr_t afterSecond = raw(RootSlotAt(static_cast<void*>(field)).LoadPlain());
    std::fprintf(stderr, "HEAP_ROOT_FIELD first=%llx second=%llx from=%llx to=%llx done=%d\n",
        static_cast<unsigned long long>(afterFirst), static_cast<unsigned long long>(afterSecond),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(from)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(to)),
        owner.GetStackWatermark().IsDone() ? 1 : 0);
    GC_EXPECT_EQ(afterSecond, reinterpret_cast<uintptr_t>(to));
}

#endif
