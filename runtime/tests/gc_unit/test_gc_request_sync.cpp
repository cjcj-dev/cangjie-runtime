// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// ZGC zDriverPort.cpp:101-184 — send_sync per-entry, ack by seqnum, send_async single slot.

#if defined(MRT_GC_UNIT_TESTS)

#include "gc_unittest.hpp"
#include "Heap/z/zDriverPort.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace MapleRuntime;

GC_TEST(ZDriverPort, AsyncSingleSlotDedup)
{
    ZDriverPort port;
    port.send_async(ZDriverRequest(GC_REASON_YOUNG, 1, 0));
    port.send_async(ZDriverRequest(GC_REASON_HEU, 2, 0));
    const ZDriverRequest first = port.receive();
    GC_EXPECT_EQ(first.cause(), GC_REASON_YOUNG);
    port.ack();
}

GC_TEST(ZDriverPort, SyncAckSatisfiesSameCauseSeqnum)
{
    ZDriverPort port;
    std::atomic<int> done{0};
    std::thread a([&] {
        port.send_sync(ZDriverRequest(GC_REASON_YOUNG, 0, 0));
        done.fetch_add(1);
    });
    std::thread b([&] {
        port.send_sync(ZDriverRequest(GC_REASON_YOUNG, 0, 0));
        done.fetch_add(1);
    });
    const ZDriverRequest got = port.receive();
    GC_EXPECT_EQ(got.cause(), GC_REASON_YOUNG);
    port.ack();
    a.join();
    b.join();
    GC_EXPECT_EQ(done.load(), 2);
}

GC_TEST(ZDriverPort, AckDoesNotSatisfyLaterSeqnum)
{
    ZDriverPort port;
    std::atomic<bool> firstDone{false};
    std::atomic<bool> secondDone{false};
    std::thread first([&] {
        port.send_sync(ZDriverRequest(GC_REASON_FORCE, 0, 0));
        firstDone.store(true);
    });
    const ZDriverRequest got = port.receive();
    GC_EXPECT_EQ(got.cause(), GC_REASON_FORCE);
    std::thread second([&] {
        port.send_sync(ZDriverRequest(GC_REASON_FORCE, 0, 0));
        secondDone.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    port.ack();
    first.join();
    GC_EXPECT_TRUE(firstDone.load());
    GC_EXPECT_FALSE(secondDone.load());
    const ZDriverRequest got2 = port.receive();
    GC_EXPECT_EQ(got2.cause(), GC_REASON_FORCE);
    port.ack();
    second.join();
    GC_EXPECT_TRUE(secondDone.load());
}

#endif
