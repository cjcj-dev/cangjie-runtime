// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
#include "Heap/Collector/MarkingStacks.h"
#include "Heap/z/zVerify.hpp"
#include "Heap/Collector/MarkEngine.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadLocal.h"
namespace MapleRuntime {
namespace MarkingStacks {
// zMark.cpp:1016-1028. Inspect the same per-generation containers used by
// ThreadLocal::GetMarkStacks; verification must not flush or create a stack.
void VerifyAllEmpty(MarkDomain& domain)
{
    if (!ZVerifyMarking) { return; }
    const size_t index = domain.Generation() == MarkingGeneration::YOUNG ? 0 : 1;
    MutatorManager::Instance().VisitMarkingThreads([&](const ThreadLocalData* tls) {
        if (tls == nullptr || tls->gcData == nullptr) { return; }
        const auto& stacks = tls->gcData->markStacks[index];
        CHECK_DETAIL(stacks == nullptr || stacks->IsEmpty(),
                     "Thread marking stack is not empty: thread=%p generation=%zu", tls, index);
    });
    CHECK_DETAIL(domain.Stripes().IsEmpty(), "Shared marking stripes are not empty");
}

void VerifyEmpty(size_t pending)
{
    if (ZVerifyMarking) { CHECK_DETAIL(pending == 0, "Marking stack is not empty: %zu", pending); }
}
}
}
