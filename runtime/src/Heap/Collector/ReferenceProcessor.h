// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


// Test observation fragment; included only by its product translation unit.
#pragma once

#if defined(MRT_TESTABLE_INTERNALS)
namespace {
std::function<void()> g_beforeWeakCleanCasForTest;
}
#endif

#if defined(MRT_TESTABLE_INTERNALS)
void ReferenceProcessor::ProcessReferences(const IsStronglyLive& isStronglyLive,
                                           const ObserveWeakFinal& observeWeakFinal)
{
    ProcessReferencesImpl(isStronglyLive, observeWeakFinal);
}

void ReferenceProcessor::SetBeforeWeakCleanCasForTest(std::function<void()> hook)
{
    g_beforeWeakCleanCasForTest = std::move(hook);
}
#endif

