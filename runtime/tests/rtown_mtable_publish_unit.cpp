// Standalone TSan unit for BUG-18: lock-free MTable fast path raced with
// unordered_map mutation. Product sites:
//   ObjectModel/MClass.h  MTableDesc::{needsResolve*,mTable,mTableMutex}
//   ObjectModel/MClass.cpp GetMTable / FindExtensionData / SetMTableDesc
//
// Pre-fix: publisher writes the map then a plain bool; reader reads the bool
// then finds in the map. TSan must report a race.
// Post-fix: publisher fills the map under mutex then release-stores the flag;
// reader acquire-loads the flag then finds under the same mutex. TSan=0.
//
// Compile:
//   clang++ -std=c++14 -O1 -g -fsanitize=thread \
//     runtime/tests/rtown_mtable_publish_unit.cpp -o /tmp/rtown_mt_pre -DPRE_FIX=1
//   clang++ -std=c++14 -O1 -g -fsanitize=thread \
//     runtime/tests/rtown_mtable_publish_unit.cpp -o /tmp/rtown_mt_post
//
// Run:
//   TSAN_OPTIONS=halt_on_error=1:exitcode=66 /tmp/rtown_mt_pre ; echo pre_rc=$?
//   TSAN_OPTIONS=halt_on_error=1:exitcode=66 /tmp/rtown_mt_post ; echo post_rc=$?

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kThreads = 32;
constexpr int kIters = 2000;
constexpr uint32_t kKey = 42;

struct Table {
    std::unordered_map<uint32_t, int> mTable;
    std::recursive_mutex mTableMutex;
#if defined(PRE_FIX)
    bool ready = false;
#else
    std::atomic<bool> ready { false };
#endif
};

#if defined(PRE_FIX)
void Publish(Table* t)
{
    t->mTable.emplace(kKey, 1);
    t->ready = true;
}

int Observe(Table* t)
{
    if (t->ready) {
        auto it = t->mTable.find(kKey);
        return it == t->mTable.end() ? -1 : it->second;
    }
    std::lock_guard<std::recursive_mutex> lock(t->mTableMutex);
    if (!t->ready) {
        t->mTable.emplace(kKey, 1);
        t->ready = true;
    }
    auto it = t->mTable.find(kKey);
    return it == t->mTable.end() ? -1 : it->second;
}
#else
void Publish(Table* t)
{
    std::lock_guard<std::recursive_mutex> lock(t->mTableMutex);
    t->mTable.emplace(kKey, 1);
    t->ready.store(true, std::memory_order_release);
}

int Observe(Table* t)
{
    if (t->ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::recursive_mutex> lock(t->mTableMutex);
        auto it = t->mTable.find(kKey);
        return it == t->mTable.end() ? -1 : it->second;
    }
    std::lock_guard<std::recursive_mutex> lock(t->mTableMutex);
    if (!t->ready.load(std::memory_order_acquire)) {
        t->mTable.emplace(kKey, 1);
        t->ready.store(true, std::memory_order_release);
    }
    auto it = t->mTable.find(kKey);
    return it == t->mTable.end() ? -1 : it->second;
}
#endif

} // namespace

int main()
{
    Table table;
    std::atomic<int> bad { 0 };
    std::vector<std::thread> threads;
    threads.emplace_back([&]() { Publish(&table); });
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&]() {
            for (int n = 0; n < kIters; ++n) {
                if (Observe(&table) != 1) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    const char* arm =
#if defined(PRE_FIX)
        "pre";
#else
        "post";
#endif
    int fails = bad.load();
    std::printf("RTOWN_MTABLE_PUBLISH arm=%s result=%s bad=%d threads=%d iters=%d\n",
                arm, fails == 0 ? "PASS" : "FAIL", fails, kThreads, kIters);
    return fails == 0 ? 0 : 1;
}
