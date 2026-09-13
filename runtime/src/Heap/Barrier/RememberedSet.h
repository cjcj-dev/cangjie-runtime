// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


// Test observation fragment; included only by its product translation unit.
#pragma once

#if defined(MRT_GC_UNIT_TESTS)
namespace {
thread_local bool flipTouchAccountingActive = false;
thread_local size_t flipBitmapWordTouches = 0;
thread_local size_t flipDirtyWordTouches = 0;
} // namespace
#endif

#if defined(MRT_GC_UNIT_TESTS)
void RememberedSet::ResetFlipTouchCountsForTest()
{
    flipBitmapWordTouches = 0;
    flipDirtyWordTouches = 0;
}

RememberedSet::FlipTouchCounts RememberedSet::ReadFlipTouchCountsForTest() const
{
    return FlipTouchCounts { flipBitmapWordTouches, flipDirtyWordTouches };
}

RememberedSet::FlipTouchCounts RememberedSet::MeasureClearBufferTouchesForTest(size_t buffer)
{
    CHECK_DETAIL(buffer < kBufferCount, "invalid remembered-set test buffer %zu", buffer);
    ResetFlipTouchCountsForTest();
    flipTouchAccountingActive = true;
    (void)ClearBuffer(buffer);
    flipTouchAccountingActive = false;
    return ReadFlipTouchCountsForTest();
}
#endif

