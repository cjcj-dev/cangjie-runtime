// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.


// Test observation fragment; included only by its product translation unit.
#pragma once

#if defined(MRT_GC_UNIT_TESTS)
static thread_local WCollector::RouteLookupTestResult* g_routeLookupTestContext = nullptr;
#endif
#if defined(MRT_GC_UNIT_TESTS)
WCollector::RouteLookupTestResult WCollector::RouteLookupForTest(BaseObject* fromObj)
{
    RouteLookupTestResult result;
    struct ContextScope {
        WCollector::RouteLookupTestResult*& slot;
        WCollector::RouteLookupTestResult* previous;
        explicit ContextScope(WCollector::RouteLookupTestResult*& context,
                              WCollector::RouteLookupTestResult* current)
            : slot(context), previous(context)
        {
            slot = current;
        }
        ~ContextScope() { slot = previous; }
    } scope(g_routeLookupTestContext, &result);
    (void)TryForwardObject(fromObj, ActiveForwardingGeneration());
    return result;
}
#endif

