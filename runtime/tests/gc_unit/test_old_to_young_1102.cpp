// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstdio>
#include <dlfcn.h>

#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zBarrier.hpp"
#include "Heap/z/zHeap.hpp"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/RefField.inline.h"

extern "C" void CJ_MCC_StoreBarrierOnHeapField(volatile MapleRuntime::zpointer* slot);

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {

class InstalledMutatorScope final {
public:
    explicit InstalledMutatorScope(Mutator& mutator) : saved(ThreadLocal::GetMutator())
    {
        ThreadLocal::SetMutator(&mutator);
    }
    ~InstalledMutatorScope() { ThreadLocal::SetMutator(saved); }
private:
    Mutator* saved;
};

}

using PromoteFn = void (*)(volatile zpointer*);

PromoteFn ProductPromoteBarrier()
{
    void* so = dlopen("libcangjie-runtime.so", RTLD_NOLOAD | RTLD_NOW);
    if (so == nullptr) {
        so = dlopen("libcangjie-runtime.so", RTLD_NOW);
    }
    if (so == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<PromoteFn>(dlsym(
        so, "_ZN12MapleRuntime8ZBarrier34promote_barrier_on_young_oop_fieldEPVNS_8zpointerE"));
}

// Product SO entry. dlopen(NOLOAD) selects the loaded runtime, not a local
// weak copy emitted because another header pulled the inline definition in.
GC_TEST(OldToYoung1102, PromotedNullIsStoreBadAndSlowStoreRemembersSlot)
{
    GcHeapFixture fx;
    RememberedSet rs;
    rs.Initialize(fx.heapStart, GcHeapFixture::kUnits * ZGranuleSize);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(zpointer::null);
    PromoteFn promote = ProductPromoteBarrier();
    std::fprintf(stderr, "OLD_TO_YOUNG_1102_PROMOTE_FN fn=%p\n", reinterpret_cast<void*>(promote));
    if (promote == nullptr) {
        GC_EXPECT_TRUE(false);
        return;
    }
    promote(reinterpret_cast<volatile zpointer*>(&field));
    const zpointer promoted = field.GetFieldValue();
    const zpointer storeGoodNull = ZAddress::store_good(zaddress::null);
    const bool promotedBad = (raw(promoted) & ::g_cjStoreBadMask) != 0;
    const bool storeGoodNotBad = (raw(storeGoodNull) & ::g_cjStoreBadMask) == 0;
    const bool colored = promoted == color_null();
    std::fprintf(stderr, "OLD_TO_YOUNG_1102_PROMOTED raw=%zx bad=%d store_good_null_not_bad=%d colored=%d\n",
                 raw(promoted), promotedBad, storeGoodNotBad, colored);

    Mutator mutator;
    InstalledMutatorScope installed(mutator);
    auto* buffer = ThreadLocal::GetGCData().storeBarrierBuffer;
    buffer->clear();
    buffer->Initialize(::g_cjStoreGoodMask);
    CJ_MCC_StoreBarrierOnHeapField(reinterpret_cast<volatile zpointer*>(&field));
    buffer->Flush();
    const bool remembered = SlotPageRemembered(reinterpret_cast<MAddress>(&field));
    std::fprintf(stderr, "OLD_TO_YOUNG_1102_REMEMBERED remembered=%d\n", remembered);
    GC_EXPECT_TRUE(promotedBad && storeGoodNotBad && colored && remembered);
}

GC_TEST(OldToYoung1102, PromotedStoreGoodYoungTargetEntersRemset)
{
    GcHeapFixture fx;
    fx.region1->reset(PageAge::eden);
    RememberedSet rs;
    rs.Initialize(fx.heapStart, GcHeapFixture::kUnits * ZGranuleSize);
    auto& field = HeapSlotAt<>(reinterpret_cast<MAddress>(fx.obj0) + TYPEINFO_PTR_SIZE);
    field.StoreColoured(StoreGoodPointer(fx.obj1));
    Heap::GetHeap().page_allocator().RememberPromotedObject(fx.obj0);
    const bool remembered = SlotPageRemembered(reinterpret_cast<MAddress>(&field));
    std::fprintf(stderr, "OLD_TO_YOUNG_1102_PROMOTE_YOUNG remembered=%d young=%d\n",
                 remembered, fx.region1->IsYoungRegion());
    GC_EXPECT_TRUE(remembered && fx.region1->IsYoungRegion());
}
