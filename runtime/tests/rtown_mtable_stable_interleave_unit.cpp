// Product-path interleave check for MTableDesc stable snapshot retirement.
// The writer must detach the snapshot and wait before ResetAtomicInfoArray;
// the reader uses the same StableReadGuard as GetMTable/GetMethodOuterTI.

#include "ObjectModel/MClass.h"

#include <atomic>
#include <cstdio>
#include <thread>

using namespace MapleRuntime;

int main()
{
    MTableDesc desc(0);
    desc.mTable.emplace(42U, InheritFuncTable(nullptr, nullptr, 0));
    auto& entry = desc.mTable.at(42U);
    entry.ResetAtomicInfoArray(1);
    desc.MarkInnerResolved();
    desc.MarkOuterResolved();
    desc.PublishStableTable();

    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    std::atomic<bool> invalidated { false };
    std::thread reader([&]() {
        MTableDesc::StableReadGuard guard(desc);
        auto* stable = guard.Get();
        if (stable == nullptr || stable->find(42U) == stable->end()) {
            return;
        }
        (void)stable->at(42U)->GetCachedTypeInfo(0);
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread writer([&]() {
        desc.InvalidateStableTable();
        // This is the product mutation that used to race a lockless cache hit.
        entry.ResetAtomicInfoArray(2);
        invalidated.store(true, std::memory_order_release);
    });

    for (int i = 0; i < 10000 && !invalidated.load(std::memory_order_acquire); ++i) {
        std::this_thread::yield();
    }
    const bool blockedWhileReaderHeld = !invalidated.load(std::memory_order_acquire);
    release.store(true, std::memory_order_release);
    reader.join();
    writer.join();

    desc.PublishStableTable();
    MTableDesc::StableReadGuard recovery(desc);
    const bool republished = recovery.Get() != nullptr && recovery.Get()->find(42U) != recovery.Get()->end();
    std::printf("RTOWN_MTABLE_STABLE_INTERLEAVE result=%s blocked=%s republished=%s\n",
                blockedWhileReaderHeld && republished ? "PASS" : "FAIL",
                blockedWhileReaderHeld ? "yes" : "no", republished ? "yes" : "no");
    return blockedWhileReaderHeld && republished ? 0 : 1;
}
