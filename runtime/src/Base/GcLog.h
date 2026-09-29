// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_LOG_H
#define MRT_GC_LOG_H

#include <atomic>
#include "Heap/z/zGCIdPrinter.hpp"
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
//   [GCLOG] v=6 rec=cycle seq= gc_tag=- name= cause= event=start|abort
//   [GCLOG] v=6 rec=cycle seq= gc_tag=- name= cause= event=end start_ns= dur_ns= used_at_start= used_at_end=
//   [GCLOG] v=6 rec=generation seq= gc_tag= name= event=start|abort
//   [GCLOG] v=6 rec=generation seq= gc_tag= name= event=end start_ns= dur_ns=
//           used_at_collection_start= used_at_collection_end=
//   [GCLOG] v=5 rec=phase seq= gc_tag= name= kind= start_ns= ns=
//   [GCLOG] v=5 rec=stw seq= gc_tag= reason= start_ns= wait_ns= held_ns=
//   [GCLOG] v=3 rec=crash ...  (crash signature; always-on via write(2), see Crash())
//
// gc_tag is y for a minor, Y/O for the young/old parts of a major, and - without a registered ID.
// A phase record carries the thread context ID, so phases join to collections without
// relying on line adjacency. Enabled with MRT_GC_LOG=1; the cost when off is one relaxed load.
// Cycle/phase emit to stderr (always-on when enabled) so MRT_GC_LOG alone is sufficient;
// they do not depend on MRT_REPORT / WriteLog(REPORT). Crash records are independent of
// MRT_GC_LOG so a crash before GcLog init still emits.
class GcLog {
public:
    static constexpr uint32_t SCHEMA_VERSION = 6;
    // Crash records remain independently emitted and parsed at v3.
    static constexpr uint32_t CRASH_SCHEMA_VERSION = 3;
    // 128: longest phase name in the tree is well under this; longer ones are truncated.
    // v3: all machine durations are nanoseconds and all names are folded to one token.
    static constexpr size_t MAX_PHASE_NAME = 128;
    // Fixed-capacity last-FATAL slot for crash assert= field. No TLS, no lock, no heap.
    static constexpr size_t FATAL_SLOT_CAP = 512;

    static bool Enabled()
    {
        static const bool enabled = ReadEnabledFromEnv();
        return enabled;
    }

    static uint64_t CurrentSeq() { return GCIdMark::Current(); }

    // ZGC zStat.cpp:654-691: collection records have no generation prefix.
    static void Collection(uint64_t seq, const char* name, const char* cause, const char* event,
                           uint64_t startNs = 0, uint64_t durNs = 0,
                           size_t usedAtStart = 0, size_t usedAtEnd = 0)
    {
        if (!Enabled()) { return; }
        char safeName[MAX_PHASE_NAME + 1];
        char safeCause[MAX_PHASE_NAME + 1];
        FoldToToken(name, safeName);
        FoldToToken(cause, safeCause);
        if (strcmp(event, "end") != 0) {
            EmitLine("[GCLOG] v=%u rec=cycle seq=%llu gc_tag=- name=%s cause=%s event=%s",
                     SCHEMA_VERSION, static_cast<unsigned long long>(seq), safeName, safeCause, event);
            return;
        }
        EmitLine("[GCLOG] v=%u rec=cycle seq=%llu gc_tag=- name=%s cause=%s event=end "
                 "start_ns=%llu dur_ns=%llu used_at_start=%zu used_at_end=%zu",
                 SCHEMA_VERSION, static_cast<unsigned long long>(seq), safeName, safeCause,
                 static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(durNs),
                 usedAtStart, usedAtEnd);
    }

    // ZGC zStat.cpp:703-741: generation events remain inside the y/Y/O scope.
    static void Generation(uint64_t seq, const char* name, const char* event,
                           uint64_t startNs = 0, uint64_t durNs = 0,
                           size_t usedAtStart = 0, size_t usedAtEnd = 0)
    {
        if (!Enabled()) { return; }
        char safeName[MAX_PHASE_NAME + 1];
        FoldToToken(name, safeName);
        if (strcmp(event, "end") != 0) {
            EmitLine("[GCLOG] v=%u rec=generation seq=%llu gc_tag=%c name=%s event=%s",
                     SCHEMA_VERSION, static_cast<unsigned long long>(seq), ZGCIdPrinter::Tag(seq), safeName, event);
            return;
        }
        EmitLine("[GCLOG] v=%u rec=generation seq=%llu gc_tag=%c name=%s event=end start_ns=%llu dur_ns=%llu "
                 "used_at_collection_start=%zu used_at_collection_end=%zu", SCHEMA_VERSION,
                 static_cast<unsigned long long>(seq), ZGCIdPrinter::Tag(seq), safeName,
                 static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(durNs),
                 usedAtStart, usedAtEnd);
    }

    static void Phase(uint64_t seq, const char* name, const char* kind, uint64_t startNs, uint64_t ns)
    {
        if (!Enabled()) {
            return;
        }
        char safe[MAX_PHASE_NAME + 1];
        FoldToToken(name, safe);
        // Same stderr channel as the collection lifecycle records.
        EmitLine("[GCLOG] v=%u rec=phase seq=%llu gc_tag=%c name=%s kind=%s start_ns=%llu ns=%llu", 5u,
                 static_cast<unsigned long long>(seq), ZGCIdPrinter::Tag(seq), safe, kind,
                 static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(ns));
    }

    // One record per stop-the-world, because neither of the two records above can answer "how long
    // was the world stopped".  rec=cycle is wall time for the whole collection and rec=phase is
    // work, and a phase timer reads the same whether its body ran inside the pause or beside a
    // running mutator -- so a change that moves work out of the pause and a change that makes the
    // work slower are indistinguishable in both.  Measured 2026-08-18: turning young concurrent
    // mark+follow on moved `mark_closure` into the concurrent window and every available ruler
    // showed it as a regression, because none of them measured a pause.
    //
    // wait_ns is the rendezvous (from the request until the last mutator has parked) and held_ns is
    // the body.  They are reported apart because they respond to different things: wait_ns tracks
    // safepoint polling density in the mutators, held_ns tracks what the collector does with the
    // world stopped.  Mutators are progressively blocked during wait_ns, so a pause budget must
    // count both -- ZGC's pause tracer likewise starts at the VM operation, not at the last arrival.
    static void Stw(const char* reason, uint64_t startNs, uint64_t waitNs, uint64_t heldNs)
    {
        if (!Enabled()) {
            return;
        }
        char safe[MAX_PHASE_NAME + 1];
        FoldToToken(reason, safe);
        EmitLine("[GCLOG] v=%u rec=stw seq=%llu gc_tag=%c reason=%s start_ns=%llu wait_ns=%llu held_ns=%llu", 5u,
                 static_cast<unsigned long long>(CurrentSeq()), ZGCIdPrinter::Tag(CurrentSeq()), safe,
                 static_cast<unsigned long long>(startNs), static_cast<unsigned long long>(waitNs),
                 static_cast<unsigned long long>(heldNs));
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

    // Names and reasons are free text at the call sites ("enum roots & update old pointers within",
    // "young collection"), and a space would end the value halfway through for any key=value
    // reader. Fold anything outside the safe set into '_' so a value is always one token.
    // `out` must have room for MAX_PHASE_NAME + 1.
    static void FoldToToken(const char* text, char* out)
    {
        size_t i = 0;
        if (text != nullptr) {
            for (; i < MAX_PHASE_NAME && text[i] != '\0'; ++i) {
                char c = text[i];
                bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == '_' || c == '-';
                out[i] = keep ? c : '_';
            }
        }
        if (i == 0) {
            out[i++] = '_';
        }
        out[i] = '\0';
    }

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
