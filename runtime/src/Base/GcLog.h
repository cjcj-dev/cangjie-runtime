// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_LOG_H
#define MRT_GC_LOG_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <unistd.h>
#endif

#include "Base/LogFile.h"

namespace MapleRuntime {
// A fixed-schema record per garbage collection, so a question that comes up after a run can be
// answered by querying the run instead of adding a counter and collecting the run again. Every
// record is one line, `key=value` separated by spaces, with a stable field order and a schema
// version so a reader can refuse a record it does not understand.
//
//   [GCLOG] v=2 rec=cycle seq= kind= reason= start_ns= dur_ns= live_before= live_after=
//           collected= heap_used= threshold= rss_kb=
//   [GCLOG] v=2 rec=phase seq= name= us=
//
// A phase record carries the seq of the cycle it belongs to, so phases join to cycles without
// relying on line adjacency. Enabled with MRT_GC_LOG=1; the cost when off is one relaxed load.
class GcLog {
public:
    static constexpr uint32_t SCHEMA_VERSION = 2;
    // 128: longest phase name in the tree is well under this; longer ones are truncated.
    // v2: dur/start are nanoseconds (were labelled us), phase names are folded to one token,
    // and the cycle record moved so minors emit one too.
    static constexpr size_t MAX_PHASE_NAME = 128;

    static bool Enabled()
    {
        static const bool enabled = ReadEnabledFromEnv();
        return enabled;
    }

    // Cycles are numbered from 1. Phase records are emitted while a collection runs and the cycle
    // record only at the end of it, so the in-progress number is one past the count of completed
    // cycles. Both accessors return that same number for the same collection: phases read it,
    // the cycle record takes it and closes the cycle.
    static uint64_t CurrentSeq() { return CycleCounter().load(std::memory_order_relaxed) + 1; }

    static uint64_t CompleteCycle() { return CycleCounter().fetch_add(1, std::memory_order_relaxed) + 1; }

    static void Cycle(uint64_t seq, const char* kind, const char* reason, uint64_t startNs, uint64_t durNs,
                      size_t liveBefore, size_t liveAfter, size_t collected, size_t heapUsed, size_t threshold)
    {
        if (!Enabled()) {
            return;
        }
        if (ENABLE_LOG(REPORT)) {
            WriteLog(true, REPORT,
                     "[GCLOG] v=%u rec=cycle seq=%llu kind=%s reason=%s start_ns=%llu dur_ns=%llu "
                     "live_before=%zu live_after=%zu collected=%zu heap_used=%zu threshold=%zu rss_kb=%zu",
                     SCHEMA_VERSION, static_cast<unsigned long long>(seq), kind, reason,
                     static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(durNs), liveBefore,
                     liveAfter, collected, heapUsed, threshold, ResidentKB());
            return;
        }
        std::fprintf(stderr,
                     "[GCLOG] v=%u rec=cycle seq=%llu kind=%s reason=%s start_ns=%llu dur_ns=%llu "
                     "live_before=%zu live_after=%zu collected=%zu heap_used=%zu threshold=%zu rss_kb=%zu\n",
                     SCHEMA_VERSION, static_cast<unsigned long long>(seq), kind, reason,
                     static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(durNs), liveBefore,
                     liveAfter, collected, heapUsed, threshold, ResidentKB());
    }

    static void Phase(const char* name, uint64_t us)
    {
        Phase(CurrentSeq(), name, us);
    }

    static void Phase(uint64_t seq, const char* name, uint64_t us)
    {
        if (!Enabled()) {
            return;
        }
        // Phase names are free text at the call sites ("enum roots & update old pointers within"),
        // and a space would end the value halfway through for any key=value reader. Fold anything
        // outside the safe set into '_' so a name is always one token.
        char safe[MAX_PHASE_NAME + 1];
        size_t i = 0;
        for (; i < MAX_PHASE_NAME && name[i] != '\0'; ++i) {
            char c = name[i];
            bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
            safe[i] = keep ? c : '_';
        }
        safe[i] = '\0';
        if (ENABLE_LOG(REPORT)) {
            WriteLog(true, REPORT, "[GCLOG] v=%u rec=phase seq=%llu name=%s us=%llu", SCHEMA_VERSION,
                     static_cast<unsigned long long>(seq), safe, static_cast<unsigned long long>(us));
            return;
        }
        std::fprintf(stderr, "[GCLOG] v=%u rec=phase seq=%llu name=%s us=%llu\n", SCHEMA_VERSION,
                     static_cast<unsigned long long>(seq), safe, static_cast<unsigned long long>(us));
    }

    // Resident set in KB, read from /proc/self/statm. Returns 0 where the file is unavailable,
    // which a reader must treat as "not measured" rather than as zero residency.
    static size_t ResidentKB()
    {
#if defined(__linux__) || defined(hongmeng)
        FILE* statm = fopen("/proc/self/statm", "re");
        if (statm == nullptr) {
            return 0;
        }
        unsigned long long totalPages = 0;
        unsigned long long residentPages = 0;
        int matched = fscanf(statm, "%llu %llu", &totalPages, &residentPages);
        (void)fclose(statm);
        // 2: both fields must be present, otherwise the read is not usable.
        if (matched != 2) {
            return 0;
        }
        long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) {
            return 0;
        }
        // 1024: bytes per KB.
        return static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize) / 1024;
#else
        return 0;
#endif
    }

private:
    static std::atomic<uint64_t>& CycleCounter()
    {
        static std::atomic<uint64_t> counter{ 0 };
        return counter;
    }

    static bool ReadEnabledFromEnv()
    {
        const char* env = std::getenv("MRT_GC_LOG");
        return env != nullptr && std::strcmp(env, "0") != 0;
    }
};
} // namespace MapleRuntime
#endif // MRT_GC_LOG_H
