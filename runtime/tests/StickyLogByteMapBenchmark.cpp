// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <time.h>

namespace {
constexpr uint8_t BARRIER_PHASE = 9;
constexpr uint8_t LINE_SHIFT = 8;
constexpr size_t LINE_SIZE = static_cast<size_t>(1) << LINE_SHIFT;
constexpr size_t HOT_LINES = 1024;
constexpr size_t HOT_WRITES = 500000000;
constexpr size_t COLD_WRITES = 4000000;
constexpr size_t BUFFER_CAPACITY = 65;

struct alignas(LINE_SIZE) ProbeLine {
    volatile uintptr_t field;
    uint8_t padding[LINE_SIZE - sizeof(uintptr_t)];
};

static_assert(sizeof(ProbeLine) == LINE_SIZE, "probe object must occupy one sticky line");

struct LineBuffer {
    uintptr_t entries[BUFFER_CAPACITY]{};
    size_t size = 0;
};

volatile uint8_t phase = 1;
volatile uintptr_t coldSink = 0;

__attribute__((noinline)) void PhaseSlowPath(ProbeLine& line, uintptr_t ref)
{
    line.field = ref;
}

__attribute__((noinline)) void RetireLineBuffer(LineBuffer& buffer)
{
    coldSink ^= buffer.entries[0] ^ buffer.entries[buffer.size - 1];
    buffer.size = 0;
}

__attribute__((noinline)) void FirstWriteSlowPath(uint8_t& loggedByte, uintptr_t lineAddress, LineBuffer& buffer)
{
    loggedByte = 1;
    if (buffer.size == BUFFER_CAPACITY) {
        RetireLineBuffer(buffer);
    }
    buffer.entries[buffer.size++] = lineAddress;
}

__attribute__((always_inline)) inline void BaselineWrite(ProbeLine& line, uintptr_t ref)
{
    if (__builtin_expect(phase >= BARRIER_PHASE, false)) {
        PhaseSlowPath(line, ref);
        return;
    }
    line.field = ref;
}

__attribute__((always_inline)) inline void LoggedWrite(ProbeLine& line, uintptr_t heapBase, uint8_t* loggedMap,
                                                       LineBuffer& buffer, uintptr_t ref)
{
    if (__builtin_expect(phase >= BARRIER_PHASE, false)) {
        PhaseSlowPath(line, ref);
        return;
    }
    uintptr_t address = reinterpret_cast<uintptr_t>(&line);
    size_t lineIndex = (address - heapBase) >> LINE_SHIFT;
    if (__builtin_expect(loggedMap[lineIndex] == 0, false)) {
        FirstWriteSlowPath(loggedMap[lineIndex], heapBase + (lineIndex << LINE_SHIFT), buffer);
    }
    line.field = ref;
}

uint64_t NanoTime()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);
}

template<bool logged>
void RunHot()
{
    std::unique_ptr<ProbeLine[]> lines(new ProbeLine[HOT_LINES]{});
    std::unique_ptr<uint8_t[]> loggedMap(new uint8_t[HOT_LINES]);
    std::memset(loggedMap.get(), 1, HOT_LINES);
    uintptr_t heapBase = reinterpret_cast<uintptr_t>(lines.get());
    LineBuffer buffer;
    constexpr size_t warmupWrites = 1000000;
    for (size_t index = 0; index < warmupWrites; ++index) {
        ProbeLine& line = lines[index & (HOT_LINES - 1)];
        logged ? LoggedWrite(line, heapBase, loggedMap.get(), buffer, index) : BaselineWrite(line, index);
    }
    uint64_t start = NanoTime();
    for (size_t index = 0; index < HOT_WRITES; ++index) {
        ProbeLine& line = lines[index & (HOT_LINES - 1)];
        logged ? LoggedWrite(line, heapBase, loggedMap.get(), buffer, index) : BaselineWrite(line, index);
    }
    uint64_t elapsed = NanoTime() - start;
    std::printf("BYTEMAP mode=%s-hot writes=%zu elapsed_ns=%lu ns_per_write=%.6f checksum=%zu sink=%zu\n",
                logged ? "logged" : "baseline", HOT_WRITES, elapsed,
                static_cast<double>(elapsed) / static_cast<double>(HOT_WRITES), lines[0].field, coldSink);
}

template<bool logged>
void RunCold()
{
    std::unique_ptr<ProbeLine[]> lines(new ProbeLine[COLD_WRITES]{});
    std::unique_ptr<uint8_t[]> loggedMap(new uint8_t[COLD_WRITES]{});
    uintptr_t heapBase = reinterpret_cast<uintptr_t>(lines.get());
    LineBuffer buffer;
    uint64_t start = NanoTime();
    for (size_t index = 0; index < COLD_WRITES; ++index) {
        logged ? LoggedWrite(lines[index], heapBase, loggedMap.get(), buffer, index) : BaselineWrite(lines[index], index);
    }
    if (buffer.size != 0) {
        RetireLineBuffer(buffer);
    }
    uint64_t elapsed = NanoTime() - start;
    std::printf("BYTEMAP mode=%s-cold writes=%zu elapsed_ns=%lu ns_per_write=%.6f checksum=%zu sink=%zu\n",
                logged ? "logged" : "baseline", COLD_WRITES, elapsed,
                static_cast<double>(elapsed) / static_cast<double>(COLD_WRITES), lines[COLD_WRITES - 1].field,
                coldSink);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <baseline-hot|logged-hot|baseline-cold|logged-cold>\n", argv[0]);
        return 2;
    }
    if (std::strcmp(argv[1], "baseline-hot") == 0) {
        RunHot<false>();
    } else if (std::strcmp(argv[1], "logged-hot") == 0) {
        RunHot<true>();
    } else if (std::strcmp(argv[1], "baseline-cold") == 0) {
        RunCold<false>();
    } else if (std::strcmp(argv[1], "logged-cold") == 0) {
        RunCold<true>();
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 2;
    }
    return 0;
}
