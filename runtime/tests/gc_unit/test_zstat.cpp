// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// zStat.cpp:65-240,386-517,1036-1049. The reference tree has no dedicated
// ZStat gtest; these exercise its registry and history invariants directly.
#include "gc_unittest.hpp"
#include "Base/ZStat.h"

using namespace MapleRuntime;
using namespace MapleRuntime::GcUnit;

namespace {
const ZStatSampler unobserved("Test", "Unobserved", ZStatUnit::TIME);
const ZStatPhase pause("Young Pause", "Same phase");
const ZStatPhase concurrent("Young Phase", "Same phase");
const ZStatSampler zeroDuration("Test", "Zero duration", ZStatUnit::TIME);
const ZStatCounter counter("Test", "Counter", ZStatUnit::OPS_PER_SECOND);

GC_TEST(ZStat, RegistryExistsBeforeSampling)
{
    ZStat::Initialize();
    bool found = false;
    for (const auto* value = ZStatSampler::First(); value != nullptr; value = value->Next()) {
        if (value == &unobserved) found = true;
    }
    GC_EXPECT_TRUE(found);
    const auto count = ZStatSampler::Count();
    std::vector<ZStatSamplerHistory> history(count);
    ZStat::SampleAndCollect(history);
    GC_EXPECT_EQ(ZStatSampler::Count(), count);
    GC_EXPECT_EQ(history[unobserved.Id()].Windows()[3].nsamples, 0ULL);
}

GC_TEST(ZStat, PauseAndConcurrentKeepStaticIdentity)
{
    ZStat::Initialize();
    pause.RegisterEnd(100);
    pause.RegisterEnd(200);
    concurrent.RegisterEnd(800);
    GC_EXPECT_TRUE(pause.Sampler().Id() != concurrent.Sampler().Id());
    const auto paused = pause.Sampler().CollectAndReset();
    const auto conc = concurrent.Sampler().CollectAndReset();
    GC_EXPECT_EQ(paused.sum, 300ULL);
    GC_EXPECT_EQ(paused.max, 200ULL);
    GC_EXPECT_EQ(conc.sum, 800ULL);
    GC_EXPECT_EQ(conc.max, 800ULL);
}

GC_TEST(ZStat, StwDepthCounterClassifies)
{
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), false);
    ZStat::EnterStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), true);
    ZStat::EnterStwScope();
    ZStat::ExitStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), true);
    ZStat::ExitStwScope();
    GC_EXPECT_EQ(ZStat::WorldStoppedNow(), false);
}

GC_TEST(ZStat, ZeroDurationSampleStillCounts)
{
    ZStat::Initialize();
    zeroDuration.Sample(0);
    const auto sample = zeroDuration.CollectAndReset();
    GC_EXPECT_EQ(sample.nsamples, 1ULL);
    GC_EXPECT_EQ(sample.sum, 0ULL);
}

GC_TEST(ZStat, CounterTickConsumesAndRetainsHistory)
{
    ZStat::Initialize();
    (void)counter.Sampler().CollectAndReset();
    counter.Increment(23);
    counter.Increment(19);
    std::vector<ZStatSamplerHistory> history(ZStatSampler::Count());
    ZStat::SampleAndCollect(history);
    const auto first = history[counter.Sampler().Id()].Windows()[3];
    GC_EXPECT_EQ(first.sum, 42ULL);
    GC_EXPECT_EQ(first.max, 42ULL);
    ZStat::SampleAndCollect(history);
    const auto second = history[counter.Sampler().Id()].Windows()[3];
    GC_EXPECT_EQ(second.sum, 42ULL);
    GC_EXPECT_EQ(second.nsamples, first.nsamples + 1);
}

GC_TEST(ZStat, HistoryRollsOverAllThreeLevels)
{
    ZStatSamplerHistory history;
    // Distinct first and later maxima test expiration independently of total.
    history.Add({1, 100, 100});
    for (size_t i = 1; i < 36001; ++i) history.Add({1, 2, 2});
    const auto windows = history.Windows();
    GC_EXPECT_EQ(windows[0].nsamples, 10ULL);
    GC_EXPECT_EQ(windows[0].max, 2ULL);
    GC_EXPECT_EQ(windows[1].nsamples, 601ULL);
    GC_EXPECT_EQ(windows[2].nsamples, 36001ULL);
    GC_EXPECT_EQ(windows[3].nsamples, 36001ULL);
    GC_EXPECT_EQ(windows[3].sum, 72100ULL);
    GC_EXPECT_EQ(windows[3].max, 100ULL);
}

GC_TEST(ZStat, HistoryIncludesPartialIntervals)
{
    ZStatSamplerHistory history;
    for (size_t i = 0; i < 11; ++i) history.Add({2, 10, 7});
    const auto windows = history.Windows();
    GC_EXPECT_EQ(windows[0].nsamples, 20ULL);
    GC_EXPECT_EQ(windows[1].nsamples, 22ULL);
    GC_EXPECT_EQ(windows[2].nsamples, 22ULL);
    GC_EXPECT_EQ(windows[3].Average(), 5ULL);
    GC_EXPECT_EQ(windows[3].max, 7ULL);
}
} // namespace
