// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

// Constructive M0 classification through the exported compiler runtime entry. The field, phase
// dispatch and unresolved exit are all product code; the test only supplies a deterministic
// collector answer so no workload sampling is involved.

#include <cstring>
#include <csignal>
#include <dlfcn.h>
#include <mutex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "gc_heap_fixture.hpp"

#include "Common/Runtime.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Barrier/Barrier.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Common/ColourPredicates.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/M0ExitDiagnostics.h"
#include "Heap/WCollector/IdleBarrier.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "gc_unittest.hpp"

// Test-only access to the existing private root-fix entry; this changes no product declaration,
// export or inlining decision. The called symbol is still WCollector from libcangjie-runtime.so.
#define private public
#include "Heap/WCollector/WCollector.h"
#undef private

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

extern "C" MapleRuntime::ObjectPtr CJ_MCC_ReadRefField(
    MapleRuntime::ObjectPtr obj, MapleRuntime::RefField<false>* field);
extern "C" void CJ_MCC_ReadStructField(MapleRuntime::MAddress dstPtr, MapleRuntime::ObjectPtr obj,
                                        MapleRuntime::MAddress srcField, size_t size, MapleRuntime::GCTib gctib);
extern "C" void CJ_MCC_ReadStaticStruct(MapleRuntime::MAddress dstPtr, size_t dstSize,
                                         MapleRuntime::MAddress srcPtr, size_t srcSize,
                                         MapleRuntime::GCTib gctib);
extern "C" void CJ_MCC_ArrayCopyRef(MapleRuntime::ObjectPtr dstObj, MapleRuntime::MAddress dstField,
                                     size_t dstSize, MapleRuntime::ObjectPtr srcObj,
                                     MapleRuntime::MAddress srcField, size_t srcSize);
extern "C" void CJ_MCC_ArrayCopyStruct(MapleRuntime::ObjectPtr dstObj, MapleRuntime::MAddress dstField,
                                        size_t dstSize, MapleRuntime::ObjectPtr srcObj,
                                        MapleRuntime::MAddress srcField, size_t srcSize);
extern "C" void MCC_WriteStaticStruct(MapleRuntime::MAddress dst, size_t dstLen,
                                       MapleRuntime::MAddress src, size_t srcLen,
                                       const MapleRuntime::GCTib gctib);

namespace {

class ProductForwardingApi {
public:
    static ProductForwardingApi& Get()
    {
        static ProductForwardingApi api;
        return api;
    }

    void Initialize(MAddress start, size_t size, size_t unit) const { initialize(start, size, unit); }
    bool PreparePublicationGeneration(MAddress start, size_t size) const
    {
        return preparePublicationGeneration(start, size);
    }
    bool InstallPublicationBeforeCopy(MAddress start, size_t size, RegionInfo* region) const
    {
        return installPublicationBeforeCopy(start, size, region);
    }
    MAddress InsertMapping(RegionInfo* region, MAddress from, MAddress to) const
    {
        ForwardingTable::Publication publication = ensurePublicationBeforeCopy(region, from);
        GC_EXPECT_TRUE(static_cast<bool>(publication));
        return insertMapping(publication, from, to);
    }
    void Remove(MAddress start, size_t size) const { remove(start, size); }
    void ClearEntries(MAddress start, size_t size) const { clearEntries(start, size); }
    void ReclaimRetired() const { reclaimRetired("gc-unit-explicit-coverage"); }
    ZForwarding* GetEntries(MAddress from) const { return getEntries(from); }
    MAddress FindRetiredTo(MAddress from) const { return findRetiredTo(from); }

private:
    template<typename Fn>
    static Fn Resolve(void* handle, const char* name)
    {
        void* symbol = dlsym(handle, name);
        GC_EXPECT_TRUE(symbol != nullptr);
        return reinterpret_cast<Fn>(symbol);
    }

    ProductForwardingApi()
    {
        // run_standalone.sh deliberately compiles another ForwardingTable.cpp for older tests.
        // Bind these constructors to the already-loaded product SO explicitly, so the receipt
        // producer and M0ExitDiagnostics consumer cannot accidentally use different globals.
        handle = dlopen("libcangjie-runtime.so", RTLD_NOW | RTLD_NOLOAD);
        GC_EXPECT_TRUE(handle != nullptr);
        initialize = Resolve<InitializeFn>(handle, "_ZN12MapleRuntime15ForwardingTable10InitializeEmmm");
        preparePublicationGeneration = Resolve<PreparePublicationGenerationFn>(
            handle, "_ZN12MapleRuntime15ForwardingTable28PreparePublicationGenerationEmm");
        installPublicationBeforeCopy = Resolve<InstallPublicationBeforeCopyFn>(
            handle, "_ZN12MapleRuntime15ForwardingTable28InstallPublicationBeforeCopyEmmPNS_10RegionInfoE");
        ensurePublicationBeforeCopy = Resolve<EnsurePublicationBeforeCopyFn>(
            handle, "_ZN12MapleRuntime15ForwardingTable27EnsurePublicationBeforeCopyEPNS_10RegionInfoEm");
        insertMapping = Resolve<InsertMappingFn>(
            handle, "_ZN12MapleRuntime15ForwardingTable13InsertMappingERKNS0_11PublicationEmm");
        remove = Resolve<RangeFn>(handle, "_ZN12MapleRuntime15ForwardingTable6RemoveEmm");
        clearEntries = Resolve<RangeFn>(handle, "_ZN12MapleRuntime15ForwardingTable12ClearEntriesEmm");
        reclaimRetired =
            Resolve<ReclaimFn>(handle, "_ZN12MapleRuntime15ForwardingTable14ReclaimRetiredEPKc");
        getEntries = Resolve<GetEntriesFn>(handle, "_ZN12MapleRuntime15ForwardingTable10GetEntriesEm");
        findRetiredTo = Resolve<FindRetiredToFn>(handle, "_ZN12MapleRuntime15ForwardingTable13FindRetiredToEm");
    }

    using InitializeFn = void (*)(MAddress, size_t, size_t);
    using PreparePublicationGenerationFn = bool (*)(MAddress, size_t);
    using InstallPublicationBeforeCopyFn = bool (*)(MAddress, size_t, RegionInfo*);
    using EnsurePublicationBeforeCopyFn = ForwardingTable::Publication (*)(RegionInfo*, MAddress);
    using InsertMappingFn = MAddress (*)(const ForwardingTable::Publication&, MAddress, MAddress);
    using RangeFn = void (*)(MAddress, size_t);
    using ReclaimFn = void (*)(const char*);
    using GetEntriesFn = ZForwarding* (*)(MAddress);
    using FindRetiredToFn = MAddress (*)(MAddress);

    void* handle = nullptr;
    InitializeFn initialize = nullptr;
    PreparePublicationGenerationFn preparePublicationGeneration = nullptr;
    InstallPublicationBeforeCopyFn installPublicationBeforeCopy = nullptr;
    EnsurePublicationBeforeCopyFn ensurePublicationBeforeCopy = nullptr;
    InsertMappingFn insertMapping = nullptr;
    RangeFn remove = nullptr;
    RangeFn clearEntries = nullptr;
    ReclaimFn reclaimRetired = nullptr;
    GetEntriesFn getEntries = nullptr;
    FindRetiredToFn findRetiredTo = nullptr;
};

class NoAnswerCollector final : public Collector {
public:
    BaseObject* answer = nullptr;
    void Init() override {}
    void RunGarbageCollection(uint64_t, GCReason) override {}
    bool ShouldIgnoreRequest(GCRequest&) override { return false; }
    FindToVersionResult FindToVersion(BaseObject*) const override
    {
        return answer == nullptr ? FindToVersionResult::NotForwarded() : FindToVersionResult::Found(answer);
    }
    BaseObject* ResolveStoreValue(BaseObject* ref) const override
    {
        return answer == nullptr ? ref : answer;
    }
    bool TryUpdateRefField(BaseObject*, RefField<>&, BaseObject*&) const override { return false; }
    bool IsOldPointer(RefField<>&) const override { return false; }
    bool IsFromObject(BaseObject*) const override { return false; }
    bool IsGhostFromObject(BaseObject*) const override { return false; }
    bool IsUnmovableFromObject(BaseObject*) const override { return false; }
    RefField<> GetAndTryTagRefField(BaseObject* object) const override
    {
        const uintptr_t remap = ColourPredicates::current_remapped(static_cast<uintptr_t>(::g_cjLoadBadMask));
        return RefField<>(GcUnit::ColouredPointer(object, remap));
    }
    ZGenerationId remap_generation(RefField<>&) const override { return ZGenerationId::old; }
    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId) const override { return object; }
};

class InstalledBarrierScope {
public:
    explicit InstalledBarrierScope(Barrier& barrier) : previous(Heap::currentBarrierPtr), installed(&barrier)
    {
        Heap::currentBarrierPtr = &installed;
    }
    ~InstalledBarrierScope() { Heap::currentBarrierPtr = previous; }

private:
    Barrier** previous;
    Barrier* installed;
};

struct ReadEntryFixture {
    ReadEntryFixture() : barrier(collector, rememberedSet), installed(barrier)
    {
        rememberedSet.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        static std::once_flag globalRememberedSet;
        std::call_once(globalRememberedSet, [this]() {
            Heap::GetHeap().GetRememberedSet().Initialize(
                heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE);
        });
        heap.region0->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj0) + 128);
        heap.region1->SetRegionAllocPtr(reinterpret_cast<MAddress>(heap.obj1) + 128);
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(heap.obj1) + TYPEINFO_PTR_SIZE);
        field->StoreColoured(GcUnit::StoreGoodPointer(heap.obj0));
    }

    void MakeFieldLoadBad()
    {
        const uintptr_t staleRemaps = static_cast<uintptr_t>(::g_cjLoadBadMask) & REMAP_COLOUR_MASK;
        const uintptr_t staleRemap = staleRemaps & (~staleRemaps + 1);
        GC_EXPECT_TRUE(staleRemap != 0);
        field->StoreColoured(GcUnit::ColouredPointer(heap.obj0, staleRemap));
    }

    GcHeapFixture heap;
    NoAnswerCollector collector;
    RememberedSet rememberedSet;
    IdleBarrier barrier;
    InstalledBarrierScope installed;
    RefField<>* field = nullptr;
};

struct RefArraySource {
    explicit RefArraySource(ReadEntryFixture& fx)
    {
        std::memset(arrayTypeStorage, 0, sizeof(arrayTypeStorage));
        arrayType = reinterpret_cast<TypeInfo*>(arrayTypeStorage);
        arrayType->SetType(TypeKind::TYPE_KIND_RAWARRAY);
        arrayType->SetComponentTypeInfo(fx.heap.typeInfo);
        TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(
            reinterpret_cast<uintptr_t>(arrayTypeStorage), sizeof(arrayTypeStorage));

        array = reinterpret_cast<MArray*>(fx.heap.obj1);
        array->SetClassInfo(arrayType);
        array->SetLength(1);
        field = &HeapSlotAt<>(reinterpret_cast<MAddress>(array) + MArray::GetContentOffset());
        field->StoreColoured(GcUnit::StoreGoodPointer(fx.heap.obj0));
    }

    alignas(TypeInfo) unsigned char arrayTypeStorage[sizeof(TypeInfo)];
    TypeInfo* arrayType = nullptr;
    MArray* array = nullptr;
    RefField<>* field = nullptr;
};

struct RootEntryFixture {
    class ProductRootRuntime final : public Runtime {
    public:
        static void Ensure()
        {
            static ProductRootRuntime runtimeContainer;
            (void)runtimeContainer;
        }

        RuntimeParam GetRuntimeParam() const override { return params; }
        void SetGCThreshold(uint64_t) override {}

    private:
        ProductRootRuntime()
        {
            runtime = this;
            mutatorManager = &manager;
            concurrencyModel = &concurrency;
            manager.Init();
            const ConcurrencyParam concurrencyParam = { 1024, 64, 1 };
            concurrency.Init(concurrencyParam);
        }

        RuntimeParam params = {};
        MutatorManager manager;
        Concurrency concurrency;
    };

    RootEntryFixture() : collector(Heap::GetHeap().GetAllocator(), Heap::GetHeap().GetCollectorResources())
    {
        ProductRootRuntime::Ensure();
        auto& forwarding = ProductForwardingApi::Get();
        forwarding.Initialize(heap.heapStart, GcHeapFixture::kUnits * RegionInfo::UNIT_SIZE,
                              RegionInfo::UNIT_SIZE);
        heap.region0->SetRegionType(RegionInfo::RegionType::FROM_REGION);
        heap.region0->SetInGhostRegion(1);
        heap.region0->SetRouteState(RegionInfo::ROUTED);
        if (!forwarding.InstallPublicationBeforeCopy(
                heap.region0->GetRegionStart(), heap.region0->GetRegionSize(), heap.region0)) {
            GC_EXPECT_TRUE(forwarding.PreparePublicationGeneration(
                heap.region0->GetRegionStart(), heap.region0->GetRegionSize()));
            GC_EXPECT_TRUE(forwarding.InstallPublicationBeforeCopy(
                heap.region0->GetRegionStart(), heap.region0->GetRegionSize(), heap.region0));
        }
        StorePlain(root, from_object(heap.obj0));
        registeredRoots[0] = &root;
        Heap::GetHeap().RegisterStaticRoots(reinterpret_cast<Uptr>(registeredRoots), 1);
    }

    ~RootEntryFixture()
    {
        Heap::GetHeap().UnregisterStaticRoots(reinterpret_cast<Uptr>(registeredRoots), 1);
        auto& forwarding = ProductForwardingApi::Get();
        forwarding.Remove(heap.region0->GetRegionStart(), heap.region0->GetRegionSize());
        forwarding.ClearEntries(heap.region0->GetRegionStart(), heap.region0->GetRegionSize());
        forwarding.ReclaimRetired();
        // Keep the standalone-only duplicate clean as well. In the CMake product-linked test these
        // calls resolve to the same already-cleared product instance and are harmless no-ops.
        ForwardingTable::Remove(heap.region0->GetRegionStart(), heap.region0->GetRegionSize());
        ForwardingTable::ClearEntries(heap.region0->GetRegionStart(), heap.region0->GetRegionSize());
        ForwardingTable::ReclaimRetired("gc-unit-explicit-coverage");
    }

    MAddress PublishUnusableActiveWitness()
    {
        auto& forwarding = ProductForwardingApi::Get();
        const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
        const MAddress to = reinterpret_cast<MAddress>(heap.obj1);
        GC_EXPECT_EQ(forwarding.InsertMapping(heap.region0, from, to), to);
        GC_EXPECT_TRUE(forwarding.GetEntries(from) != nullptr);
        GC_EXPECT_EQ(forwarding.GetEntries(from)->find(from), to);
        GC_EXPECT_EQ(forwarding.FindRetiredTo(from), static_cast<MAddress>(0));
        *reinterpret_cast<uint64_t*>(heap.obj1) = 0; // product FindToVersion must reject this receipt
        return to;
    }

    MAddress PublishUnusableRetiredWitness()
    {
        auto& forwarding = ProductForwardingApi::Get();
        const MAddress to = PublishUnusableActiveWitness();
        forwarding.ClearEntries(heap.region0->GetRegionStart(), heap.region0->GetRegionSize());
        const MAddress from = reinterpret_cast<MAddress>(heap.obj0);
        GC_EXPECT_TRUE(forwarding.GetEntries(from) == nullptr);
        GC_EXPECT_EQ(forwarding.FindRetiredTo(from), to);
        return to;
    }

    GcHeapFixture heap;
    WCollector collector;
    RootSlot root;
    RootSlot* registeredRoots[1] = {};
};

} // namespace

// loadfc: a zero-header from-address must never be handed out; every one of these arms pins the
// controlled [LOADFC] abort (SIGABRT) of a specific product load exit, and each has a healthy
// positive arm asserting the same exit still returns normally.
namespace {
void ExpectControlledAbort(const std::function<void()>& body)
{
    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        body();
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}
} // namespace

GC_OTHER_VM_TEST(M0Exit, ReadRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0; // deterministic cleared-from state

    ExpectControlledAbort([&]() { (void)CJ_MCC_ReadRefField(fx.heap.obj1, fx.field); });
}

GC_TEST(M0Exit, ReadRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, ReadRuntimeEntryFailsClosedOnForwardedWithoutMapping)
{
    ReadEntryFixture fx;
    BaseObject* publishedCopy = fx.heap.PlaceObject(fx.heap.heapStart + 256);
    std::memcpy(publishedCopy, fx.heap.obj0, fx.heap.obj0->GetSize());
    GC_EXPECT_TRUE(publishedCopy->IsValidObject());
    // Copy bytes exist, and FORWARDED is the product's post-copy publication witness. Deliberately
    // omit the active/retired lookup entry: an unresolvable FORWARDED hand-out is exactly the
    // zRelocate.cpp:412-416 shape ZGC forbids.
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    fx.MakeFieldLoadBad();

    ExpectControlledAbort([&]() { (void)CJ_MCC_ReadRefField(fx.heap.obj1, fx.field); });
}

GC_OTHER_VM_TEST(M0Exit, ReadRuntimeEntryForwardedWithMappingResolvesToVersion)
{
    ReadEntryFixture fx;
    BaseObject* publishedCopy = fx.heap.PlaceObject(fx.heap.heapStart + 256);
    std::memcpy(publishedCopy, fx.heap.obj0, fx.heap.obj0->GetSize());
    GC_EXPECT_TRUE(publishedCopy->IsValidObject());
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    fx.collector.answer = publishedCopy;
    fx.MakeFieldLoadBad();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    // Positive control: a resolvable to-version is handed out, proving the abort above is precise
    // (missing mapping, not the FORWARDED colour itself).
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(publishedCopy));
}

GC_TEST(M0Exit, ReadRuntimeEntryResolvedPathCountsM0Exit)
{
    ReadEntryFixture fx;
    Barrier baseBarrier(fx.collector, fx.rememberedSet);
    InstalledBarrierScope baseInstalled(baseBarrier);
    BaseObject* publishedCopy = fx.heap.PlaceObject(fx.heap.heapStart + 256);
    std::memcpy(publishedCopy, fx.heap.obj0, fx.heap.obj0->GetSize());
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    fx.collector.answer = publishedCopy;
    fx.MakeFieldLoadBad();
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(publishedCopy));
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s1, before.s1 + 1);
    GC_EXPECT_EQ(after.readBarrier, before.readBarrier + 1);
}

GC_OTHER_VM_TEST(M0Exit, ReadStructRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0;
    RootSlot copied;

    ExpectControlledAbort([&]() {
        CJ_MCC_ReadStructField(reinterpret_cast<MAddress>(&copied), fx.heap.obj1,
                               reinterpret_cast<MAddress>(fx.field), sizeof(*fx.field), GCTib {});
    });
}

GC_TEST(M0Exit, ReadStructRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RootSlot copied;

    CJ_MCC_ReadStructField(reinterpret_cast<MAddress>(&copied), fx.heap.obj1,
                           reinterpret_cast<MAddress>(fx.field), sizeof(*fx.field), GCTib {});

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, ReadStructRuntimeEntryFailsClosedOnForwardedWithoutMapping)
{
    ReadEntryFixture fx;
    BaseObject* publishedCopy = fx.heap.PlaceObject(fx.heap.heapStart + 256);
    std::memcpy(publishedCopy, fx.heap.obj0, fx.heap.obj0->GetSize());
    GC_EXPECT_TRUE(publishedCopy->IsValidObject());
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    RootSlot copied;

    ExpectControlledAbort([&]() {
        CJ_MCC_ReadStructField(reinterpret_cast<MAddress>(&copied), fx.heap.obj1,
                               reinterpret_cast<MAddress>(fx.field), sizeof(*fx.field), GCTib {});
    });
}

GC_OTHER_VM_TEST(M0Exit, ReadStaticStructRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0;
    RootSlot copied;

    ExpectControlledAbort([&]() {
        CJ_MCC_ReadStaticStruct(reinterpret_cast<MAddress>(&copied), sizeof(copied),
                                reinterpret_cast<MAddress>(fx.field), sizeof(*fx.field), fx.heap.obj1->GetGCTib());
    });
}

GC_TEST(M0Exit, ReadStaticStructRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RootSlot copied;

    CJ_MCC_ReadStaticStruct(reinterpret_cast<MAddress>(&copied), sizeof(copied),
                            reinterpret_cast<MAddress>(fx.field), sizeof(*fx.field), fx.heap.obj1->GetGCTib());

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, CopyStructArrayRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    RefArraySource source(fx);
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0;
    RootSlot copied;

    ExpectControlledAbort([&]() {
        CJ_MCC_ArrayCopyStruct(nullptr, reinterpret_cast<MAddress>(&copied), sizeof(copied), source.array,
                               reinterpret_cast<MAddress>(source.field), sizeof(*source.field));
    });
}

GC_TEST(M0Exit, CopyStructArrayRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    RefArraySource source(fx);
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RootSlot copied;

    CJ_MCC_ArrayCopyStruct(nullptr, reinterpret_cast<MAddress>(&copied), sizeof(copied), source.array,
                           reinterpret_cast<MAddress>(source.field), sizeof(*source.field));

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, CopyRefArrayForwardRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0;
    static RootSlot copied;
    const MAddress dst = reinterpret_cast<MAddress>(&copied);
    const MAddress src = reinterpret_cast<MAddress>(fx.field);
    GC_EXPECT_TRUE(dst < src);

    ExpectControlledAbort([&]() {
        CJ_MCC_ArrayCopyRef(nullptr, dst, sizeof(copied), fx.heap.obj1, src, sizeof(*fx.field));
    });
}

GC_TEST(M0Exit, CopyRefArrayForwardRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    static RootSlot copied;
    const MAddress dst = reinterpret_cast<MAddress>(&copied);
    const MAddress src = reinterpret_cast<MAddress>(fx.field);
    GC_EXPECT_TRUE(dst < src);

    CJ_MCC_ArrayCopyRef(nullptr, dst, sizeof(copied), fx.heap.obj1, src, sizeof(*fx.field));

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, CopyRefArrayBackwardRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0;
    RootSlot copied;
    const MAddress dst = reinterpret_cast<MAddress>(&copied);
    const MAddress src = reinterpret_cast<MAddress>(fx.field);
    GC_EXPECT_TRUE(dst > src);

    ExpectControlledAbort([&]() {
        CJ_MCC_ArrayCopyRef(nullptr, dst, sizeof(copied), fx.heap.obj1, src, sizeof(*fx.field));
    });
}

GC_TEST(M0Exit, CopyRefArrayBackwardRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RootSlot copied;
    const MAddress dst = reinterpret_cast<MAddress>(&copied);
    const MAddress src = reinterpret_cast<MAddress>(fx.field);
    GC_EXPECT_TRUE(dst > src);

    CJ_MCC_ArrayCopyRef(nullptr, dst, sizeof(copied), fx.heap.obj1, src, sizeof(*fx.field));

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, WriteStaticStructRuntimeEntryFailsClosedOnZeroHeaderFromAddress)
{
    ReadEntryFixture fx;
    *reinterpret_cast<uint64_t*>(fx.heap.obj0) = 0;
    RefField<> source(fx.field->GetFieldValue());
    RootSlot copied;

    ExpectControlledAbort([&]() {
        MCC_WriteStaticStruct(reinterpret_cast<MAddress>(&copied), sizeof(copied),
                              reinterpret_cast<MAddress>(&source), sizeof(source), fx.heap.obj1->GetGCTib());
    });
}

GC_TEST(M0Exit, WriteStaticStructRuntimeEntryHealthyTargetStillReturnsNormally)
{
    ReadEntryFixture fx;
    GC_EXPECT_TRUE(fx.heap.obj0->IsValidObject());
    RefField<> source(fx.field->GetFieldValue());
    RootSlot copied;

    MCC_WriteStaticStruct(reinterpret_cast<MAddress>(&copied), sizeof(copied),
                          reinterpret_cast<MAddress>(&source), sizeof(source), fx.heap.obj1->GetGCTib());

    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(copied.LoadPlain())), reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_TEST(M0Exit, ReadRuntimeEntryResolvedNormalPathIsSilent)
{
    ReadEntryFixture fx;
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    fx.collector.answer = fx.heap.obj1;
    fx.MakeFieldLoadBad();
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    ObjectPtr got = CJ_MCC_ReadRefField(fx.heap.obj1, fx.field);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(got), reinterpret_cast<uintptr_t>(fx.heap.obj1));
    GC_EXPECT_EQ(after.total, before.total);
    GC_EXPECT_EQ(after.s0, before.s0);
    GC_EXPECT_EQ(after.s1, before.s1);
}

GC_OTHER_VM_TEST(M0Exit, RootFixRuntimeEnumerationClassifiesNoCopyAsS0)
{
    RootEntryFixture fx;
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    fx.collector.FixMinorRootSlots(nullptr);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s0, before.s0 + 1);
    GC_EXPECT_EQ(after.rootFix, before.rootFix + 1);
    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(fx.root.LoadPlain())),
                 reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, RootFixRuntimeEnumerationFailsClosedWithoutPublishedMapping)
{
    RootEntryFixture fx;
    std::memcpy(fx.heap.obj1, fx.heap.obj0, fx.heap.obj0->GetSize());
    fx.heap.obj0->SetStateCode(ObjectState::FORWARDED);
    GC_EXPECT_TRUE(fx.collector.FindToVersion(fx.heap.obj0).state() ==
                   FindToVersionResult::State::Unavailable);

    const pid_t child = fork();
    GC_EXPECT_TRUE(child >= 0);
    if (child == 0) {
        (void)signal(SIGABRT, SIG_DFL);
        fx.collector.FixMinorRootSlots(nullptr);
        _exit(0);
    }
    int status = 0;
    GC_EXPECT_EQ(waitpid(child, &status, 0), child);
    GC_EXPECT_TRUE(WIFSIGNALED(status));
    GC_EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

GC_OTHER_VM_TEST(M0Exit, RootFixClassifiesActiveOnlyUnusableCopyAsS1)
{
    RootEntryFixture fx;
    (void)fx.PublishUnusableActiveWitness();
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    fx.collector.FixMinorRootSlots(nullptr);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s1, before.s1 + 1);
    GC_EXPECT_EQ(after.activeWitness, before.activeWitness + 1);
    GC_EXPECT_EQ(after.retiredWitness, before.retiredWitness);
    GC_EXPECT_EQ(after.copyPublishedWitness, before.copyPublishedWitness);
    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(fx.root.LoadPlain())),
                 reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

GC_OTHER_VM_TEST(M0Exit, RootFixClassifiesRetiredOnlyUnusableCopyAsS1)
{
    RootEntryFixture fx;
    (void)fx.PublishUnusableRetiredWitness();
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();

    fx.collector.FixMinorRootSlots(nullptr);

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(after.total, before.total + 1);
    GC_EXPECT_EQ(after.s1, before.s1 + 1);
    GC_EXPECT_EQ(after.activeWitness, before.activeWitness);
    GC_EXPECT_EQ(after.retiredWitness, before.retiredWitness + 1);
    GC_EXPECT_EQ(after.copyPublishedWitness, before.copyPublishedWitness);
    GC_EXPECT_EQ(static_cast<uintptr_t>(raw(fx.root.LoadPlain())),
                 reinterpret_cast<uintptr_t>(fx.heap.obj0));
}

// loadfc: the zero-header runtime-entry arm above now aborts by design, so the detailed-sample
// cap is pinned directly against the product sampler (M0ExitDiagnostics::Note); entry wiring of
// the resolved path stays covered by ReadRuntimeEntryForwardedWithMappingResolvesToVersion.
GC_TEST(M0Exit, SamplerCapsDetailedSamplesAndCountsSuppression)
{
    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_TRUE(before.sampled <= M0ExitDiagnostics::kDetailedSampleLimit);
    const uint64_t toLimit = M0ExitDiagnostics::kDetailedSampleLimit - before.sampled;
    constexpr uint64_t kBeyondLimit = 3;
    const uint64_t attempts = toLimit + kBeyondLimit;

    for (uint64_t i = 0; i < attempts; ++i) {
        M0ExitDiagnostics::Note(M0ExitDiagnostics::Exit::ReadBarrier, nullptr, nullptr, nullptr, 0);
    }

    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    GC_EXPECT_EQ(after.total, before.total + attempts);
    GC_EXPECT_EQ(after.sampled, M0ExitDiagnostics::kDetailedSampleLimit);
    GC_EXPECT_EQ(after.suppressed, before.suppressed + kBeyondLimit);
}
