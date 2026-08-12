// Standalone unit for BUG-20: exiting threads leaked ThreadCache (and any
// freelist blocks still sitting in it). Product sites:
//   Common/NativeAllocator.cpp  new ThreadCache
//   Mutator/ThreadLocal.cpp     CleanThreadLocalData dtor (no delete)
//   Common/ThreadCache.cpp      no Flush / no destructor
//
// Pre-fix arm: each thread new's a ThreadCache-sized object + one leftover
// block and does not release them. RSS must grow; LSAN must report leaks.
// Post-fix arm: same create/use then Flush+delete. RSS stays flat; LSAN=0.
//
// Compile (either arm):
//   clang++ -std=c++14 -O1 -g runtime/tests/rtown_threadcache_tls_unit.cpp \
//     -o /tmp/rtown_tc_pre  -DPRE_FIX=1
//   clang++ -std=c++14 -O1 -g runtime/tests/rtown_threadcache_tls_unit.cpp \
//     -o /tmp/rtown_tc_post -fsanitize=leak
//
// Run:
//   /tmp/rtown_tc_pre
//   ASAN_OPTIONS=detect_leaks=1 /tmp/rtown_tc_post

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

namespace {

constexpr size_t NFREELIST = 208;
constexpr int kThreads = 100000;
constexpr int kWarmup = 2000;
constexpr size_t kBlock = 64;

struct FreeList {
    size_t size = 0;
    size_t adjustSize = 1;
    void* freeList = nullptr;

    void PushLike(void* obj)
    {
        freeList = obj;
        size = obj == nullptr ? 0 : 1;
    }
};

struct ThreadCache {
    FreeList freeLists[NFREELIST];
    void* leftover = nullptr;

    void UseOnce()
    {
        leftover = std::malloc(kBlock);
        if (leftover != nullptr) {
            std::memset(leftover, 0xab, kBlock);
        }
        freeLists[0].PushLike(leftover);
    }

    void Flush()
    {
        if (leftover != nullptr) {
            std::free(leftover);
            leftover = nullptr;
        }
        freeLists[0].freeList = nullptr;
        freeLists[0].size = 0;
    }
};

#if defined(PRE_FIX)
void* ThreadMain(void*)
{
    ThreadCache* cache = new (std::nothrow) ThreadCache();
    if (cache == nullptr) {
        return reinterpret_cast<void*>(1);
    }
    cache->UseOnce();
    return nullptr;
}
#else
void* ThreadMain(void*)
{
    ThreadCache* cache = new (std::nothrow) ThreadCache();
    if (cache == nullptr) {
        return reinterpret_cast<void*>(1);
    }
    cache->UseOnce();
    cache->Flush();
    delete cache;
    return nullptr;
}
#endif

long ReadVmRssKb()
{
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) {
        return -1;
    }
    char line[256];
    long rss = -1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            rss = std::strtol(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return rss;
}

int RunBatch(int n)
{
    for (int i = 0; i < n; ++i) {
        pthread_t th;
        if (pthread_create(&th, nullptr, ThreadMain, nullptr) != 0) {
            return 1;
        }
        void* ret = nullptr;
        if (pthread_join(th, &ret) != 0) {
            return 1;
        }
        if (ret != nullptr) {
            return 1;
        }
    }
    return 0;
}

} // namespace

int main()
{
    if (RunBatch(kWarmup) != 0) {
        std::printf("RTOWN_THREADCACHE result=FAIL warmup\n");
        return 1;
    }
    long rss0 = ReadVmRssKb();
    if (RunBatch(kThreads) != 0) {
        std::printf("RTOWN_THREADCACHE result=FAIL spawn rss0=%ld\n", rss0);
        return 1;
    }
    long rss1 = ReadVmRssKb();
    long deltaKb = rss1 - rss0;
    const char* arm =
#if defined(PRE_FIX)
        "pre";
#else
        "post";
#endif
    bool growLoud = deltaKb >= 8 * 1024;     // 8 MiB: old arm must ring
    bool stayFlat = deltaKb < 2 * 1024;      // 2 MiB: new arm contract
#if defined(PRE_FIX)
    bool ok = growLoud;
#else
    bool ok = stayFlat;
#endif
    std::printf("RTOWN_THREADCACHE arm=%s result=%s threads=%d warmup=%d "
                "rss0_kb=%ld rss1_kb=%ld delta_kb=%ld growLoud=%d stayFlat=%d "
                "cache_bytes=%zu\n",
                arm, ok ? "PASS" : "FAIL", kThreads, kWarmup, rss0, rss1, deltaKb,
                growLoud ? 1 : 0, stayFlat ? 1 : 0, sizeof(ThreadCache));
    return ok ? 0 : 1;
}
