// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "gc_unittest.hpp"

#include <atomic>
#include <cstdlib>
#include <thread>

#include "Heap/Collector/RelocationSetTxn.h"

namespace MapleRuntime {
namespace {

ZForwarding* Envelope(uintptr_t value)
{
    return reinterpret_cast<ZForwarding*>(value);
}

void PublishFake(Generation generation, MAddress start, size_t participants, uintptr_t envelopeBase)
{
    RelocationSetTxn::Builder builder(generation);
    for (size_t i = 0; i < participants; ++i) {
        GC_EXPECT_TRUE(builder.AddParticipantForTest(nullptr, start + i * 0x1000, 0x1000,
                                                     static_cast<RegionLifeId>(100 + i),
                                                     Envelope(envelopeBase + i * 0x10)));
    }
    GC_EXPECT_TRUE(builder.TryPublish());
}

} // namespace

GC_TEST(RelocationSetTxn, FailureAtKRollsBackWithoutPublishing)
{
    PublishFake(Generation::Young, 0x100000, 2, 0x1000);
    const size_t beforeParticipants = RelocationSetTxn::ActiveParticipantsForTest(Generation::Young);
    const auto before = RelocationSetTxn::GetCounters();
    (void)setenv("CJRT_RELOC_TXN_FAIL_PARTICIPANT", "2", 1);
    {
        RelocationSetTxn::Builder builder(Generation::Young);
        GC_EXPECT_TRUE(builder.AddParticipantForTest(nullptr, 0x200000, 0x1000, 201, Envelope(0x2000)));
        GC_EXPECT_FALSE(builder.AddParticipantForTest(nullptr, 0x201000, 0x1000, 202, Envelope(0x2010)));
        GC_EXPECT_FALSE(builder.TryPublish());
    }
    (void)unsetenv("CJRT_RELOC_TXN_FAIL_PARTICIPANT");
    const auto after = RelocationSetTxn::GetCounters();
    GC_EXPECT_EQ(RelocationSetTxn::ActiveParticipantsForTest(Generation::Young), beforeParticipants);
    GC_EXPECT_EQ(after.rollback, before.rollback + 1);
    GC_EXPECT_EQ(after.handleLeaks, 0U);
}

GC_TEST(RelocationSetTxn, ReaderSeesOnlyWholeOldOrNewSet)
{
    constexpr MAddress base = 0x300000;
    PublishFake(Generation::Old, base, 2, 0x3000);
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> invalid{ 0 };
    std::thread reader([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            const size_t n = RelocationSetTxn::ActiveParticipantsForTest(Generation::Old);
            if (n != 2 && n != 4) {
                invalid.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    PublishFake(Generation::Old, base + 0x10000, 4, 0x4000);
    stop.store(true, std::memory_order_release);
    reader.join();
    GC_EXPECT_EQ(invalid.load(std::memory_order_relaxed), 0U);
    GC_EXPECT_EQ(RelocationSetTxn::ActiveParticipantsForTest(Generation::Old), 4U);
}

GC_TEST(RelocationSetTxn, HandlePinsRetiredTransactionUntilLastReader)
{
    constexpr MAddress base = 0x500000;
    PublishFake(Generation::Young, base, 1, 0x5000);
    auto handle = RelocationSetTxn::AcquireForAddress(base);
    GC_EXPECT_TRUE(static_cast<bool>(handle));
    const auto before = RelocationSetTxn::GetCounters();
    PublishFake(Generation::Young, base + 0x10000, 1, 0x5100);
    GC_EXPECT_EQ(handle.GetState(), RelocationSetTxn::State::REMAP_CLOSED);
    GC_EXPECT_EQ(RelocationSetTxn::GetCounters().destroyed, before.destroyed);
    handle = RelocationSetTxn::Handle();
    GC_EXPECT_EQ(RelocationSetTxn::GetCounters().destroyed, before.destroyed + 1);
}

GC_TEST(RelocationSetTxn, OutstandingBlocksRemapCloseAndDestroy)
{
    constexpr MAddress base = 0x600000;
    PublishFake(Generation::Old, base, 1, 0x6000);
    auto handle = RelocationSetTxn::AcquireForAddress(base);
    GC_EXPECT_TRUE(static_cast<bool>(handle));
    handle.AddOutstanding();
    const auto before = RelocationSetTxn::GetCounters();
    PublishFake(Generation::Old, base + 0x10000, 1, 0x6100);
    GC_EXPECT_EQ(handle.GetState(), RelocationSetTxn::State::COPY_CLOSED);
    handle.AckOutstanding();
    GC_EXPECT_EQ(handle.GetState(), RelocationSetTxn::State::REMAP_CLOSED);
    GC_EXPECT_EQ(RelocationSetTxn::GetCounters().destroyed, before.destroyed);
    handle = RelocationSetTxn::Handle();
    GC_EXPECT_EQ(RelocationSetTxn::GetCounters().destroyed, before.destroyed + 1);
}

GC_TEST(RelocationSetTxn, LookupIsLifeAndEnvelopeStamped)
{
    constexpr MAddress base = 0x700000;
    PublishFake(Generation::Young, base, 2, 0x7000);
    auto first = RelocationSetTxn::AcquireForAddress(base + 8);
    auto second = RelocationSetTxn::AcquireForAddress(base + 0x1008);
    auto miss = RelocationSetTxn::AcquireForAddress(base + 0x3000);
    GC_EXPECT_TRUE(static_cast<bool>(first));
    GC_EXPECT_TRUE(static_cast<bool>(second));
    GC_EXPECT_FALSE(static_cast<bool>(miss));
    GC_EXPECT_EQ(first.GetParticipant()->fromLife, 100U);
    GC_EXPECT_EQ(second.GetParticipant()->fromLife, 101U);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(first.GetEnvelope()), 0x7000U);
    GC_EXPECT_EQ(reinterpret_cast<uintptr_t>(second.GetEnvelope()), 0x7010U);
}

GC_TEST(RelocationSetTxn, DuplicateInstallIsRejectedAndCounted)
{
    RelocationSetTxn::Builder builder(Generation::Old);
    GC_EXPECT_TRUE(builder.AddParticipantForTest(nullptr, 0x800000, 0x1000, 800, Envelope(0x8000)));
    const auto before = RelocationSetTxn::GetCounters();
    GC_EXPECT_TRUE(builder.TryPublish());
    GC_EXPECT_FALSE(builder.TryPublish());
    GC_EXPECT_EQ(RelocationSetTxn::GetCounters().duplicateInstall, before.duplicateInstall + 1);
    RelocationSetTxn::CloseCopy(Generation::Young);
    RelocationSetTxn::CloseRemap(Generation::Young);
    RelocationSetTxn::CloseCopy(Generation::Old);
    RelocationSetTxn::CloseRemap(Generation::Old);
}

} // namespace MapleRuntime
