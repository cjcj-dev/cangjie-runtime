// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/HdrWhoDiag.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "Common/Runtime.h"
#include "Common/StateWord.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Heap.h"
#include "Heap/Verify/DiagGate.h"
#include "TypeInfoManager.h"
#include "securec.h"

namespace MapleRuntime {
namespace HdrWhoDiag {
namespace {

bool EnvIsOne(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool GateOn()
{
    static const bool on = []() {
        DiagGate::MaybeAnnounce();
        return EnvIsOne("MRT_GCV2_HDRWHO") || DiagGate::TokenOn("hdrwho");
    }();
    return on;
}

void WriteLine(const char* buf, size_t len)
{
    if (buf != nullptr && len > 0) {
        (void)write(STDERR_FILENO, buf, len);
    }
}

void DecodeAscii(uintptr_t word, char out[9])
{
    for (int i = 0; i < 8; ++i) {
        unsigned char c = static_cast<unsigned char>((word >> (8 * i)) & 0xffU);
        out[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    }
    out[8] = '\0';
}

bool WordReadable(uintptr_t addr)
{
    if (addr < 0x1000UL) {
        return false;
    }
    if ((addr & 7UL) != 0) {
        return false;
    }
    if (Runtime::CurrentRef() != nullptr && Heap::IsHeapAddress(addr)) {
        return true;
    }
    return false;
}

uintptr_t PeekWord(uintptr_t addr, unsigned& ok)
{
    ok = 0;
    if (!WordReadable(addr)) {
        return 0;
    }
    ok = 1;
    return *reinterpret_cast<const uintptr_t*>(addr);
}

bool TipLooksPlausible(uintptr_t tipAddr)
{
    constexpr uintptr_t kMin = 0x100000000ULL;
    if (tipAddr == 0 || tipAddr < kMin) {
        return false;
    }
    if ((tipAddr & StateWord::ADDRESS_ALIGN_MASK) != 0) {
        return false;
    }
    if ((tipAddr & 0xffffffffULL) == 0) {
        return false;
    }
    if (Runtime::CurrentRef() != nullptr && Heap::IsHeapAddress(tipAddr)) {
        return false;
    }
    return true;
}

} // namespace

bool Enabled()
{
    return GateOn();
}

void NoteCrashRdi(uintptr_t rdi)
{
    if (!GateOn()) {
        return;
    }
    unsigned heap = 0;
    unsigned rtype = 255;
    unsigned young = 0;
    unsigned garbage = 0;
    unsigned freeReg = 0;
    uintptr_t rstart = 0;
    uintptr_t rend = 0;
    uintptr_t alloc = 0;
    uintptr_t off = 0;
    unsigned atStart = 0;
    unsigned inAlloc = 0;
    unsigned align8 = (rdi & 7UL) == 0 ? 1 : 0;
    if (Runtime::CurrentRef() != nullptr && rdi != 0 && Heap::IsHeapAddress(rdi)) {
        heap = 1;
        RegionInfo* region = RegionInfo::TryGetRegionInfoAt(rdi);
        if (region != nullptr) {
            rtype = static_cast<unsigned>(region->GetRegionType());
            young = region->IsYoungRegion() ? 1 : 0;
            garbage = region->IsGarbageRegion() ? 1 : 0;
            freeReg = region->IsFreeRegion() ? 1 : 0;
            rstart = region->GetRegionStart();
            rend = region->GetRegionEnd();
            alloc = region->GetRegionAllocPtr();
            off = rdi >= rstart ? rdi - rstart : 0;
            atStart = (rdi == rstart) ? 1 : 0;
            inAlloc = (rdi >= rstart && rdi < alloc) ? 1 : 0;
        }
    }

    unsigned tipOk = 0;
    uintptr_t tipWord = PeekWord(rdi, tipOk);
    char tipAscii[9];
    DecodeAscii(tipWord, tipAscii);
    unsigned tipPlausible = (tipOk != 0 && TipLooksPlausible(tipWord)) ? 1 : 0;
    unsigned tipInHeap = 0;
    unsigned tipInTim = 0;
    if (tipOk != 0 && Runtime::CurrentRef() != nullptr) {
        tipInHeap = Heap::IsHeapAddress(tipWord) ? 1 : 0;
        tipInTim = TypeInfoManager::GetTypeInfoManager().ContainsAddress(tipWord) ? 1 : 0;
    }

    unsigned hostOff = 0;
    uintptr_t hostTip = 0;
    unsigned hostPlausible = 0;
    if (heap != 0 && rstart != 0) {
        for (unsigned k : { 8u, 16u, 24u, 32u, 40u, 48u, 56u, 64u }) {
            if (rdi < k) {
                continue;
            }
            uintptr_t cand = rdi - k;
            if (cand < rstart || (alloc != 0 && cand >= alloc)) {
                continue;
            }
            unsigned ok = 0;
            uintptr_t candTip = PeekWord(cand, ok);
            if (ok != 0 && TipLooksPlausible(candTip)) {
                hostOff = k;
                hostTip = candTip;
                hostPlausible = 1;
                break;
            }
        }
    }

    unsigned startOk = 0;
    uintptr_t startWord = (rstart != 0) ? PeekWord(rstart, startOk) : 0;
    char startAscii[9];
    DecodeAscii(startWord, startAscii);

    char line[1024];
    int n = sprintf_s(line, sizeof(line),
                      "[GCV2][hdrwho] crash rdi=%#zx heap=%u align8=%u regionType=%u young=%u "
                      "garbage=%u free=%u start=%#zx end=%#zx alloc=%#zx off=%#zx atStart=%u "
                      "inAlloc=%u tipOk=%u tip=%#zx tipAscii=%s tipPlausible=%u tipInHeap=%u "
                      "tipInTim=%u hostOff=%u hostTip=%#zx hostPlausible=%u startOk=%u "
                      "startWord=%#zx startAscii=%s env=MRT_GCV2_HDRWHO=1\n",
                      rdi, heap, align8, rtype, young, garbage, freeReg, rstart, rend, alloc, off,
                      atStart, inAlloc, tipOk, tipWord, tipAscii, tipPlausible, tipInHeap, tipInTim,
                      hostOff, hostTip, hostPlausible, startOk, startWord, startAscii);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }

    if (heap == 0 || rstart == 0) {
        return;
    }
    uintptr_t dumpBase = rdi > 32 ? rdi - 32 : rdi;
    if (dumpBase < rstart) {
        dumpBase = rstart;
    }
    char words[512];
    size_t pos = 0;
    words[0] = '\0';
    for (unsigned i = 0; i < 8; ++i) {
        uintptr_t addr = dumpBase + static_cast<uintptr_t>(i) * 8U;
        unsigned ok = 0;
        uintptr_t w = PeekWord(addr, ok);
        char ascii[9];
        DecodeAscii(w, ascii);
        int m = sprintf_s(words + pos, sizeof(words) - pos, " %#zx:%s:%#zx", addr, ok ? ascii : "????????",
                          ok ? w : 0);
        if (m <= 0) {
            break;
        }
        pos += static_cast<size_t>(m);
        if (pos + 32 >= sizeof(words)) {
            break;
        }
    }
    n = sprintf_s(line, sizeof(line), "[GCV2][hdrwho] words%s env=MRT_GCV2_HDRWHO=1\n", words);
    if (n > 0) {
        WriteLine(line, static_cast<size_t>(n));
    }
}

} // namespace HdrWhoDiag
} // namespace MapleRuntime
