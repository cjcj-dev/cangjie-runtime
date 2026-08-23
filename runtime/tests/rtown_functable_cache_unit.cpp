// Standalone ASan unit for BUG-19: ResetAtomicInfoArray deleted a cache that
// a concurrent GetCachedTypeInfo was still indexing. Product sites:
//   ObjectModel/MClass.h  InheritFuncTable::{ResetAtomicInfoArray,Get,Set}
//   ObjectModel/MClass.cpp TryUpdateExtensionData / GetMethodOuterTI
//
// Ownership: the same mTableMutex that publishes the map also owns the cache.
// Reset deletes only after the mutex excludes every Get/Set. No delayed-delete.
//
// Compile:
//   clang++ -std=c++14 -O1 -g -fsanitize=address \
//     runtime/tests/rtown_functable_cache_unit.cpp -o /tmp/rtown_ft_pre -DPRE_FIX=1
//   clang++ -std=c++14 -O1 -g -fsanitize=address \
//     runtime/tests/rtown_functable_cache_unit.cpp -o /tmp/rtown_ft_post
//
// Run:
//   ASAN_OPTIONS=halt_on_error=1:exitcode=66 /tmp/rtown_ft_pre ; echo pre_rc=$?
//   ASAN_OPTIONS=halt_on_error=1:exitcode=66 /tmp/rtown_ft_post ; echo post_rc=$?

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct Cache {
    std::atomic<int>* slots;
    size_t size;

    explicit Cache(size_t n) : slots(n == 0 ? nullptr : new std::atomic<int>[n]), size(n)
    {
        for (size_t i = 0; i < n; ++i) {
            slots[i].store(1, std::memory_order_relaxed);
        }
    }
    Cache() : slots(nullptr), size(0) {}
    Cache(const Cache&) = delete;
    Cache& operator=(Cache&& other)
    {
        delete[] slots;
        slots = other.slots;
        size = other.size;
        other.slots = nullptr;
        other.size = 0;
        return *this;
    }
    ~Cache() { delete[] slots; }

    int Get(size_t index) const { return slots[index].load(std::memory_order_acquire); }
};

struct Table {
    Cache cache;
    std::recursive_mutex mu;
    Table() : cache(2) {}
};

#if defined(PRE_FIX)
void Reset(Table* t, size_t n) { t->cache = Cache(n); }
int Read(Table* t, size_t index) { return t->cache.Get(index); }
#else
void Reset(Table* t, size_t n)
{
    std::lock_guard<std::recursive_mutex> lock(t->mu);
    t->cache = Cache(n);
}
int Read(Table* t, size_t index)
{
    std::lock_guard<std::recursive_mutex> lock(t->mu);
    return t->cache.Get(index);
}
#endif

} // namespace

int main()
{
    Table table;
    std::atomic<int> sum { 0 };
    std::atomic<bool> stop { false };
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                sum.fetch_add(Read(&table, 0), std::memory_order_relaxed);
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        Reset(&table, 4);
        Reset(&table, 2);
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : readers) {
        th.join();
    }
    const char* arm =
#if defined(PRE_FIX)
        "pre";
#else
        "post";
#endif
    std::printf("RTOWN_FUNCTABLE_CACHE arm=%s result=PASS sum=%d\n", arm, sum.load());
    return 0;
}
