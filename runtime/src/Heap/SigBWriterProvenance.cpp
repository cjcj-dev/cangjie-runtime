// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SigBWriterProvenance.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "Base/Log.h"
#include "Base/SysCall.h"
#include "Heap/Collector/Collector.h"
#include "Heap/Collector/GcStats.h"
#include "Heap/Heap.h"
#include "ObjectModel/MClass.h"

namespace MapleRuntime {
namespace {
thread_local SigBWriterProvenance::ThreadRing* g_sigbThreadRing = nullptr;

const char* WriterName(uint8_t kind)
{
    switch (kind) {
        case SigBWriterProvenance::WRITER_BULK_FIX:
            return "BULK_FIX";
        case SigBWriterProvenance::WRITER_BARRIER:
            return "BARRIER";
        case SigBWriterProvenance::WRITER_COPY_OBJECT:
            return "COPY_OBJECT";
        default:
            return "NONE";
    }
}
} // namespace

SigBWriterProvenance& SigBWriterProvenance::Instance() noexcept
{
    static SigBWriterProvenance instance;
    return instance;
}

bool SigBWriterProvenance::IsConstAnalysisVictimType(TypeInfo* typeInfo) noexcept
{
    if (typeInfo == nullptr) {
        return false;
    }
    const char* name = typeInfo->GetName();
    if (name == nullptr) {
        return false;
    }
    // Victim surface for signature B (gcsigb): HashMap backing arrays of
    // ConstAnalysisWrapper.resultsMap / resultsPoolMap.
    // Runtime TypeInfo expands ConstDomain → State<ValueDomain<ConstValue>,
    // FullStatePool|ActiveStatePool>. Also match:
    //   - RawArray<HashMapEntry<Function, Results<...>>> (entries)
    //   - RawArray<Int64> is too broad for primary filter; covered only when
    //     an entries array is registered (same HashMap instance not needed —
    //     we track exact slots that receive barrier/bulk writes).
    if (std::strstr(name, "RawArray") == nullptr) {
        return false;
    }
    // Primary: entries array of resultsMap / resultsPoolMap.
    if (std::strstr(name, "HashMapEntry") != nullptr) {
        // Expanded Results value type for const analysis maps.
        if (std::strstr(name, "Results") != nullptr && std::strstr(name, "ConstValue") != nullptr) {
            return true;
        }
        if (std::strstr(name, "Function") != nullptr &&
            (std::strstr(name, "FullStatePool") != nullptr || std::strstr(name, "ActiveStatePool") != nullptr ||
             std::strstr(name, "ConstDomain") != nullptr || std::strstr(name, "ConstPoolDomain") != nullptr)) {
            return true;
        }
        // Literal alias names (if ever present unexpanded).
        if (std::strstr(name, "ConstDomain") != nullptr || std::strstr(name, "ConstPoolDomain") != nullptr) {
            return true;
        }
    }
    return false;
}

void SigBWriterProvenance::MaybeRegister(BaseObject* obj, TypeInfo* typeInfo, size_t size) noexcept
{
    if (obj == nullptr || size == 0 || typeInfo == nullptr) {
        return;
    }
    // Observe-only probe: any HashMapEntry RawArray allocation + sample names.
    const char* name = typeInfo->GetName();
    if (name != nullptr && std::strstr(name, "RawArray") != nullptr &&
        std::strstr(name, "HashMapEntry") != nullptr) {
        hashMapEntryAllocCount.fetch_add(1, std::memory_order_relaxed);
        // Sample unique type names that mention Function / Results / ConstValue.
        if (std::strstr(name, "Function") != nullptr || std::strstr(name, "Results") != nullptr ||
            std::strstr(name, "ConstValue") != nullptr) {
            size_t sc = typeNameSampleCount.load(std::memory_order_relaxed);
            if (sc < SAMPLE_CAP) {
                bool seen = false;
                for (size_t i = 0; i < sc; ++i) {
                    if (typeNameSamples[i] == name) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    typeNameSamples[sc] = name;
                    typeNameSampleCount.compare_exchange_strong(sc, sc + 1, std::memory_order_relaxed);
                }
            }
        }
    }
    if (!IsConstAnalysisVictimType(typeInfo)) {
        return;
    }
    EnsureExitDumpRegistered();
    const uintptr_t start = reinterpret_cast<uintptr_t>(obj);
    const uintptr_t end = start + size;
    std::lock_guard<std::mutex> lg(registryMutex);
    const size_t n = registrySize.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        if (registry[i].start == start) {
            registry[i].end = end;
            registry[i].typeInfo = typeInfo;
            return;
        }
    }
    if (n >= REGISTRY_CAP) {
        overflowCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    registry[n] = RegistryEntry{ start, end, typeInfo };
    registrySize.store(n + 1, std::memory_order_relaxed);
}

bool SigBWriterProvenance::ContainsSlot(uintptr_t slot) const noexcept
{
    if (slot == 0) {
        return false;
    }
    const size_t n = registrySize.load(std::memory_order_relaxed);
    // Lock-free snapshot read: STW writers (copy relocate) and mutator
    // registration only grow/update rows; a torn end is acceptable for
    // observe-only (may miss a hit, never rewrite).
    for (size_t i = 0; i < n && i < REGISTRY_CAP; ++i) {
        const uintptr_t start = registry[i].start;
        const uintptr_t end = registry[i].end;
        if (slot >= start && slot < end) {
            return true;
        }
    }
    return false;
}

SigBWriterProvenance::ThreadRing* SigBWriterProvenance::GetThreadRing() noexcept
{
    ThreadRing* ring = g_sigbThreadRing;
    if (ring != nullptr) {
        return ring;
    }
    ring = new (std::nothrow) ThreadRing();
    if (ring == nullptr) {
        return nullptr;
    }
    ring->tid = static_cast<uint64_t>(MapleRuntime::GetTid());
    {
        std::lock_guard<std::mutex> lg(ringListMutex);
        ring->next = ringListHead;
        ringListHead = ring;
    }
    g_sigbThreadRing = ring;
    return ring;
}

void SigBWriterProvenance::RecordRing(WriterKind kind, uintptr_t slot, BaseObject* holder, uintptr_t oldValue,
                                      uintptr_t newValue) noexcept
{
    ThreadRing* ring = GetThreadRing();
    if (ring == nullptr) {
        return;
    }
    const uint8_t phase = static_cast<uint8_t>(Heap::GetHeap().GetGCPhase());
    const uint32_t gcOrdinal = static_cast<uint32_t>(g_gcCount + 1);
    const uint32_t idx = ring->cursor.fetch_add(1, std::memory_order_relaxed) & RING_MASK;
    RingEntry& e = ring->entries[idx];
    e.writer = static_cast<uint8_t>(kind);
    e.phase = phase;
    e.pad = 0;
    e.gcOrdinal = gcOrdinal;
    e.slot = slot;
    e.oldValue = oldValue;
    e.newValue = newValue;
    e.holder = reinterpret_cast<uintptr_t>(holder);
    ring->total.fetch_add(1, std::memory_order_relaxed);
    writeHitCount.fetch_add(1, std::memory_order_relaxed);
}

void SigBWriterProvenance::MaybeLogWrite(WriterKind kind, void* slot, BaseObject* holder, uintptr_t oldValue,
                                         uintptr_t newValue) noexcept
{
    if (slot == nullptr || registrySize.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
    if (!ContainsSlot(addr)) {
        return;
    }
    RecordRing(kind, addr, holder, oldValue, newValue);
}

void SigBWriterProvenance::OnCopyObject(BaseObject* from, BaseObject* to, size_t size, TypeInfo* typeInfo) noexcept
{
    if (from == nullptr || to == nullptr || size == 0) {
        return;
    }
    const uintptr_t fromStart = reinterpret_cast<uintptr_t>(from);
    const uintptr_t fromEnd = fromStart + size;
    const uintptr_t toStart = reinterpret_cast<uintptr_t>(to);
    const uintptr_t toEnd = toStart + size;

    {
        std::lock_guard<std::mutex> lg(registryMutex);
        const size_t n = registrySize.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            if (registry[i].start == fromStart && registry[i].end == fromEnd) {
                registry[i].start = toStart;
                registry[i].end = toEnd;
                if (typeInfo != nullptr) {
                    registry[i].typeInfo = typeInfo;
                }
                relocateCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Destination range overlaps a registered victim ⇒ GC content move into
    // victim memory (H1 candidate). Log once per overlapping registry row.
    const size_t n = registrySize.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n && i < REGISTRY_CAP; ++i) {
        const uintptr_t start = registry[i].start;
        const uintptr_t end = registry[i].end;
        if (toEnd <= start || toStart >= end) {
            continue;
        }
        copyHitCount.fetch_add(1, std::memory_order_relaxed);
        RecordRing(WRITER_COPY_OBJECT, start > toStart ? start : toStart, to, fromStart, toStart);
    }

    // Fresh victim body produced by copy of a matching type (identity or move).
    if (IsConstAnalysisVictimType(typeInfo)) {
        MaybeRegister(to, typeInfo, size);
    }
}

void SigBWriterProvenance::WriteText(int fd, const char* text) noexcept
{
    if (fd < 0 || text == nullptr) {
        return;
    }
    const size_t len = std::strlen(text);
    (void)write(fd, text, len);
}

void SigBWriterProvenance::WriteHex(int fd, uintptr_t value) noexcept
{
    char buf[2 + sizeof(uintptr_t) * 2 + 1];
    buf[0] = '0';
    buf[1] = 'x';
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(uintptr_t) * 2; ++i) {
        const size_t shift = (sizeof(uintptr_t) * 2 - 1 - i) * 4;
        buf[2 + i] = kHex[(value >> shift) & 0xfu];
    }
    buf[2 + sizeof(uintptr_t) * 2] = '\0';
    WriteText(fd, buf);
}

void SigBWriterProvenance::WriteDec(int fd, uint64_t value) noexcept
{
    char buf[32];
    size_t i = sizeof(buf);
    buf[--i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0 && i > 0) {
            buf[--i] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
    }
    WriteText(fd, buf + i);
}

void SigBWriterProvenance::DumpRegistry(int fd) const noexcept
{
    WriteText(fd, "[SIGB_WRITER] registry size=");
    WriteDec(fd, registrySize.load(std::memory_order_relaxed));
    WriteText(fd, " overflow=");
    WriteDec(fd, overflowCount.load(std::memory_order_relaxed));
    WriteText(fd, " write_hits=");
    WriteDec(fd, writeHitCount.load(std::memory_order_relaxed));
    WriteText(fd, " copy_hits=");
    WriteDec(fd, copyHitCount.load(std::memory_order_relaxed));
    WriteText(fd, " relocates=");
    WriteDec(fd, relocateCount.load(std::memory_order_relaxed));
    WriteText(fd, " hashmapentry_allocs=");
    WriteDec(fd, hashMapEntryAllocCount.load(std::memory_order_relaxed));
    WriteText(fd, "\n");
    const size_t sc = typeNameSampleCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < sc && i < SAMPLE_CAP; ++i) {
        WriteText(fd, "[SIGB_TYPE_SAMPLE] ");
        WriteText(fd, typeNameSamples[i] == nullptr ? "<null>" : typeNameSamples[i]);
        WriteText(fd, "\n");
    }

    const size_t n = registrySize.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n && i < REGISTRY_CAP; ++i) {
        WriteText(fd, "[SIGB_REG] i=");
        WriteDec(fd, i);
        WriteText(fd, " start=");
        WriteHex(fd, registry[i].start);
        WriteText(fd, " end=");
        WriteHex(fd, registry[i].end);
        WriteText(fd, " type=");
        const char* name = registry[i].typeInfo == nullptr ? "<null>" : registry[i].typeInfo->GetName();
        WriteText(fd, name == nullptr ? "<null-name>" : name);
        WriteText(fd, "\n");
    }
}

void SigBWriterProvenance::DumpRing(int fd, const ThreadRing* ring) const noexcept
{
    if (ring == nullptr) {
        return;
    }
    const uint32_t total = ring->total.load(std::memory_order_relaxed);
    const uint32_t cursor = ring->cursor.load(std::memory_order_relaxed);
    WriteText(fd, "[SIGB_RING] tid=");
    WriteDec(fd, ring->tid);
    WriteText(fd, " total=");
    WriteDec(fd, total);
    WriteText(fd, " cursor=");
    WriteDec(fd, cursor);
    WriteText(fd, "\n");
    if (total == 0) {
        return;
    }
    const uint32_t count = total < RING_CAP ? total : RING_CAP;
    const uint32_t start = total < RING_CAP ? 0 : (cursor & RING_MASK);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t idx = (start + i) & RING_MASK;
        const RingEntry& e = ring->entries[idx];
        if (e.writer == WRITER_NONE && e.slot == 0) {
            continue;
        }
        WriteText(fd, "[SIGB_WRITE] writer=");
        WriteText(fd, WriterName(e.writer));
        WriteText(fd, " phase=");
        WriteDec(fd, e.phase);
        WriteText(fd, " gc_ordinal=");
        WriteDec(fd, e.gcOrdinal);
        WriteText(fd, " slot=");
        WriteHex(fd, e.slot);
        WriteText(fd, " old=");
        WriteHex(fd, e.oldValue);
        WriteText(fd, " new=");
        WriteHex(fd, e.newValue);
        WriteText(fd, " holder=");
        WriteHex(fd, e.holder);
        WriteText(fd, "\n");
    }
}

void SigBWriterProvenance::Dump() noexcept
{
    bool expected = false;
    if (!dumped.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }

    // Async-signal-safe-ish: no locks, open/write/close only. Torn registry/ring
    // reads are acceptable for observe-only crash dumps.
    int fd = -1;
    const char* basePath = std::getenv("MRT_FATAL_ATTR_PATH");
    if (basePath != nullptr && *basePath != '\0') {
        char path[512];
        size_t i = 0;
        for (; basePath[i] != '\0' && i + 32 < sizeof(path); ++i) {
            path[i] = basePath[i];
        }
        const char* suffix = ".sigbwriter.";
        for (size_t j = 0; suffix[j] != '\0' && i + 1 < sizeof(path); ++j) {
            path[i++] = suffix[j];
        }
        uint64_t pid = static_cast<uint64_t>(MapleRuntime::GetPid());
        char digits[20];
        size_t d = 0;
        if (pid == 0) {
            digits[d++] = '0';
        } else {
            while (pid > 0 && d < sizeof(digits)) {
                digits[d++] = static_cast<char>('0' + (pid % 10));
                pid /= 10;
            }
        }
        while (d > 0 && i + 1 < sizeof(path)) {
            path[i++] = digits[--d];
        }
        path[i] = '\0';
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    }

    if (fd >= 0) {
        DumpRegistry(fd);
        for (ThreadRing* ring = ringListHead; ring != nullptr; ring = ring->next) {
            DumpRing(fd, ring);
        }
        WriteText(fd, "[SIGB_WRITER] dump_done\n");
        (void)close(fd);
    }
}

void SigBWriterProvenance::EnsureExitDumpRegistered() noexcept
{
    bool expected = false;
    if (!exitHookRegistered.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }
    (void)std::atexit([]() { SigBWriterProvenance::Instance().Dump(); });
}

void SigBWriterProvenance::LogSummary() noexcept
{
    EnsureExitDumpRegistered();
    VLOG(REPORT,
         "[SIGB_WRITER] summary registry=%zu overflow=%zu write_hits=%zu copy_hits=%zu relocates=%zu "
         "hashmapentry_allocs=%zu type_samples=%zu",
         registrySize.load(std::memory_order_relaxed), overflowCount.load(std::memory_order_relaxed),
         writeHitCount.load(std::memory_order_relaxed), copyHitCount.load(std::memory_order_relaxed),
         relocateCount.load(std::memory_order_relaxed),
         hashMapEntryAllocCount.load(std::memory_order_relaxed),
         typeNameSampleCount.load(std::memory_order_relaxed));
    const size_t sc = typeNameSampleCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < sc && i < SAMPLE_CAP; ++i) {
        VLOG(REPORT, "[SIGB_TYPE_SAMPLE] %s", typeNameSamples[i] == nullptr ? "<null>" : typeNameSamples[i]);
    }
}
} // namespace MapleRuntime
