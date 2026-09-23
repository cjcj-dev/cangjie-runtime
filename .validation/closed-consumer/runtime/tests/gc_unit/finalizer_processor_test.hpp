// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zReferenceProcessor.hpp"
namespace MapleRuntime {
class FinalizerProcessorTest {
public:
    static bool EnqueueRegistered(FinalizerProcessor& processor, BaseObject* object)
    { return processor.EnqueueFinalizableReference(object); }
    static bool Queue(FinalizerProcessor& processor, BaseObject* object)
    {
        processor.RegisterFinalizer(object);
        return EnqueueRegistered(processor, object);
    }
    static void FinishBatch(FinalizerProcessor& processor) { processor.FinishFinalizableBatch(); }
    static bool HasJob(FinalizerProcessor& processor) { return processor.HasFinalizableJob(); }
};
}
