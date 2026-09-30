// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#pragma once
#include "Heap/z/zMark.hpp"
#include "Heap/z/zMarkContext.hpp"
#include "gc_worker_fixture.hpp"
namespace MapleRuntime::GcUnit {
struct MarkConsumerFixture {
    WorkerFixture worker;
    ZMark domain;
    MarkThreadLocalStacks stacks;
    MarkContext context;
    explicit MarkConsumerFixture(size_t stripes = 1)
        : domain(stripes, MarkingStacks::MarkingGeneration::MAJOR), stacks(stripes),
          context(1, 0, domain.Stripes(), stacks)
    {
        domain.ResizeWorkers(1);
        domain.Stripes().SetNStripes(stripes);
    }
    void Consume(const MarkStackEntry& entry) {
        stacks.Push(domain.Stripes(), context.Stripe(), entry, false);
        domain.Drain(context, 0);
    }
};
}
