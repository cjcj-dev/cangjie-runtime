// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B2RingProbe.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace MapleRuntime {
namespace {

#define BR_LOG(fmt, ...)                                                                                               \
    do {                                                                                                               \
        std::fprintf(stderr, "[GCV2][b2ring] " fmt "\n", ##__VA_ARGS__);                                                \
        std::fflush(stderr);                                                                                           \
    } while (0)

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

size_t EnvSizeT(const char* name, size_t def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (end == v) {
        return def;
    }
    return static_cast<size_t>(n);
}

const char* EnvStr(const char* name, const char* def)
{
    const char* v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return def;
    }
    return v;
}

// Entry = two words only (task: zero classify).
struct RingEntry {
    uintptr_t slot;
    uintptr_t value;
};

// Per-thread ring. Allocated once via malloc; never freed (process lifetime).
struct ThreadRing {
    uint32_t cap;          // power-of-two preferred but any
    uint32_t head;         // next write index
    uint64_t total;        // total writes (may exceed cap)
    uint64_t wraps;        // times head wrapped past 0 after fill
    pid_t tid;
    ThreadRing* next;      // global registry
    RingEntry* entries;    // heap array [cap]
};

thread_local ThreadRing* gTlsRing = nullptr;
thread_local const char* gInstallPath = "sink";

// Global registry of all TLS rings for dump-all.
std::atomic<ThreadRing*> gRingList{nullptr};
std::atomic<bool> gArmedLogged{false};
std::atomic<uint32_t> gDumpSerial{0};
std::atomic<uint64_t> gGlobalNotes{0};

uint32_t CapFromEnv()
{
    size_t n = EnvSizeT("MRT_GCV2_B2RING_CAP", 262144u); // 256K × 16B ≈ 4MB/thread
    if (n < 1024u) {
        n = 1024u;
    }
    if (n > (1u << 22)) { // 4M entries max
        n = 1u << 22;
    }
    return static_cast<uint32_t>(n);
}

ThreadRing* EnsureTlsRing()
{
    ThreadRing* r = gTlsRing;
    if (r != nullptr) {
        return r;
    }
    uint32_t cap = CapFromEnv();
    void* mem = std::malloc(sizeof(ThreadRing));
    if (mem == nullptr) {
        return nullptr;
    }
    void* ents = std::calloc(cap, sizeof(RingEntry));
    if (ents == nullptr) {
        std::free(mem);
        return nullptr;
    }
    r = static_cast<ThreadRing*>(mem);
    r->cap = cap;
    r->head = 0;
    r->total = 0;
    r->wraps = 0;
    r->tid = static_cast<pid_t>(syscall(SYS_gettid));
    r->entries = static_cast<RingEntry*>(ents);
    r->next = nullptr;
    gTlsRing = r;

    // Publish into global list (lock-free push).
    ThreadRing* head = gRingList.load(std::memory_order_relaxed);
    do {
        r->next = head;
    } while (!gRingList.compare_exchange_weak(head, r, std::memory_order_release, std::memory_order_relaxed));

    return r;
}

void EnsureMkdir(const char* dir)
{
    if (dir == nullptr || dir[0] == '\0') {
        return;
    }
    // best-effort nested mkdir for /root/b2ring-run/rings
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", dir);
    size_t len = std::strlen(buf);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            ::mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
    ::mkdir(dir, 0755);
}

void DumpOneRing(FILE* f, const ThreadRing* r, uintptr_t targetObj, int* hitOut, int* wrapHitOut)
{
    if (r == nullptr || r->entries == nullptr || f == nullptr) {
        return;
    }
    uint32_t cap = r->cap;
    uint32_t head = r->head;
    uint64_t total = r->total;
    uint64_t wraps = r->wraps;
    bool wrapped = total > cap;
    uint32_t count = wrapped ? cap : static_cast<uint32_t>(total);
    // Oldest retained index:
    // if wrapped: head is next-to-write = oldest; walk head..head+cap
    // else: 0..total-1
    std::fprintf(f, "# tid=%d cap=%u head=%u total=%llu wraps=%llu wrapped=%u count=%u\n", static_cast<int>(r->tid),
                 cap, head, static_cast<unsigned long long>(total), static_cast<unsigned long long>(wraps),
                 wrapped ? 1u : 0u, count);
    int hit = 0;
    for (uint32_t j = 0; j < count; ++j) {
        uint32_t idx = wrapped ? ((head + j) % cap) : j;
        const RingEntry& e = r->entries[idx];
        int isHit = (targetObj != 0 && e.value == targetObj) ? 1 : 0;
        if (isHit) {
            ++hit;
        }
        std::fprintf(f, "%u\t%p\t%p\t%d\n", j, reinterpret_cast<void*>(e.slot), reinterpret_cast<void*>(e.value),
                     isHit);
    }
    if (hitOut != nullptr) {
        *hitOut = hit;
    }
    if (wrapHitOut != nullptr) {
        *wrapHitOut = wrapped ? 1 : 0;
    }
}

// Scan live rings for value==target; log neighbors around each hit.
void ScanLive(uintptr_t targetObj, int* anyHit, int* anyWrap)
{
    *anyHit = 0;
    *anyWrap = 0;
    if (targetObj == 0) {
        return;
    }
    ThreadRing* r = gRingList.load(std::memory_order_acquire);
    while (r != nullptr) {
        uint32_t cap = r->cap;
        uint32_t head = r->head;
        uint64_t total = r->total;
        bool wrapped = total > cap;
        uint32_t count = wrapped ? cap : static_cast<uint32_t>(total);
        if (wrapped) {
            *anyWrap = 1;
        }
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t idx = wrapped ? ((head + j) % cap) : j;
            const RingEntry& e = r->entries[idx];
            if (e.value != targetObj) {
                continue;
            }
            *anyHit = 1;
            BR_LOG("RING_HIT tid=%d seq=%u slot=%p value=%p total=%llu wraps=%llu wrapped=%u",
                   static_cast<int>(r->tid), j, reinterpret_cast<void*>(e.slot), reinterpret_cast<void*>(e.value),
                   static_cast<unsigned long long>(total), static_cast<unsigned long long>(r->wraps),
                   wrapped ? 1u : 0u);
            // neighbors ±20 in retained window
            int lo = static_cast<int>(j) - 20;
            if (lo < 0) {
                lo = 0;
            }
            int hi = static_cast<int>(j) + 20;
            if (hi >= static_cast<int>(count)) {
                hi = static_cast<int>(count) - 1;
            }
            for (int k = lo; k <= hi; ++k) {
                uint32_t kidx = wrapped ? ((head + static_cast<uint32_t>(k)) % cap) : static_cast<uint32_t>(k);
                const RingEntry& n = r->entries[kidx];
                BR_LOG("RING_NEI tid=%d rel=%+d seq=%d slot=%p value=%p %s", static_cast<int>(r->tid), k - static_cast<int>(j),
                       k, reinterpret_cast<void*>(n.slot), reinterpret_cast<void*>(n.value),
                       (static_cast<uint32_t>(k) == j) ? "<--HIT" : "");
            }
        }
        r = r->next;
    }
}

} // namespace

bool B2RingProbe::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B2RING");
    return on;
}

const char* B2RingProbe::CurrentPath()
{
    return gInstallPath != nullptr ? gInstallPath : "sink";
}

B2RingProbe::ScopedInstallPath::ScopedInstallPath(const char* path) : prev_(gInstallPath)
{
    gInstallPath = path != nullptr ? path : "null";
}

B2RingProbe::ScopedInstallPath::~ScopedInstallPath()
{
    gInstallPath = prev_;
}

void B2RingProbe::NoteInstall(const char* /*path*/, const char* /*kind*/, void* slot, void* value)
{
    // Hot path: Enabled check (static once) + two stores + index bump.
    // No Classify, no Heap::IsHeapAddress, no type walk.
    if (!Enabled()) {
        return;
    }
    ThreadRing* r = EnsureTlsRing();
    if (r == nullptr) {
        return;
    }
    if (!gArmedLogged.exchange(true, std::memory_order_relaxed)) {
        BR_LOG("ARMED env=MRT_GCV2_B2RING=1 cap=%u dir=%s (two-word ring, full coverage, zero classify)", r->cap,
               EnvStr("MRT_GCV2_B2RING_DIR", "/root/b2ring-run/rings"));
    }
    uint32_t i = r->head;
    r->entries[i].slot = reinterpret_cast<uintptr_t>(slot);
    r->entries[i].value = reinterpret_cast<uintptr_t>(value);
    uint32_t next = i + 1;
    if (next >= r->cap) {
        next = 0;
        r->wraps += 1;
    }
    r->head = next;
    r->total += 1;
    gGlobalNotes.fetch_add(1, std::memory_order_relaxed);
    (void)gInstallPath; // path intentionally not stored (two-word contract)
}

void B2RingProbe::ScanAndLog(const char* reason, uintptr_t targetObj)
{
    if (!Enabled()) {
        return;
    }
    BR_LOG("SCAN reason=%s target=%p globalNotes=%llu", reason != nullptr ? reason : "?",
           reinterpret_cast<void*>(targetObj),
           static_cast<unsigned long long>(gGlobalNotes.load(std::memory_order_relaxed)));
    int anyHit = 0;
    int anyWrap = 0;
    ScanLive(targetObj, &anyHit, &anyWrap);
    if (anyHit) {
        BR_LOG("RING_FOUND_WRITER target=%p wrap_window=%u", reinterpret_cast<void*>(targetObj), anyWrap);
    } else {
        BR_LOG("RING_NO_WRITE_FOR_VALUE target=%p wrap_window=%u (value not seen in any retained ring)",
               reinterpret_cast<void*>(targetObj), anyWrap);
    }
}

void B2RingProbe::DumpAllRings(const char* reason, uintptr_t targetObj)
{
    if (!Enabled()) {
        return;
    }
    ScanAndLog(reason, targetObj);

    const char* dir = EnvStr("MRT_GCV2_B2RING_DIR", "/root/b2ring-run/rings");
    EnsureMkdir(dir);
    uint32_t serial = gDumpSerial.fetch_add(1, std::memory_order_relaxed);
    pid_t pid = ::getpid();
    char path[640];
    std::snprintf(path, sizeof(path), "%s/ring_pid%d_s%u_%s.txt", dir, static_cast<int>(pid), serial,
                  reason != nullptr ? reason : "dump");
    // sanitize path: replace spaces
    for (char* p = path; *p; ++p) {
        if (*p == ' ' || *p == '/') {
            if (p > path + std::strlen(dir) + 1) {
                if (*p == ' ') {
                    *p = '_';
                }
            }
        }
    }
    // rebuild safely without slash in reason
    char safeReason[64];
    std::snprintf(safeReason, sizeof(safeReason), "%s", reason != nullptr ? reason : "dump");
    for (char* p = safeReason; *p; ++p) {
        if (*p == '/' || *p == ' ') {
            *p = '_';
        }
    }
    std::snprintf(path, sizeof(path), "%s/ring_pid%d_s%u_%s.txt", dir, static_cast<int>(pid), serial, safeReason);

    FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        BR_LOG("DUMP_FAIL path=%s errno=%d", path, errno);
        return;
    }
    std::fprintf(f, "# B2Ring dump reason=%s target=%p pid=%d serial=%u globalNotes=%llu\n", safeReason,
                 reinterpret_cast<void*>(targetObj), static_cast<int>(pid), serial,
                 static_cast<unsigned long long>(gGlobalNotes.load(std::memory_order_relaxed)));
    std::fprintf(f, "# format: seq\\tslot\\tvalue\\thit\n");

    int totalHits = 0;
    int rings = 0;
    int wrapRings = 0;
    ThreadRing* r = gRingList.load(std::memory_order_acquire);
    while (r != nullptr) {
        int hit = 0;
        int wrap = 0;
        DumpOneRing(f, r, targetObj, &hit, &wrap);
        totalHits += hit;
        rings += 1;
        wrapRings += wrap;
        r = r->next;
    }
    std::fprintf(f, "# SUMMARY rings=%d totalHits=%d wrapRings=%d\n", rings, totalHits, wrapRings);
    std::fclose(f);
    BR_LOG("DUMP_OK path=%s rings=%d hits=%d wrapRings=%d", path, rings, totalHits, wrapRings);
}

} // namespace MapleRuntime
