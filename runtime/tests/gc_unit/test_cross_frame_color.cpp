// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// zStackWatermark.cpp:609-617, :696-704 and Mutator.cpp:762-785.
// A callee heap root that names a caller stack object is expanded with the
// callee frame's colour. The field must still be relocated with the colour of
// the frame that contains it (ZGC zStackWatermark.cpp:83-92).

#include "gc_unittest.hpp"
#include "gc_verify_fixture.hpp"
#include "CangjieRuntime.h"
#include "Common/BaseObject.inline.h"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.inline.hpp"
#include "Heap/z/zForwarding.inline.hpp"
#include "Heap/z/zForwardingAllocator.hpp"
#include "Heap/z/zPage.inline.hpp"
#include "Heap/z/zUncoloredRoot.inline.hpp"
#include "Loader/ElfUnloadQuiescence.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MClass.h"
#include "ObjectModel/MFuncdesc.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "StackMap/StackMap.h"
#include "TypeInfoManager.h"
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

void BuildEmptySlotBits(uint8_t* data)
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
    b.Put(0, 1);
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

SlotFuncDesc gEmptyDesc;
bool gEmptyLinked = false;

uintptr_t EmptySlotStartPC()
{
    if (!gEmptyLinked) {
        std::memset(&gEmptyDesc, 0, sizeof(gEmptyDesc));
        gEmptyDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&gEmptyDesc.stackMapOffset) -
            reinterpret_cast<char*>(&gEmptyDesc.descriptorOffset));
        gEmptyDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(gEmptyDesc.bits) -
            reinterpret_cast<char*>(&gEmptyDesc.stackMapOffset));
        gEmptyDesc.returnPollFlag = 1;
        BuildEmptySlotBits(gEmptyDesc.bits);
        ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(gEmptyDesc.pc));
        gEmptyLinked = true;
    }
    return reinterpret_cast<uintptr_t>(gEmptyDesc.pc);
}

// header at FA-24, reference field at FA-16. The oop-slot bit names FA-16,
// which is also the first payload word of the headered stack object.
struct alignas(16) RawFrame {
    uint64_t header;
    uint64_t field;
    uint64_t startWord;
    FrameAddress fa;
};

static_assert(offsetof(RawFrame, field) + 16 == offsetof(RawFrame, fa), "map slot must be FA-16");
static_assert(offsetof(RawFrame, header) + 8 == offsetof(RawFrame, field), "payload field follows the type word");

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

bool LoadGood(uintptr_t color)
{
    return ZPointer::is_load_good(ZAddress::color(zaddress::null, color));
}

uintptr_t ColorAt(ZStackWatermark& mark, uintptr_t sp)
{
    FrameInfo frame;
    frame.mFrame.SetSP(sp);
    return mark.prev_frame_color(frame);
}

// Four return-poll frames: Lo, Mid, G, F. The first start stores a completed
// bad colour. A mark flip then stores G's SP with a load-good colour. G's
// heap root names a headered object in F; one process_one hold covers both.
// The field in F must become the forwarded address. callerListsField is false
// when F's return-poll map names no slot, so only G's root reaches the field.
void CheckOwningFrame(bool callerListsField)
{
    const StackGrowConfig savedGrow = CangjieRuntime::stackGrowConfig;
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
    struct GrowRestore {
        StackGrowConfig* slot;
        StackGrowConfig value;
        ~GrowRestore() { *slot = value; }
    } growRestore { &CangjieRuntime::stackGrowConfig, savedGrow };

    const uintptr_t start = SlotStartPC();
    const uintptr_t callerStart = callerListsField ? start : EmptySlotStartPC();
    const uintptr_t site = start + 16;
    const uintptr_t callerSite = callerStart + 16;
    FuncDescRef descriptor = MFuncDesc::GetFuncDesc(start);
    GC_EXPECT_TRUE(descriptor != nullptr && descriptor->HasReturnPoll());

    alignas(8) uint8_t typeStorage[sizeof(TypeInfo)] {};
    TypeInfo* stackType = reinterpret_cast<TypeInfo*>(typeStorage);
    stackType->SetType(TypeKind::TYPE_KIND_CLASS);
    stackType->SetFlagHasRefField();
    stackType->SetInstanceSize(16);
    GCTib tib {};
    tib.tag = SIGN_BIT | 1;
    stackType->SetGCTib(tib);
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
        reinterpret_cast<uintptr_t>(typeStorage), sizeof(typeStorage));

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

    const uintptr_t badEpoch = owner.GetStackWatermark().GetEpoch();
    int flips = 0;
    while (LoadGood(badEpoch) || StackWatermark::epoch_id() == badEpoch) {
        if ((flips & 1) == 0) {
            ZGlobalsPointers::flip_young_relocate_start();
        } else {
            ZGlobalsPointers::flip_old_relocate_start();
        }
        ++flips;
        GC_EXPECT_TRUE(flips <= 8);
    }
    const uintptr_t goodEpoch = StackWatermark::epoch_id();
    std::fprintf(stderr, "XFRAME_ARM caller_lists=%d\n", callerListsField ? 1 : 0);
    std::fprintf(stderr, "XFRAME_EPOCH bad=%llx good=%llx flips=%d bad_load=%d good_load=%d started=%d\n",
        static_cast<unsigned long long>(badEpoch), static_cast<unsigned long long>(goodEpoch), flips,
        LoadGood(badEpoch) ? 1 : 0, LoadGood(goodEpoch) ? 1 : 0,
        owner.GetStackWatermark().processing_started() ? 1 : 0);
    GC_EXPECT_FALSE(LoadGood(badEpoch));
    GC_EXPECT_TRUE(LoadGood(goodEpoch));
    GC_EXPECT_FALSE(owner.GetStackWatermark().processing_started());

    PageForwarding installed;
    BaseObject* to = InstallInPlace(installed, fixture, badEpoch);
    BaseObject* from = fixture.obj0;

    alignas(16) RawFrame frames[4] {};
    for (int i = 0; i < 4; ++i) {
        const bool caller = i == 3;
        frames[i].startWord = (caller ? callerStart : start) + 9;
        frames[i].fa.returnAddress = reinterpret_cast<uint32_t*>(caller ? callerSite : site);
        frames[i].fa.callerFrameAddress = caller ? nullptr : &frames[i + 1].fa;
    }
    BaseObject* stackObj = reinterpret_cast<BaseObject*>(&frames[3].header);
    stackObj->SetClassInfo(stackType);
    uint64_t* field = &frames[3].field;
    StorePlain(RootSlotAt(static_cast<void*>(field)), from_object(from));
    int payloadVisits = 0;
    uintptr_t payloadSlot = 0;
    stackObj->ForEachRefField([&](RefField<>& ref) {
        ++payloadVisits;
        payloadSlot = reinterpret_cast<uintptr_t>(&ref);
    });
    std::fprintf(stderr, "XFRAME_OBJECT valid=%d type=%p visits=%d slot=%llx field=%llx heap_type=%d\n",
        stackObj->IsValidObject() ? 1 : 0, static_cast<void*>(stackObj->GetTypeInfo()), payloadVisits,
        static_cast<unsigned long long>(payloadSlot),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(field)),
        Heap::IsHeapAddress(stackType) ? 1 : 0);
    GC_EXPECT_TRUE(stackObj->IsValidObject());
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(stackObj->GetTypeInfo()), reinterpret_cast<uintptr_t>(stackType));
    GC_EXPECT_TRUE(stackType->IsVaildType());
    GC_EXPECT_TRUE(stackType->HasRefField());
    GC_EXPECT_FALSE(Heap::IsHeapAddress(reinterpret_cast<uintptr_t>(stackType)));
    GC_EXPECT_EQ(payloadVisits, 1);
    GC_EXPECT_EQ(payloadSlot, reinterpret_cast<uintptr_t>(field));

    uintptr_t decodedSlot = 0;
    size_t decodedVisits = 0;
    RootVisitor locate = [&](ObjectRef& slot) {
        ++decodedVisits;
        decodedSlot = reinterpret_cast<uintptr_t>(&slot);
    };
    HeapReferenceMap placed = StackMapBuilder(callerStart, callerSite, reinterpret_cast<uintptr_t>(&frames[3].fa))
        .Build<HeapReferenceMap>();
    placed.VisitSlotRoots(locate, nullptr);
    GC_EXPECT_TRUE(placed.IsValid());
    if (callerListsField) {
        GC_EXPECT_EQ(decodedVisits, size_t { 1 });
        GC_EXPECT_EQ(decodedSlot, reinterpret_cast<uintptr_t>(field));
    } else {
        GC_EXPECT_EQ(decodedVisits, size_t { 0 });
    }
    uintptr_t calleeSlot = 0;
    size_t calleeVisits = 0;
    RootVisitor locateCallee = [&](ObjectRef& slot) {
        ++calleeVisits;
        calleeSlot = reinterpret_cast<uintptr_t>(&slot);
    };
    HeapReferenceMap calleeMap = StackMapBuilder(start, site, reinterpret_cast<uintptr_t>(&frames[2].fa))
        .Build<HeapReferenceMap>();
    calleeMap.VisitSlotRoots(locateCallee, nullptr);
    GC_EXPECT_TRUE(calleeMap.IsValid());
    GC_EXPECT_EQ(calleeVisits, size_t { 1 });
    GC_EXPECT_EQ(calleeSlot, reinterpret_cast<uintptr_t>(&frames[2].field));

    owner.SetStackTopAddr(reinterpret_cast<uintptr_t>(&frames[0]) - 8);
    owner.SetStackSize(sizeof(frames) + 16);
    GC_EXPECT_TRUE(owner.IsStackAddr(reinterpret_cast<uintptr_t>(stackObj)));
    GC_EXPECT_TRUE(owner.IsStackAddr(reinterpret_cast<uintptr_t>(field)));
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&frames[3].fa);
    context.frameInfo.mFrame.SetFA(&frames[0].fa);
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(site));
    context.frameInfo.mFrame.SetSP(reinterpret_cast<uintptr_t>(&frames[0]));
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    owner.SetManagedContext(true);

    owner.GetStackWatermark().process_one();
    const uintptr_t boundary = owner.GetStackWatermark().last_processed_raw();
    const uintptr_t covered = ColorAt(owner.GetStackWatermark(), ~uintptr_t { 0 });
    std::fprintf(stderr, "XFRAME_SETUP done=%d epoch=%llx boundary=%llx covered=%llx field=%llx\n",
        owner.GetStackWatermark().IsDone() ? 1 : 0,
        static_cast<unsigned long long>(owner.GetStackWatermark().GetEpoch()),
        static_cast<unsigned long long>(boundary), static_cast<unsigned long long>(covered),
        static_cast<unsigned long long>(raw(RootSlotAt(static_cast<void*>(field)).LoadPlain())));
    GC_EXPECT_FALSE(owner.GetStackWatermark().IsDone());
    GC_EXPECT_NE(boundary, uintptr_t { 0 });
    GC_EXPECT_EQ(owner.GetStackWatermark().GetEpoch(), goodEpoch);
    GC_EXPECT_EQ(covered, badEpoch);
    GC_EXPECT_EQ(raw(RootSlotAt(static_cast<void*>(field)).LoadPlain()),
        reinterpret_cast<uintptr_t>(from));

    ZGlobalsPointers::flip_young_mark_start();
    if (StackWatermark::epoch_id() == goodEpoch) { ZGlobalsPointers::flip_old_mark_start(); }
    const uintptr_t nextEpoch = StackWatermark::epoch_id();
    std::fprintf(stderr, "XFRAME_MARK next=%llx good_load=%d bad_load=%d started=%d\n",
        static_cast<unsigned long long>(nextEpoch), LoadGood(goodEpoch) ? 1 : 0, LoadGood(badEpoch) ? 1 : 0,
        owner.GetStackWatermark().processing_started() ? 1 : 0);
    GC_EXPECT_NE(nextEpoch, goodEpoch);
    GC_EXPECT_TRUE(LoadGood(goodEpoch));
    GC_EXPECT_FALSE(LoadGood(badEpoch));
    GC_EXPECT_FALSE(owner.GetStackWatermark().processing_started());
    GC_EXPECT_TRUE(reinterpret_cast<uintptr_t>(field) > boundary);

    frames[2].field = reinterpret_cast<uint64_t>(stackObj);
    GC_EXPECT_TRUE(owner.IsStackAddr(frames[2].field));
    owner.GetStackWatermark().process_one();
    const uintptr_t gColor = ColorAt(owner.GetStackWatermark(), boundary);
    const uintptr_t fColor = ColorAt(owner.GetStackWatermark(), boundary + 1);
    const uintptr_t after = raw(RootSlotAt(static_cast<void*>(field)).LoadPlain());
    std::fprintf(stderr,
        "XFRAME_FIELD after=%llx from=%llx to=%llx g=%llx f=%llx g_load=%d f_load=%llx done=%d\n",
        static_cast<unsigned long long>(after),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(from)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(to)),
        static_cast<unsigned long long>(gColor), static_cast<unsigned long long>(fColor),
        LoadGood(gColor) ? 1 : 0, static_cast<unsigned long long>(LoadGood(fColor) ? 1 : 0),
        owner.GetStackWatermark().IsDone() ? 1 : 0);
    GC_EXPECT_NE(gColor, fColor);
    GC_EXPECT_TRUE(LoadGood(gColor));
    GC_EXPECT_FALSE(LoadGood(fColor));
    GC_EXPECT_EQ(after, reinterpret_cast<uintptr_t>(to));
}

GC_OTHER_VM_TEST(CrossFrameColor, OwningFrameRemapsField)
{
    CheckOwningFrame(true);
}

GC_OTHER_VM_TEST(CrossFrameColor, CalleeRootOnlyRemapsField)
{
    CheckOwningFrame(false);
}

} // namespace

#endif
