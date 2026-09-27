// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "HwasanInterface.h"

#include <map>

#include "Base/Globals.h"
#include "Base/Log.h"
#include "Base/LogFile.h"
#include "Base/Macros.h"
#include "Base/RwLock.h"
#include "ObjectModel/MArray.inline.h"
#include "Sanitizer/ArrayReferenceCounter.h"
#include "Sanitizer/SanitizerCompilerCalls.h"
#include "Sanitizer/SanitizerMacros.h"
#include "Sanitizer/SanitizerSymbols.h"
#include "schedule.h"

namespace MapleRuntime {
namespace Sanitizer {
using namespace std;

// array size, tag
static ArrayReferenceCounter<pair<uint64_t, uint8_t>>* g_counter = nullptr;

static constexpr unsigned HWASAN_ADDR_TAG_SHIFT = 56;
static constexpr unsigned HWASAN_TAG_BITS = 8;
static constexpr uintptr_t HWASAN_TAG_MASK = (1UL << HWASAN_TAG_BITS) - 1;
static constexpr uintptr_t HWASAN_TAG_ADDR_MASK = HWASAN_TAG_MASK << HWASAN_ADDR_TAG_SHIFT;

// shadow constants
static constexpr uint64_t HWASAN_SHADOW_SCALE = 4;
static constexpr uint64_t HWASAN_SHADOW_ALIGN = 1ULL << HWASAN_SHADOW_SCALE;

uintptr_t UntagAddr(uintptr_t taggedAddr)
{
    return taggedAddr & ~HWASAN_TAG_ADDR_MASK;
}

static inline uintptr_t GetTagFromPointer(const void* p)
{
    return (reinterpret_cast<uintptr_t>(p) >> HWASAN_ADDR_TAG_SHIFT) & HWASAN_TAG_MASK;
}

static inline void* UntagPtr(const void* taggedPtr)
{
    return reinterpret_cast<void*>(UntagAddr(reinterpret_cast<uintptr_t>(taggedPtr)));
}

// this free hook is for libc's free, so if ptr is located in cangjie heap,
// it must be an invalid free
static void MallocCheckCallback(const volatile void*, size_t) {}
static void FreeCheckCallback(const volatile void* ptr)
{
    // check cangjie heap
    uintptr_t addr = UntagAddr(reinterpret_cast<uintptr_t>(ptr));
    if (g_counter != nullptr && g_counter->AddressInRange(addr)) {
        // check failed, abort
        Logger::GetLogger().FormatLog(RTLOG_FATAL, true, "invalid free in cangjie heap with ptr %p", ptr);
    }
}

static void TagMemory(volatile void* addr, uint8_t tag, uintptr_t size)
{
    uintptr_t fullBlockSize = AlignDown(size, HWASAN_SHADOW_ALIGN);
    if (fullBlockSize != 0) {
        REAL(__hwasan_tag_memory)(addr, tag, fullBlockSize);
    }

    // have remaining size
    if (fullBlockSize != size) {
        uint8_t volatile* shortGranuleAddr = reinterpret_cast<uint8_t volatile*>(addr) + fullBlockSize;
        uint8_t shortGranule = size % HWASAN_SHADOW_ALIGN;
        REAL(__hwasan_tag_memory)(shortGranuleAddr, shortGranule, HWASAN_SHADOW_ALIGN);
        // write full block tag to the end of remain block
        shortGranuleAddr[HWASAN_SHADOW_ALIGN - 1] = tag;
    }
}

void OnHeapAllocated(void* addr, size_t size)
{
    // 1. allocate reference counter for array
    g_counter = new ArrayReferenceCounter<pair<uint64_t, uint8_t>>(UntagPtr(addr), size);
    CHECK_DETAIL(g_counter != nullptr, "Array Reference Counter allocation failed.");

    // 2. register hook in hwasan
    // limited by macro declaration, we have to cast to general pointer
    int result = REAL(__sanitizer_install_malloc_and_free_hooks)(reinterpret_cast<const void*>(MallocCheckCallback),
                                                                 reinterpret_cast<const void*>(FreeCheckCallback));
    if (result == 0) {
        LOG(RTLOG_WARNING, "free hook register failed, false positive may occurs");
    }
}

void OnHeapDeallocated(void*, size_t)
{
    // 1. check and deallocate counter
    if (!g_counter->Empty()) {
        auto logger = [](uintptr_t addr, pair<int64_t, pair<uint64_t, uint8_t>> data) {
            Logger::GetLogger().FormatLog(RTLOG_FAIL, true,
                                          "Unreleased array 0x%lx: length[0x%lx], refcount[%d], tag[%u]",
                                          UntagAddr(addr), data.second.first, data.first, data.second.second);
        };
        g_counter->DiagnoseUnreleased(logger);
        delete g_counter;
        g_counter = nullptr;

        Logger::GetLogger().FormatLog(RTLOG_FATAL, true, "Detect un-released array");
        BUILTIN_UNREACHABLE();
    }
    delete g_counter;
    g_counter = nullptr;
}

void UntagMemory(void* addr, size_t size)
{
    addr = UntagPtr(addr);
    // align up to 16, clear out all shadow tags and short granule
    TagMemory(addr, 0, AlignUp(size, HWASAN_SHADOW_ALIGN));
}

void HandleNoReturn(uint64_t from, uint64_t to)
{
    DLOG(SANITIZER, "Handle no return stack range (0x%lx, 0x%lx)", from, to);
    if (from >= to) {
        return;
    }

    // align down to 16
    from = AlignDown(from, HWASAN_SHADOW_ALIGN);
    size_t unpoisonSize = to - from;
    // align up to 16, clear out all shadow tags and short granule
    TagMemory(reinterpret_cast<void*>(from), 0, AlignUp(unpoisonSize, HWASAN_SHADOW_ALIGN));
}

void AsanRead(volatile const void* addr, uintptr_t size)
{
    REAL(__hwasan_loadN)(addr, size);
}

void AsanWrite(volatile const void* addr, uintptr_t size)
{
    REAL(__hwasan_storeN)(addr, size);
}

extern "C" {
MRT_EXPORT void CJ_MCC_AsanRead(volatile const void* addr, uintptr_t size)
{
    REAL(__hwasan_loadN)(addr, size);
}

MRT_EXPORT void CJ_MCC_AsanWrite(volatile const void* addr, uintptr_t size)
{
    REAL(__hwasan_storeN)(addr, size);
}

MRT_EXPORT void CJ_MCC_AsanHandleNoReturn(const void* rsp)
{
    void* stackAddr = CJThreadStackAddrGet();
    if (stackAddr == nullptr) {
        // not a cj thread
        REAL(__hwasan_handle_vfork)(rsp);
    } else {
        Sanitizer::HandleNoReturn(reinterpret_cast<uint64_t>(stackAddr), reinterpret_cast<uint64_t>(rsp));
    }
}
}
} // namespace Sanitizer
} // namespace MapleRuntime
