// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
// Test observation fragment, included by the product PostTrace translation unit.
#pragma once

#if defined(MRT_TESTABLE_INTERNALS)
void Collector::ObserveExportOwnershipForTest(bool afterHandoff)
{
    if (!testExportOwnershipResult) {
        return;
    }
    ExportOwnershipTestObservation observation;
    observation.afterHandoff = afterHandoff;
    {
        std::lock_guard<std::mutex> lock(externMtx);
        observation.discoveredOwners = discoveredExternObjects.size();
        for (const auto& owner : discoveredExternObjects) {
            for (const auto& value : owner.second) {
                observation.discovered.emplace_back(owner.first, value);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
        observation.handoffOwners = cycleRefWorkStack.size();
        for (const auto& owner : cycleRefWorkStack) {
            for (const auto& value : owner.second) {
                observation.handoff.emplace_back(owner.first, value);
            }
        }
    }
    // No references to mutable product carriers escape this observation.
    testExportOwnershipResult(observation);
}
#endif
