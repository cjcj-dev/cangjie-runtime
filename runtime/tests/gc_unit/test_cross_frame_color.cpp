// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
//
// zStackWatermark.cpp:83-92 and :210.
// A callee heap root that names a caller stack object is expanded from the
// callee frame. The field is relocated with the colour of the frame that
// contains it. A slot or register the frame lists directly keeps that frame's
// colour, even when the spill sits in an already processed callee.

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
#include <elf.h>
#include <vector>

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
    void Put(uint64_t value, unsigned width)
    {
        for (unsigned i = 0; i < width; ++i, ++bit) {
            data[bit / 8] |= static_cast<uint8_t>(((value >> i) & 1ull) << (bit % 8));
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

enum class ColorArm { Listed, CalleeOnly, Boundary };

// Four return-poll frames: Lo, Mid, G, F. The first start stores a completed
// bad colour. A mark flip then stores G's SP with a load-good colour. G's
// heap root names a headered object in F; one process_one hold covers both.
// The field in F must become the forwarded address. CalleeOnly leaves F's
// return-poll map with no slot, so only G's root reaches the field. Boundary
// puts an already forwarded field in G, above G.sp, which is the saved watermark.
void CheckOwningFrame(ColorArm arm)
{
    const bool callerListsField = arm == ColorArm::Listed;
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

    uint64_t* boundaryField = nullptr;
    if (arm == ColorArm::Boundary) {
        boundaryField = reinterpret_cast<uint64_t*>(boundary) + 1;
        std::fprintf(stderr, "XFRAME_BOUNDARY sp=%llx field=%llx header=%llx slot=%llx\n",
            static_cast<unsigned long long>(boundary),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(boundaryField)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&frames[2].header)),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&frames[2].field)));
        GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(boundaryField), reinterpret_cast<uintptr_t>(&frames[2].header));
        GC_EXPECT_NE(reinterpret_cast<uintptr_t>(boundaryField), reinterpret_cast<uintptr_t>(&frames[2].field));
        BaseObject* local = reinterpret_cast<BaseObject*>(boundary);
        local->SetClassInfo(stackType);
        StorePlain(RootSlotAt(static_cast<void*>(boundaryField)), from_object(to));
        frames[2].field = reinterpret_cast<uint64_t>(local);
        GC_EXPECT_TRUE(owner.IsStackAddr(reinterpret_cast<uintptr_t>(local)));
        GC_EXPECT_TRUE(owner.IsStackAddr(reinterpret_cast<uintptr_t>(boundaryField)));
        GC_EXPECT_TRUE(local->IsValidObject());
    } else {
        frames[2].field = reinterpret_cast<uint64_t>(stackObj);
    }
    GC_EXPECT_TRUE(owner.IsStackAddr(frames[2].field));
    owner.GetStackWatermark().process_one();
    const uintptr_t gColor = ColorAt(owner.GetStackWatermark(), boundary);
    const uintptr_t fColor = ColorAt(owner.GetStackWatermark(), boundary + 1);
    const uintptr_t after = arm == ColorArm::Boundary
        ? raw(RootSlotAt(static_cast<void*>(boundaryField)).LoadPlain())
        : raw(RootSlotAt(static_cast<void*>(field)).LoadPlain());
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
    if (arm == ColorArm::Boundary) {
        const uintptr_t addressColor = ColorAt(owner.GetStackWatermark(), reinterpret_cast<uintptr_t>(boundaryField));
        std::fprintf(stderr, "XFRAME_BOUNDARY_COLOR frame=%llx addr=%llx frame_load=%d addr_load=%d\n",
            static_cast<unsigned long long>(gColor), static_cast<unsigned long long>(addressColor),
            LoadGood(gColor) ? 1 : 0, LoadGood(addressColor) ? 1 : 0);
        GC_EXPECT_TRUE(LoadGood(gColor));
        GC_EXPECT_FALSE(LoadGood(addressColor));
    }
}

uintptr_t LocalRuntimeSymbol(const char* name)
{
    char path[768] {};
    FILE* maps = std::fopen("/proc/self/maps", "r");
    GC_EXPECT_TRUE(maps != nullptr);
    char line[1024];
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long offset = 0;
        char perms[8] {};
        char file[768] {};
        if (std::sscanf(line, "%lx-%*lx %7s %lx %*s %*u %767s", &start, perms, &offset, file) != 4) {
            continue;
        }
        if (perms[0] == 'r' && perms[2] == 'x' && std::strstr(file, "libcangjie-runtime.so") != nullptr) {
            std::snprintf(path, sizeof(path), "%s", file);
            found = true;
            break;
        }
    }
    std::fclose(maps);
    GC_EXPECT_TRUE(found);
    FILE* elf = std::fopen(path, "rb");
    GC_EXPECT_TRUE(elf != nullptr);
    Elf64_Ehdr header {};
    GC_EXPECT_EQ(std::fread(&header, sizeof(header), 1, elf), size_t { 1 });
    GC_EXPECT_EQ(std::fseek(elf, static_cast<long>(header.e_phoff), SEEK_SET), 0);
    std::vector<Elf64_Phdr> programs(header.e_phnum);
    GC_EXPECT_EQ(std::fread(programs.data(), sizeof(Elf64_Phdr), header.e_phnum, elf), size_t { header.e_phnum });
    GC_EXPECT_EQ(std::fseek(elf, static_cast<long>(header.e_shoff), SEEK_SET), 0);
    std::vector<Elf64_Shdr> sections(header.e_shnum);
    GC_EXPECT_EQ(std::fread(sections.data(), sizeof(Elf64_Shdr), header.e_shnum, elf), size_t { header.e_shnum });
    uintptr_t value = 0;
    for (const Elf64_Shdr& section : sections) {
        if (section.sh_type != SHT_SYMTAB) { continue; }
        const Elf64_Shdr& stringsHeader = sections[section.sh_link];
        std::vector<char> strings(stringsHeader.sh_size);
        GC_EXPECT_EQ(std::fseek(elf, static_cast<long>(stringsHeader.sh_offset), SEEK_SET), 0);
        GC_EXPECT_EQ(std::fread(strings.data(), 1, strings.size(), elf), strings.size());
        const size_t count = section.sh_size / sizeof(Elf64_Sym);
        std::vector<Elf64_Sym> symbols(count);
        GC_EXPECT_EQ(std::fseek(elf, static_cast<long>(section.sh_offset), SEEK_SET), 0);
        GC_EXPECT_EQ(std::fread(symbols.data(), sizeof(Elf64_Sym), count, elf), count);
        for (const Elf64_Sym& symbol : symbols) {
            if (symbol.st_name == 0 || symbol.st_name >= strings.size() || symbol.st_value == 0) { continue; }
            if (std::strcmp(strings.data() + symbol.st_name, name) == 0) { value = symbol.st_value; }
        }
    }
    std::fclose(elf);
    GC_EXPECT_NE(value, uintptr_t { 0 });
    maps = std::fopen("/proc/self/maps", "r");
    GC_EXPECT_TRUE(maps != nullptr);
    uintptr_t address = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long offset = 0;
        char file[768] {};
        if (std::sscanf(line, "%lx-%*lx %*s %lx %*s %*u %767s", &start, &offset, file) != 3) { continue; }
        if (std::strcmp(file, path) != 0) { continue; }
        for (const Elf64_Phdr& program : programs) {
            if (program.p_type != PT_LOAD || program.p_offset != offset) { continue; }
            if (value < program.p_vaddr || value >= program.p_vaddr + program.p_memsz) { continue; }
            address = static_cast<uintptr_t>(start) - program.p_vaddr + value;
        }
    }
    std::fclose(maps);
    GC_EXPECT_NE(address, uintptr_t { 0 });
    return address;
}

struct ChainNode {
    uint64_t startWord;
    FrameAddress fa;
};
static_assert(offsetof(ChainNode, startWord) + 8 == offsetof(ChainNode, fa), "start word must sit at FA-8");

void LinkNode(ChainNode& node, uintptr_t start, FrameAddress* caller, const uint32_t* ret)
{
    node.startWord = start + 9;
    node.fa.callerFrameAddress = caller;
    node.fa.returnAddress = ret;
}

// Caller F's callee-saved register is spilled in the already processed callee.
// F itself is not processed until the value is still from. The spill address is
// the saved watermark, so an address colour is load-good and skips relocation.
void CheckRegisterRoot()
{
    const StackGrowConfig savedGrow = CangjieRuntime::stackGrowConfig;
    CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
    struct GrowRestore {
        StackGrowConfig* slot;
        StackGrowConfig value;
        ~GrowRestore() { *slot = value; }
    } growRestore { &CangjieRuntime::stackGrowConfig, savedGrow };

    const uintptr_t empty = EmptySlotStartPC();
    const uintptr_t emptySite = empty + 16;
    static SlotFuncDesc regDesc;
    static bool regLinked = false;
    if (!regLinked) {
        std::memset(&regDesc, 0, sizeof(regDesc));
        regDesc.descriptorOffset = static_cast<int32_t>(reinterpret_cast<char*>(&regDesc.stackMapOffset) -
            reinterpret_cast<char*>(&regDesc.descriptorOffset));
        regDesc.stackMapOffset = static_cast<int32_t>(reinterpret_cast<char*>(regDesc.bits) -
            reinterpret_cast<char*>(&regDesc.stackMapOffset));
        regDesc.returnPollFlag = 1;
        Bits bits { regDesc.bits };
        bits.Var(0); bits.Var(0); bits.Var(0);
        bits.Var(2); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(1); bits.Var(0);
        bits.Put(0, 32); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
        bits.Put(16, 32); bits.Put(1, 1); bits.Put(0, 1); bits.Put(0, 1); bits.Put(0, 1);
        bits.Var(1); bits.Var(33); bits.Put(uint64_t { 1 } << RBX, 33);
        bits.Var(0); bits.Var(0); bits.Var(0);
        bits.Var(0); bits.Var(0);
        bits.Var(0);
        ElfUnloadQuiescence::LinkImage(reinterpret_cast<uintptr_t>(regDesc.pc));
        regLinked = true;
    }
    const uintptr_t regStart = reinterpret_cast<uintptr_t>(regDesc.pc);
    const uintptr_t regSite = regStart + 16;
    const uintptr_t safepoint = LocalRuntimeSymbol("unwindPCForSafepointHandlerStub");

    alignas(16) uint8_t mem[4096] {};
    const uintptr_t base = reinterpret_cast<uintptr_t>(mem);
    const uintptr_t safepointFa = base + 0x800;
    RegSlotsMap spills;
    RegRoot::RecordStubAllRegister(spills, safepointFa);
    GC_EXPECT_TRUE(spills.HasReg(RBX));
    RootSlot* spill = spills.addrMap[RBX];
    const uintptr_t spillAddr = reinterpret_cast<uintptr_t>(spill);
    auto* callerB = reinterpret_cast<ChainNode*>(spillAddr - 16 - offsetof(ChainNode, fa));
    auto* frameA = reinterpret_cast<ChainNode*>(base + 0x100);
    auto* frameC = reinterpret_cast<ChainNode*>(base + 0x180);
    auto* frameF = reinterpret_cast<ChainNode*>(base + 0x200);
    auto* frameS = reinterpret_cast<ChainNode*>(safepointFa - offsetof(ChainNode, fa));
    std::fprintf(stderr, "XFRAME_REG spill=%llx bfa=%llx sfa=%llx fsp=%llx\n",
        static_cast<unsigned long long>(spillAddr),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&callerB->fa)),
        static_cast<unsigned long long>(safepointFa),
        static_cast<unsigned long long>(safepointFa + sizeof(FrameAddress)));
    GC_EXPECT_TRUE(reinterpret_cast<uintptr_t>(callerB) > base);
    GC_EXPECT_TRUE(reinterpret_cast<uintptr_t>(callerB) + sizeof(ChainNode) < base + sizeof(mem));
    LinkNode(*frameA, empty, &callerB->fa, reinterpret_cast<const uint32_t*>(emptySite));
    LinkNode(*callerB, empty, &frameC->fa, reinterpret_cast<const uint32_t*>(emptySite));
    LinkNode(*frameC, empty, &frameS->fa, reinterpret_cast<const uint32_t*>(safepoint));
    frameS->fa.callerFrameAddress = &frameF->fa;
    frameS->fa.returnAddress = reinterpret_cast<const uint32_t*>(regSite);
    frameF->startWord = regStart + 9;
    frameF->fa.callerFrameAddress = nullptr;
    frameF->fa.returnAddress = nullptr;

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
        if ((flips & 1) == 0) { ZGlobalsPointers::flip_young_relocate_start(); }
        else { ZGlobalsPointers::flip_old_relocate_start(); }
        ++flips;
        GC_EXPECT_TRUE(flips <= 8);
    }
    PageForwarding installed;
    BaseObject* to = InstallInPlace(installed, fixture, badEpoch);
    BaseObject* from = fixture.obj0;
    owner.SetStackTopAddr(base);
    owner.SetStackSize(sizeof(mem));
    GC_EXPECT_TRUE(owner.IsStackAddr(spillAddr));
    UnwindContext& context = owner.GetUnwindContext();
    context.anchorFA = reinterpret_cast<uint32_t*>(&frameF->fa);
    context.frameInfo.mFrame.SetFA(&frameA->fa);
    context.frameInfo.mFrame.SetIP(reinterpret_cast<const uint32_t*>(emptySite));
    context.frameInfo.mFrame.SetSP(base + 0x40);
    context.SetUnwindContextStatus(UnwindContextStatus::RISKY);
    owner.SetManagedContext(true);

    owner.GetStackWatermark().process_one();
    const uintptr_t boundary = owner.GetStackWatermark().last_processed_raw();
    std::fprintf(stderr, "XFRAME_REG_SETUP boundary=%llx spill=%llx fsp=%llx started=%d done=%d\n",
        static_cast<unsigned long long>(boundary), static_cast<unsigned long long>(spillAddr),
        static_cast<unsigned long long>(safepointFa + sizeof(FrameAddress)),
        owner.GetStackWatermark().processing_started() ? 1 : 0,
        owner.GetStackWatermark().IsDone() ? 1 : 0);
    GC_EXPECT_FALSE(owner.GetStackWatermark().IsDone());
    GC_EXPECT_TRUE(spillAddr <= boundary);
    GC_EXPECT_TRUE(safepointFa + sizeof(FrameAddress) > boundary);

    ZGlobalsPointers::flip_young_mark_start();
    if (StackWatermark::epoch_id() == owner.GetStackWatermark().GetEpoch()) {
        ZGlobalsPointers::flip_old_mark_start();
    }
    GC_EXPECT_FALSE(owner.GetStackWatermark().processing_started());
    owner.GetStackWatermark().process_one();
    StorePlain(*spill, from_object(from));
    const uintptr_t spillColor = ColorAt(owner.GetStackWatermark(), spillAddr);
    const uintptr_t frameColor = ColorAt(owner.GetStackWatermark(), safepointFa + sizeof(FrameAddress));
    std::fprintf(stderr, "XFRAME_REG_COLOR spill=%llx frame=%llx spill_load=%d frame_load=%d\n",
        static_cast<unsigned long long>(spillColor), static_cast<unsigned long long>(frameColor),
        LoadGood(spillColor) ? 1 : 0, LoadGood(frameColor) ? 1 : 0);
    GC_EXPECT_TRUE(LoadGood(spillColor));
    GC_EXPECT_FALSE(LoadGood(frameColor));
    owner.GetStackWatermark().process_one();
    const uintptr_t after = raw(spill->LoadPlain());
    std::fprintf(stderr, "XFRAME_REG_FIELD after=%llx from=%llx to=%llx\n",
        static_cast<unsigned long long>(after),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(from)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(to)));
    GC_EXPECT_EQ(after, reinterpret_cast<uintptr_t>(to));
}

GC_OTHER_VM_TEST(CrossFrameColor, OwningFrameRemapsField)
{
    CheckOwningFrame(ColorArm::Listed);
}

GC_OTHER_VM_TEST(CrossFrameColor, CalleeRootOnlyRemapsField)
{
    CheckOwningFrame(ColorArm::CalleeOnly);
}

GC_OTHER_VM_TEST(CrossFrameColor, BoundaryFrameKeepsGoodField)
{
    CheckOwningFrame(ColorArm::Boundary);
}

GC_OTHER_VM_TEST(CrossFrameColor, RegisterRootUsesListingFrame)
{
    CheckRegisterRoot();
}

} // namespace

#endif
