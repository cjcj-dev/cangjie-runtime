// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/syscall.h>

#include "Common/ColourMask.h"
#include "Base/Log.h"
#include "BaseObject.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/FinalizerProcessor.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MArray.inline.h"
#include "ObjectModel/MObject.h"
#include "ObjectModel/MObject.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "Heap/Verify/PlainCensus.h"

namespace MapleRuntime {
namespace {
constexpr size_t kLockOwnerCap = 4096;
struct LockOwnerSlot {
    std::atomic<uintptr_t> obj{ 0 };
    std::atomic<uint64_t> tid{ 0 };
    std::atomic<void*> ra{ nullptr };
};
LockOwnerSlot g_lockOwners[kLockOwnerCap];

size_t OwnerIndex(const void* obj)
{
    auto p = reinterpret_cast<uintptr_t>(obj);
    return (p >> 4) & (kLockOwnerCap - 1);
}

uint64_t ThisTid() { return static_cast<uint64_t>(syscall(SYS_gettid)); }

void NoteLockOwner(BaseObject* obj)
{
    LockOwnerSlot& s = g_lockOwners[OwnerIndex(obj)];
    s.obj.store(reinterpret_cast<uintptr_t>(obj), std::memory_order_relaxed);
    s.tid.store(ThisTid(), std::memory_order_relaxed);
    s.ra.store(__builtin_return_address(0), std::memory_order_relaxed);
}

void ClearLockOwner(BaseObject* obj)
{
    LockOwnerSlot& s = g_lockOwners[OwnerIndex(obj)];
    if (s.obj.load(std::memory_order_relaxed) == reinterpret_cast<uintptr_t>(obj)) {
        s.tid.store(0, std::memory_order_relaxed);
        s.ra.store(nullptr, std::memory_order_relaxed);
        s.obj.store(0, std::memory_order_relaxed);
    }
}

void ReportUnlockNotLocked(BaseObject* obj, ObjectState current, ObjectState want)
{
    static std::atomic<size_t> n{ 0 };
    size_t hit = n.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t word = 0;
    if (obj != nullptr) {
        word = __atomic_load_n(reinterpret_cast<uint64_t*>(obj), __ATOMIC_ACQUIRE);
    }
    unsigned sc = current.GetStateCode();
    RegionInfo* region =
        obj == nullptr ? nullptr : RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(obj));
    unsigned rtype = region == nullptr ? 255u : static_cast<unsigned>(region->GetRegionType());
    unsigned unit = region == nullptr ? 255u : static_cast<unsigned>(region->GetUnitRole());
    unsigned young = region == nullptr ? 0u : static_cast<unsigned>(region->IsYoungRegion());
    MAddress rstart = region == nullptr ? 0 : region->GetRegionStart();
    MAddress rend = region == nullptr ? 0 : region->GetRegionEnd();
    LockOwnerSlot& s = g_lockOwners[OwnerIndex(obj)];
    uintptr_t ownerObj = s.obj.load(std::memory_order_relaxed);
    uint64_t ownerTid = s.tid.load(std::memory_order_relaxed);
    void* ownerRa = s.ra.load(std::memory_order_relaxed);
    const bool ownerMatch = ownerObj == reinterpret_cast<uintptr_t>(obj);
    std::fprintf(stderr,
                 "[GCV2][lockstate] UNLOCK_NOT_LOCKED n=%zu obj=%p word=%#llx stateCode=%u want=%u "
                 "tid=%llu owner_tid=%llu owner_match=%d owner_ra=%p "
                 "region=%p start=%#zx end=%#zx regionType=%u unitRole=%u young=%u "
                 "ra0=%p ra1=%p ra2=%p\n",
                 hit, static_cast<void*>(obj), static_cast<unsigned long long>(word), sc,
                 static_cast<unsigned>(want.GetStateCode()), static_cast<unsigned long long>(ThisTid()),
                 static_cast<unsigned long long>(ownerMatch ? ownerTid : 0), ownerMatch ? 1 : 0, ownerRa,
                 static_cast<void*>(region), static_cast<size_t>(rstart), static_cast<size_t>(rend), rtype, unit,
                 young, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
    std::fflush(stderr);
    LOG(RTLOG_ERROR,
        "[GCV2][lockstate] UNLOCK_NOT_LOCKED n=%zu obj=%p word=%#llx stateCode=%u want=%u tid=%llu "
        "owner_tid=%llu owner_match=%d",
        hit, obj, static_cast<unsigned long long>(word), sc, static_cast<unsigned>(want.GetStateCode()),
        static_cast<unsigned long long>(ThisTid()), static_cast<unsigned long long>(ownerMatch ? ownerTid : 0),
        ownerMatch ? 1 : 0);
}
} // namespace

bool BaseObject::TryLockObject(const StateWord curWord)
{
    bool ok = stateWord.TryLockStateWord(curWord.GetObjectState());
    if (ok) {
        NoteLockOwner(this);
    }
    return ok;
}

void BaseObject::UnlockObject(const ObjectState newState)
{
    ObjectState current = GetObjectState();
    if (!current.IsLockedState()) {
        ReportUnlockNotLocked(this, current, newState);
    } else {
        ClearLockOwner(this);
    }
    stateWord.UnlockStateWord(newState);
}

// COLOUR_WRITEBACK_AUDIT §六 判据 1：唯一落笔 choke（单一定义，默认关）。
// plaincensus: also feed fail-open writer counters (MRT_GCV2_PLAIN_WRITE_COUNT=1).
void AssertColouredWriteIfEnabled(const void* slot, MAddress newVal)
{
    static const bool assertOn = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_ASSERT_COLOURED_WRITES */;
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    static const bool countOn = []() {
        const char* v = static_cast<const char*>(nullptr) /* pinned-off:MRT_GCV2_PLAIN_WRITE_COUNT */;
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    if (LIKELY(!assertOn && !countOn)) {
        return;
    }
    if (!Heap::IsHeapAddress(slot)) {
        return;
    }
    if ((newVal & ((MAddress(1) << 48) - 1)) == 0) {
        return;
    }
    constexpr MAddress kColourMask = REMAP_COLOUR_MASK | MARKED_YOUNG_MASK | MARKED_OLD_MASK;
    bool hasColour = (newVal & kColourMask) != 0;
    bool tagged = ((newVal >> 48) & 1) != 0;
    bool loadGood = (newVal & static_cast<MAddress>(::g_cjLoadBadMask)) == 0;
    if (!hasColour) {
        NotePlainHeapWrite(slot, static_cast<uintptr_t>(newVal));
    }
    if (LIKELY(!assertOn)) {
        return;
    }
    CHECK_DETAIL(hasColour && (loadGood || tagged),
                 "MRT_GCV2_ASSERT_COLOURED_WRITES: plain/bad-colour heap ref write @%p val=%#zx "
                 "hasColour=%d loadGood=%d tagged=%d",
                 slot, newVal, hasColour, loadGood, tagged);
}

TypeInfo* BaseObject::GetTypeInfo() const { return stateWord.GetTypeInfo(); }

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void BaseObject::DumpObject(int logtype, bool isSimple) const
{
    static constexpr size_t DUMP_WORDS_PER_LINE = 4;
    size_t objSize = GetSize();

    DLOG(LogType(logtype), "[obj] %p %p %zu", this, GetTypeInfo(), objSize);
    if (isSimple) {
        return;
    }
    // dump more details
    size_t word = RoundUp(objSize, sizeof(void*)) / sizeof(void*);
    MAddress obj = reinterpret_cast<MAddress>(this);
    constexpr size_t bufferSize = 256;
    char buf[bufferSize];
    for (size_t i = 0; i < word; i += DUMP_WORDS_PER_LINE) {
        int index = sprintf_s(buf, sizeof(buf), "%zx: ", (obj + (i * sizeof(MAddress))));
        size_t bound = (i + DUMP_WORDS_PER_LINE) < word ? (i + DUMP_WORDS_PER_LINE) : word;
        for (size_t j = i; j < bound; j++) {
            index += sprintf_s(buf + index, sizeof(buf) - static_cast<size_t>(index), "%p ",
                               *reinterpret_cast<ObjectPtr*>(obj + (j * sizeof(MAddress))));
#ifdef USE_32BIT_REF
            index += sprintf_s(buf + index, sizeof(buf) - static_cast<size_t>(index), "%p ",
                               *reinterpret_cast<ObjectPtr*>(obj + (j * sizeof(MAddress) + sizeof(ObjectPtr))));
#endif // USE_32BIT_REF
        }
        DLOG(LogType(logtype), buf);
    }
}
#endif

static void ForEachRefFieldInNonArrayObject(ObjectPtr obj, const RefFieldVisitor& visitor)
{
    GCTib gcTib = obj->GetGCTib();
    // gcTib record payload data, skip the TypeInfo
    MAddress objAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    gcTib.ForEachBitmapWord(objAddr, visitor);
}

// Call func on each element in an object array.
static void ForEachElementInArray(ObjectPtr obj, const RefFieldVisitor& visitor)
{
    // take array length and content.
    MArray* mArray = reinterpret_cast<MArray*>(obj);
    MIndex arrayLengthVal = mArray->GetLength();
    TypeInfo* componentTypeInfo = mArray->GetComponentTypeInfo();
    if (componentTypeInfo->IsStructType()) {
        GCTib gcTib = componentTypeInfo->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(mArray) + MArray::GetContentOffset();
        for (MIndex i = 0; i < arrayLengthVal; ++i) {
            gcTib.ForEachBitmapWord(contentAddr, visitor);
            contentAddr += mArray->GetElementSize();
        }
    } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
               componentTypeInfo->IsInterface()) {
        HeapSlot<>* arrayContent = &HeapSlotAt<>(mArray->ConvertToCArray());
        // for each object in array.
        for (MIndex i = 0; i < arrayLengthVal; ++i) {
            visitor(arrayContent[i]);
        }
    } else {
        LOG(RTLOG_FATAL, "array object %p has wrong component type", mArray);
    }
}

void BaseObject::ForEachRefField(const RefFieldVisitor& visitor)
{
    TypeInfo* typeInfo = GetTypeInfo();
    if (typeInfo->HasRefField()) {
        if (UNLIKELY(typeInfo->IsRawArray())) {
            ForEachElementInArray(this, visitor);
        } else {
            ForEachRefFieldInNonArrayObject(this, visitor);
        }
    }
};

void BaseObject::ForEachRefInStruct(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd)
{
    TypeInfo* typeInfo = GetTypeInfo();
    if (typeInfo->HasRefField()) {
        if (UNLIKELY(typeInfo->IsRawArray())) {
            ForEachAggRefFieldInArray(visitor, aggStart, aggEnd);
        } else {
            ForEachAggRefFieldInNonArray(visitor, aggStart, aggEnd);
        }
    }
}

void BaseObject::ForEachAggRefFieldInArray(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd)
{
    // take array length and content.
    MArray* mArray = static_cast<MArray*>(this);
    MIndex arrayLen = mArray->GetLength();
    TypeInfo* component = mArray->GetComponentTypeInfo();
    if (component->IsStructType()) {
        GCTib gcTib = component->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(this) + MArray::GetContentOffset();
        size_t contentSize = mArray->GetElementSize();
        // MIndex is enough to describe the size;
        MIndex startIndex = static_cast<MIndex>((aggStart - contentAddr) / contentSize);
        size_t alignedStart = startIndex * contentSize + contentAddr;
        MRT_ASSERT((alignedStart + contentSize) >= aggEnd, "aggregate element is not align\n");
        MAddress currentAddr = alignedStart;
        for (U64 i = startIndex; (i < arrayLen) && (currentAddr < aggEnd); ++i) {
            gcTib.ForEachBitmapWordInRange(currentAddr, visitor, aggStart, aggEnd);
            currentAddr += contentSize;
        }
    } else {
        LOG(RTLOG_FATAL, "this interface mustn't be invoked by array whose element is not record");
    }
}

void BaseObject::ForEachAggRefFieldInNonArray(const RefFieldVisitor& visitor, MAddress aggStart, MAddress aggEnd) const
{
    // gcTib record payload data, skip the TypeInfo
    GetGCTib().ForEachBitmapWordInRange(reinterpret_cast<MAddress>(this) + TYPEINFO_PTR_SIZE, visitor, aggStart,
                                        aggEnd);
}

size_t BaseObject::GetSize() const
{
    TypeInfo* kls = GetTypeInfo();
    if (kls->IsArrayType()) {
        const MArray* mArray = reinterpret_cast<const MArray*>(this);
        size_t size = mArray->GetMArraySize();
        return MapleRuntime::AlignUp<size_t>(size, AllocatorUtils::ALLOC_ALIGNMENT);
    } else {
        return MapleRuntime::AlignUp<size_t>(kls->GetInstanceSize() + TYPEINFO_PTR_SIZE,
                                             AllocatorUtils::ALLOC_ALIGNMENT);
    }
}

void BaseObject::OnFinalizerCreated()
{
    Heap& heap = Heap::GetHeap();
    heap.GetCollector().MarkNewObject(this);
    Mutator* mutator = Mutator::GetMutator();
    if (mutator != nullptr) {
        mutator->AddLocalFinalizer(this);
    } else {
        heap.GetFinalizerProcessor().RegisterFinalizer(this);
    }
}

bool BaseObject::IsInTraceRegion() const
{
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<Uptr>(this));
    MRT_ASSERT(region != nullptr, "region is nullptr");
    return region->IsTraceRegion();
}

bool BaseObject::CompareExchangeRefField(RefField<>& field, const RefField<> oldRef, const RefField<> newRef)
{
    if (HealSlot(field, oldRef.GetFieldValue(), newRef.GetFieldValue(),
                 HealSite::BaseObjectCompareExchangeRefField, HealNull::Allow)) {
        DLOG(BARRIER, "update obj %p ref-field@%p: %#zx => %#zx", raw(oldRef.GetFieldValue()), raw(newRef.GetFieldValue()));
        return true;
    }
    return false;
}
} // namespace MapleRuntime

// Phase B: the read-barrier mask the compiler tests against (see TypeDef.h for why).
// "All colour bits set" keeps the predicate identical to the shift form it replaces.
// Phase C: bad = mid-evacuation, or carrying a colour other than the one being handed out.
// The initial conceptual state is RemappedYoung0 x RemappedOld0, encoded by Remapped00.
// ⭐⭐⭐ 0809：⭐ 让"⭐ 这枚 .so 是哪个 commit 建的"变成一条 `strings` 能答的问题。
//   ⛔ 一晚三次：⭐ 修法在源码里，⭐ 而跑的是旧二进制（⭐ 信号死锁修法 · ⭐ opt · ⭐ cjc/std/runtime 三份）
//   ⇒ ⭐ 而 `.so` 只有 GNU build-id ⇒ ⭐⭐ 能区分两枚，⛔ 说不出来自哪个 commit
//   ⇒ ⭐ 于是只能靠目录日期猜 —— ⭐⭐ 而 `sdk-stageA2` 目录是 08-07、⭐ 里面的 runtime 更早
//   ⭐ 判法：`strings <libcangjie-runtime.so> | grep CJRT-COMMIT:`
#ifndef CJ_RUNTIME_COMMIT
#define CJ_RUNTIME_COMMIT "unknown"
#endif
extern "C" __attribute__((used, visibility("default")))
const char g_cjRuntimeProvenance[] = "CJRT-COMMIT:" CJ_RUNTIME_COMMIT;

// c4unify: these three used to be hand-written literal expressions -- a second copy of
// WCollector::set_good_masks, whose own comment said it was written to "match live
// set_good_masks shape". They are now the same function evaluated at the initial epoch, and the
// old literals survive only as witnesses in the static_asserts below: if the shared formula ever
// drifts from what shipped, the build stops here rather than at the first flip.
//
// ⭐ The named constexpr constants are load-bearing, not style. These globals are
// constant-initialised today (they land in .data), and both the compiler-emitted barriers and
// BaseObject.cpp itself read them before main. Routing through a `constexpr unsigned long`
// makes a platform on which the expression is not a constant expression a compile error instead
// of a silent demotion to dynamic initialisation -- which would leave the masks reading 0 during
// static init, i.e. every reference load-good, i.e. no barrier at all.
namespace {
constexpr unsigned long kLoadBad0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.loadBad);
constexpr unsigned long kMarkBad0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.markBad);
constexpr unsigned long kStoreBad0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.storeBad);
constexpr unsigned long kStoreGood0 = static_cast<unsigned long>(MapleRuntime::kInitialBadMasks.storeGood);

// Witnesses: the literal expressions this file carried before c4unify, verbatim.
static_assert(kLoadBad0 == (MapleRuntime::TAGGED_BITS_MASK |
                            (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00)),
              "g_cjLoadBadMask initial value changed");
// Mark-good includes load-good plus the current young and old mark epochs. The initial current
// epochs are *_0, so their *_1 bits are bad (OpenJDK zAddress.cpp:78-87,120-127).
static_assert(kMarkBad0 == (MapleRuntime::TAGGED_BITS_MASK |
                            (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00) |
                            MapleRuntime::MARKED_YOUNG_1 | MapleRuntime::MARKED_OLD_1),
              "g_cjMarkBadMask initial value changed");
// Store-good = mark-good | current Remembered (initial REMEMBERED_0). Store-bad rejects the
// other rem bit and all mark-bad bits (OpenJDK zAddress.cpp:83-87).
static_assert(kStoreBad0 == (MapleRuntime::TAGGED_BITS_MASK |
                             (MapleRuntime::REMAP_COLOUR_MASK ^ MapleRuntime::ZPointerRemapped00) |
                             MapleRuntime::MARKED_YOUNG_1 | MapleRuntime::MARKED_OLD_1 |
                             MapleRuntime::REMEMBERED_1),
              "g_cjStoreBadMask initial value changed");
// Store-good = current remap | current MY | current MO | current Remembered
// (OpenJDK zAddress.cpp:83). Initial epoch: Remapped00 | MY_0 | MO_0 | REM_0.
static_assert(kStoreGood0 == (MapleRuntime::ZPointerRemapped00 | MapleRuntime::MARKED_YOUNG_0 |
                              MapleRuntime::MARKED_OLD_0 | MapleRuntime::REMEMBERED_0),
              "g_cjStoreGoodMask initial value changed");
// good/bad complementarity at the initial epoch (zAddress.cpp:87):
// StoreBad == StoreGood ^ StoreMetadataMask  (TAGGED_BITS_MASK is 0).
static_assert((kStoreGood0 ^ static_cast<unsigned long>(MapleRuntime::STORE_METADATA_MASK)) == kStoreBad0,
              "g_cjStoreGoodMask ^ STORE_METADATA_MASK != g_cjStoreBadMask at init");
} // namespace

extern "C" unsigned long g_cjLoadBadMask = kLoadBad0;

extern "C" MRT_EXPORT unsigned long g_cjMarkBadMask = kMarkBad0;

extern "C" MRT_EXPORT unsigned long g_cjStoreBadMask = kStoreBad0;

extern "C" MRT_EXPORT unsigned long g_cjStoreGoodMask = kStoreGood0;
