// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Heap/z/zPageAllocator.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGeneration.hpp"
#include "Heap/z/zVerify.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <unistd.h>
#include <vector>
#if defined(_WIN64)
#include <processthreadsapi.h>
#endif

#include "Heap/Allocator/RegionSpace.h"
#include "Base/CString.h"
#include "Base/LogFile.h"
#include "Base/TimeUtils.h"
#include "Heap/z/zCollectedHeap.hpp"
#include "Heap/z/zForwarding.hpp"
#include "Heap/z/zDriver.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDirector.hpp"
#include "Heap/z/zUncommitter.hpp"
#include "Heap/z/zStat.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Common/BaseObject.h"
#include "Common/ScopedObjectAccess.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zRememberedSet.hpp"
#include "Heap/shared/collectedHeap.hpp"
#include "Heap/z/zForwardingTable.hpp"
#include "Heap/z/zRelocationSetSelector.hpp"
#include "Mutator/Mutator.inline.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"
#if defined(CANGJIE_TSAN_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif
#include "Sync/Sync.h"

namespace MapleRuntime {
// Keep the empty allocation page identity in the product DSO. An inline
// function-local object gives callers in another DSO a different sentinel.
ZPage* ZPage::NullRegion()
{
    static ZPage nullRegion;
    return &nullRegion;
}

uintptr_t ZPage::heapStartAddress = 0;
std::vector<ZPage::ReservedSegment> ZPage::reservedSegments;
ZSafeDelete<ZPage> ZPage::safeDestroy;

ZPage::~ZPage()
{
    if (_retireHook) {
        _retireHook();
    }
}

std::atomic<size_t> ZPage::youngRegionCount { 0 };
namespace {
// ZPageAllocator::used_generation (zPageAllocator.cpp:1311). TLAB extents
// vary, so region counts cannot stand in for young-generation byte occupancy.
std::atomic<size_t> youngRegionBytes{ 0 };
}
std::atomic<size_t> ZPage::tdWindowCount { 0 };

std::mutex ZPage::youngRegionFlagMutex;

size_t ZPage::GetYoungRegionCount()
{
    return youngRegionCount.load(std::memory_order_acquire);
}

size_t RegionManager::GetYoungAllocatedSize() const
{
    return used_generation(ZGenerationId::young);
}

bool ZPage::HasYoungRegions()
{
    return GetYoungRegionCount() != 0;
}

static size_t GetPageSize() noexcept
{
    size_t pageSize = 0;
#if defined(_WIN64)
    SYSTEM_INFO systeminfo;
    GetSystemInfo(&systeminfo);
    if (systeminfo.dwPageSize != 0) {
        pageSize = systeminfo.dwPageSize;
    } else {
        // default page size is 4KB if get system page size failed.
        pageSize = 4 * KB;
    }
#elif defined(__APPLE__)
    pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#else
    pageSize = static_cast<size_t>(getpagesize());
#endif
    return pageSize;
}

// System default page size
const size_t MRT_PAGE_SIZE = GetPageSize();
const size_t AllocatorUtils::ALLOC_PAGE_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// ZGC zPage.cpp:153-163: the page owns both remembered-set checks.
void ZPage::verify_remset_cleared_current() const
{
    if (ZVerifyRemembered) {
        CHECK_DETAIL(is_remset_cleared_current(), "current remset bits should be cleared");
    }
}

void ZPage::verify_remset_cleared_previous() const
{
    if (ZVerifyRemembered) {
        CHECK_DETAIL(is_remset_cleared_previous(), "previous remset bits should be cleared");
    }
}

// ZGC zPage.cpp:196-203: verify the source page's own livemap.
void ZPage::verify_live(uint32_t liveObjects, size_t liveBytes, bool inPlace) const
{
    const ZLiveMap* map = &livemap();
    if (!inPlace) {
        // In-place relocation has changed the page to allocating
        const ZGenerationId id = generation_id();
        MRT_ASSERT(map->is_marked(id), "Should be marked");
        (void)id;
        ZGeneration* generation = ZGeneration::generation(id);
        MRT_ASSERT(generation == nullptr || !generation->is_phase_mark(), "Wrong phase");
        (void)generation;
    }
    CHECK_DETAIL(liveObjects == map->live_objects(), "Invalid number of live objects");
    CHECK_DETAIL(liveBytes == map->live_bytes(), "Invalid number of live bytes");
}

} // namespace MapleRuntime



// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/ImmortalWrapper.h"
#include "Heap/z/zPage.hpp"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zLiveMap.inline.hpp"
#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zGlobals.hpp"

namespace MapleRuntime {
// ZPage::reset_seqnum (zPage.cpp:90-93), after owner selection and before publication.
void ZPage::ResetPageSequence()
{
    _seqnum = generation()->seqnum();
    _seqnum_other = ZGeneration::generation(
        _generation_id == ZGenerationId::young ? ZGenerationId::old : ZGenerationId::young)->seqnum();
}

// ZGC zPage.cpp:82-88: resolve the page's owning generation.
ZGeneration* ZPage::generation()
{
    return ZGeneration::generation(_generation_id);
}

const ZGeneration* ZPage::generation() const
{
    return ZGeneration::generation(_generation_id);
}
} // namespace MapleRuntime

namespace MapleRuntime {
ZPage::ZPage()
    : _type(ZPageType::small),
      _generation_id(ZGenerationId::old),
      _age(PageAge::old),
      _seqnum(0),
      _seqnum_other(0),
      _partition_id(0),
      _virtual(),
      _top(zoffset_end::invalid),
       _livemap(object_max_count_for(ZPageType::small, 0)),
       _remembered_set(),
       _relocate_promoted(false)
    {
    }

ZPage::ZPage(ZPageType type, PageAge age, const ZVirtualMemory& vmem)
    : _type(type),
      _generation_id(age != PageAge::old ? ZGenerationId::young : ZGenerationId::old),
      _age(age),
      _seqnum(0),
      _seqnum_other(0),
      _partition_id(0),
      _virtual(vmem),
      _top(to_zoffset_end(vmem.start())),
       _livemap(object_max_count_for(type, vmem.size())),
       _remembered_set(),
       _relocate_promoted(false)
{
    MRT_ASSERT((type == ZPageType::small && size() == ZPageSizeSmall) ||
               (type == ZPageType::medium && ZPageSizeMediumMin <= size() && size() <= ZPageSizeMediumMax) ||
               (type == ZPageType::large && size() % ZGranuleSize == 0), "Page type/size mismatch");
    reset(age);
    if (age == PageAge::old) {
        remset_alloc();
    }
}

void ZPage::remset_alloc()
{
    CHECK(!_remembered_set.is_initialized());
    CHECK(!IsYoungRegion());
    _remembered_set.initialize(size());
}

const char* ZPage::type_to_string() const
{
    switch (type()) {
        case ZPageType::small:
            return "Small";
        case ZPageType::medium:
            return "Medium";
        case ZPageType::large:
            return "Large";
        default:
            return "Unknown";
    }
}

ZPage* ZPage::reset(PageAge age)
{
    std::lock_guard<std::mutex> lock(youngRegionFlagMutex);
    const bool wasYoung = _generation_id == ZGenerationId::young;
    _age = age;
    _generation_id = age == PageAge::old ? ZGenerationId::old : ZGenerationId::young;
    const bool makeYoung = _generation_id == ZGenerationId::young;
    if (!wasYoung && makeYoung) {
        youngRegionBytes.fetch_add(GetRegionSize(), std::memory_order_release);
        youngRegionCount.fetch_add(1, std::memory_order_release);
    }
    if (wasYoung && !makeYoung) {
        size_t count = youngRegionCount.load(std::memory_order_relaxed);
        if (count > 0) {
            youngRegionCount.fetch_sub(1, std::memory_order_release);
            youngRegionBytes.fetch_sub(GetRegionSize(), std::memory_order_release);
        }
    }
    ResetPageSequence();
    return this;
}

ZPage* ZPage::clone_for_promotion() const
{
    CHECK(IsYoungRegion());
    ZPage* page = new ZPage(_type, PageAge::old, _virtual);
    page->_top = _top;
    return page;
}

// zPage.inline.hpp:453-522: atomic allocation and allocation undo.
uintptr_t ZPage::alloc_object_atomic(size_t size)
{
    MRT_ASSERT(is_allocating(), "Invalid state");
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    zoffset_end addr = top();
    for (;;) {
        zoffset_end newTop;
        if (!to_zoffset_end(&newTop, addr, aligned) || newTop > end()) { return 0; }
        if (__atomic_compare_exchange(&_top, &addr, &newTop, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return untype(ZOffset::address_unsafe(to_zoffset(addr)));
        }
    }
}

bool ZPage::undo_alloc_object(uintptr_t addr, size_t size)
{
    MRT_ASSERT(is_allocating(), "Invalid state");
    const zoffset offset = ZAddress::offset(to_zaddress_unsafe(addr));
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    const zoffset_end newTop = top() - aligned;
    if (newTop != offset) { return false; }
    _top = newTop;
    return true;
}

bool ZPage::undo_alloc_object_atomic(uintptr_t addr, size_t size)
{
    MRT_ASSERT(is_allocating(), "Invalid state");
    const zoffset offset = ZAddress::offset(to_zaddress_unsafe(addr));
    const size_t aligned = AlignUp<size_t>(size, object_alignment());
    zoffset_end oldTop = top();
    for (;;) {
        zoffset_end newTop = oldTop - aligned;
        if (newTop != offset) { return false; }
        if (__atomic_compare_exchange(&_top, &oldTop, &newTop, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
}

ZForwarding* ZPage::GetFromPageCarrier() const
    {
        const MAddress start = GetRegionStart();
        if (start == 0) {
            return nullptr;
        }
        ZForwarding* carrier = Heap::GetHeap().GetZGeneration(GetOwnerGeneration()).forwarding_table().get(start);
        return carrier != nullptr && carrier->page() == this ? carrier : nullptr;
    }


}
