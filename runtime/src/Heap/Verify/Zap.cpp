// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/Zap.h"

#include <cstdlib>
#include <cstring>

#include "Base/LogFile.h"
#include "Base/Macros.h"

namespace MapleRuntime {
namespace {
bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}
} // namespace

bool HeapZap::ReclaimEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_ZAP_RECLAIM");
    return on;
}

bool HeapZap::AllocEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_ZAP_ALLOC");
    return on;
}

bool HeapZap::IsZapWord(uintptr_t value)
{
    if (value == static_cast<uintptr_t>(ZAP_WORD)) {
        return true;
    }
    // Also recognize HotSpot-classic patterns when present in residual memory.
    if (value == static_cast<uintptr_t>(0xBAADBABEUL) ||
        value == static_cast<uintptr_t>(0xBAADBABEBAADBABEULL) ||
        value == static_cast<uintptr_t>(0x2BAD4B0BBAADBABEULL) ||
        value == static_cast<uintptr_t>(0xDEADBEEFDEADBEEFULL)) {
        return true;
    }
    // 32-bit half match (unaligned residual).
    if (static_cast<uint32_t>(value) == ZAP_WORD32 &&
        static_cast<uint32_t>(value >> 32) == ZAP_WORD32) {
        return true;
    }
    return false;
}

void HeapZap::Fill(void* addr, size_t size)
{
    if (addr == nullptr || size == 0) {
        return;
    }
    auto* p = reinterpret_cast<uint8_t*>(addr);
    size_t i = 0;
    uint64_t word = ZAP_WORD;
    for (; i + sizeof(uint64_t) <= size; i += sizeof(uint64_t)) {
        std::memcpy(p + i, &word, sizeof(uint64_t));
    }
    if (i < size) {
        std::memcpy(p + i, &word, size - i);
    }
}

void HeapZap::ZapReclaimedRegion(MAddress start, MAddress end)
{
    if (!ReclaimEnabled() || end <= start) {
        return;
    }
    size_t size = static_cast<size_t>(end - start);
    Fill(reinterpret_cast<void*>(start), size);
    DLOG(REGION, "[GCV2][zap][reclaim] start=%p size=%zu pattern=0x%llx env=MRT_GCV2_ZAP_RECLAIM=1",
         reinterpret_cast<void*>(start), size, static_cast<unsigned long long>(ZAP_WORD));
}

void HeapZap::ZapAllocated(MAddress addr, size_t size)
{
    if (!AllocEnabled() || addr == 0 || size == 0) {
        return;
    }
    Fill(reinterpret_cast<void*>(addr), size);
    DLOG(ALLOC, "[GCV2][zap][alloc] addr=%p size=%zu pattern=0x%llx env=MRT_GCV2_ZAP_ALLOC=1",
         reinterpret_cast<void*>(addr), size, static_cast<unsigned long long>(ZAP_WORD));
}

} // namespace MapleRuntime
