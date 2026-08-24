// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#if defined(MRT_M0_CORRELATION_EXPERIMENT) && defined(MRT_GC_UNIT_TEST_ACCESS)

#include "Heap/Verify/M0Correlation.h"
#include "Heap/Verify/M0ExitDiagnostics.h"
#include "gc_heap_fixture.hpp"
#include "gc_unittest.hpp"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
M0Correlation::ObjectStamp ValidStamp(uintptr_t address, uintptr_t start, uint64_t life)
{
    M0Correlation::ObjectStamp stamp;
    stamp.valid = true;
    stamp.address = address;
    stamp.regionStart = start;
    stamp.regionLife = life;
    stamp.offset = address - start;
    return stamp;
}
} // namespace

GC_TEST(M0Correlation, NewAllocationAtSameStampGetsNewToken)
{
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp stamp = ValidStamp(0x100028u, 0x100000u, 7);
    const M0Correlation::AllocationToken first = M0Correlation::BindStampForTest(11, stamp);
    const M0Correlation::AllocationToken second = M0Correlation::BindStampForTest(12, stamp);
    GC_EXPECT_NE(first, 0u);
    GC_EXPECT_NE(second, 0u);
    GC_EXPECT_NE(first, second);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(stamp), second);
}

GC_TEST(M0Correlation, InvalidPresentEndpointInvalidatesCandidate)
{
    const M0Correlation::ObjectStamp good = ValidStamp(0x200030u, 0x200000u, 9);
    GC_EXPECT_TRUE(M0Correlation::ValidateEndpointForTest(true, good));
    GC_EXPECT_TRUE(M0Correlation::ValidateEndpointForTest(false, {}));

    M0Correlation::ObjectStamp zeroLife = good;
    zeroLife.regionLife = 0;
    GC_EXPECT_FALSE(M0Correlation::ValidateEndpointForTest(true, zeroLife));

    M0Correlation::ObjectStamp inconsistent = good;
    inconsistent.address += 8;
    GC_EXPECT_FALSE(M0Correlation::ValidateEndpointForTest(true, inconsistent));

    M0Correlation::ObjectStamp invalid = good;
    invalid.valid = false;
    GC_EXPECT_FALSE(M0Correlation::ValidateEndpointForTest(true, invalid));
}

GC_TEST(M0Correlation, RegionResetReuseMintsNewToken)
{
    GC_EXPECT_TRUE(M0Correlation::Enabled());
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp oldStamp = M0Correlation::CaptureStamp(fx.obj0);
    const M0Correlation::AllocationToken oldToken = M0Correlation::BindStampForTest(21, oldStamp);
    GC_EXPECT_NE(oldToken, 0u);

    fx.region0->InitRegion(1, RegionInfo::UnitRole::SMALL_SIZED_UNITS);
    fx.region0->SetRegionType(RegionInfo::RegionType::THREAD_LOCAL_REGION);
    const M0Correlation::ObjectStamp newStamp = M0Correlation::CaptureStamp(fx.obj0);
    GC_EXPECT_NE(newStamp.regionLife, oldStamp.regionLife);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(oldStamp), 0u);

    const M0Correlation::AllocationToken newToken = M0Correlation::BindStampForTest(22, newStamp);
    GC_EXPECT_NE(newToken, 0u);
    GC_EXPECT_NE(newToken, oldToken);
    GC_EXPECT_EQ(M0Correlation::LookupStampForTest(newStamp), newToken);
}

GC_TEST(M0Correlation, LedgerBypassesHumanDetailCap)
{
    GC_EXPECT_TRUE(M0Correlation::Enabled());
    GcHeapFixture fx;
    M0Correlation::ResetForTest();
    const M0Correlation::ObjectStamp stamp = M0Correlation::CaptureStamp(fx.obj0);
    const M0Correlation::AllocationToken token = M0Correlation::BindStampForTest(31, stamp);
    GC_EXPECT_NE(token, 0u);

    const M0ExitDiagnostics::Counts before = M0ExitDiagnostics::GetCounts();
    constexpr uint64_t kEvents = 35;
    for (uint64_t i = 0; i < kEvents; ++i) {
        M0ExitDiagnostics::Note(M0ExitDiagnostics::Exit::ReadBarrier, fx.obj0, nullptr, fx.obj1, 7);
    }
    const M0ExitDiagnostics::Counts after = M0ExitDiagnostics::GetCounts();
    const M0Correlation::TestSnapshot ledger = M0Correlation::SnapshotForTest();
    GC_EXPECT_EQ(after.total, before.total + kEvents);
    GC_EXPECT_EQ(after.sampled, M0ExitDiagnostics::kDetailedSampleLimit);
    GC_EXPECT_EQ(ledger.m0Seen, kEvents);
    GC_EXPECT_EQ(ledger.m0Written, kEvents);
    M0Correlation::Release(31);
}

#endif
