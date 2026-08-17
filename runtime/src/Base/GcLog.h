// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_LOG_H
#define MRT_GC_LOG_H

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__linux__) || defined(hongmeng)
#include <unistd.h>
#endif

namespace MapleRuntime {
// A fixed-schema record per garbage collection, so a question that comes up after a run can be
// answered by querying the run instead of adding a counter and collecting the run again. Every
// record is one line, `key=value` separated by spaces, with a stable field order and a schema
// version so a reader can refuse a record it does not understand.
//
//   [GCLOG] v=2 rec=cycle seq= kind= reason= start_ns= dur_ns= live_before= live_after=
//           collected= heap_used= threshold= rss_kb=
//   [GCLOG] v=2 rec=phase seq= name= us=
//   [GCLOG] v=3 rec=crash ...  (crash signature; always-on via write(2), see Crash())
//
// A phase record carries the seq of the cycle it belongs to, so phases join to cycles without
// relying on line adjacency. Enabled with MRT_GC_LOG=1; the cost when off is one relaxed load.
// Cycle/phase emit to stderr (always-on when enabled) so MRT_GC_LOG alone is sufficient;
// they do not depend on MRT_REPORT / WriteLog(REPORT). Crash records are independent of
// MRT_GC_LOG so a crash before GcLog init still emits.
class GcLog {
public:
    static constexpr uint32_t SCHEMA_VERSION = 2;
    // Crash records advance the schema; cycle/phase stay at v=2 for existing readers.
    static constexpr uint32_t CRASH_SCHEMA_VERSION = 3;
    // 128: longest phase name in the tree is well under this; longer ones are truncated.
    // v2: dur/start are nanoseconds (were labelled us), phase names are folded to one token,
    // and the cycle record moved so minors emit one too.
    static constexpr size_t MAX_PHASE_NAME = 128;
    // Fixed-capacity last-FATAL slot for crash assert= field. No TLS, no lock, no heap.
    static constexpr size_t FATAL_SLOT_CAP = 512;

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
        // Always-on stderr (same shape as rec=crash): MRT_GC_LOG alone must emit rec=cycle.
        // Do not route through WriteLog(REPORT) — Release gates REPORT on MRT_REPORT=<path>
        // (DEFAULT_MRT_REPORT=0), which silently dropped cycle/phase and caused false
        // "rec=cycle=0 ⇒ no GC" readings (walkcost/hostslow).
        EmitLine("[GCLOG] v=%u rec=cycle seq=%llu kind=%s reason=%s start_ns=%llu dur_ns=%llu "
                 "live_before=%zu live_after=%zu collected=%zu heap_used=%zu threshold=%zu rss_kb=%zu",
                 SCHEMA_VERSION, static_cast<unsigned long long>(seq), kind, reason,
                 static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(durNs), liveBefore,
                 liveAfter, collected, heapUsed, threshold, ResidentKB());
    }

    static void Phase(const char* name, uint64_t us)
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
        // Same always-on channel as Cycle (see Cycle comment).
        EmitLine("[GCLOG] v=%u rec=phase seq=%llu name=%s us=%llu", SCHEMA_VERSION,
                 static_cast<unsigned long long>(CurrentSeq()), safe, static_cast<unsigned long long>(us));
    }

    // Remember the most recent FATAL log body (text after the level letter). Called from
    // FormatLog immediately before abort. AS-oriented: memcpy into a fixed slot, no lock.
    // Spaces / newlines in the body are folded to '_' so assert= stays one token.
    static void RememberFatal(const char* text, size_t len)
    {
        if (text == nullptr || len == 0) {
            return;
        }
        size_t n = len < FATAL_SLOT_CAP - 1 ? len : FATAL_SLOT_CAP - 1;
        char* slot = FatalSlot();
        for (size_t i = 0; i < n; ++i) {
            char c = text[i];
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                slot[i] = '_';
            } else {
                slot[i] = c;
            }
        }
        slot[n] = '\0';
        FatalLen().store(n, std::memory_order_release);
    }

    // Copy current fatal text into out (NUL-terminated). Returns length, or 0 if empty.
    // Safe to call from a signal handler: only relaxed/acquire loads + stack memcpy.
    static size_t CopyFatal(char* out, size_t outCap)
    {
        if (out == nullptr || outCap == 0) {
            return 0;
        }
        size_t n = FatalLen().load(std::memory_order_acquire);
        if (n == 0) {
            out[0] = '\0';
            return 0;
        }
        if (n >= outCap) {
            n = outCap - 1;
        }
        const char* slot = FatalSlot();
        for (size_t i = 0; i < n; ++i) {
            out[i] = slot[i];
        }
        out[n] = '\0';
        return n;
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
    // Fixed buffer + fprintf(stderr): independent of LogFile REPORT enablement and of any
    // MRT_REPORT file path. Mirrors rec=crash intent (always visible when the feature is on)
    // without requiring signal-handler AS-safety (cycle/phase run on the GC thread).
    static void EmitLine(const char* format, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, format);
        int n = vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        if (n < 0) {
            return;
        }
        if (static_cast<size_t>(n) >= sizeof(buf)) {
            n = static_cast<int>(sizeof(buf) - 1);
        }
        buf[n] = '\0';
        std::fprintf(stderr, "%s\n", buf);
        std::fflush(stderr);
    }

    static std::atomic<uint64_t>& CycleCounter()
    {
        static std::atomic<uint64_t> counter{ 0 };
        return counter;
    }

    static char* FatalSlot()
    {
        static char slot[FATAL_SLOT_CAP] = {};
        return slot;
    }

    static std::atomic<size_t>& FatalLen()
    {
        static std::atomic<size_t> len{ 0 };
        return len;
    }

    static bool ReadEnabledFromEnv()
    {
        const char* env = std::getenv("MRT_GC_LOG");
        return env != nullptr && std::strcmp(env, "0") != 0;
    }
};
} // namespace MapleRuntime
#endif // MRT_GC_LOG_H
