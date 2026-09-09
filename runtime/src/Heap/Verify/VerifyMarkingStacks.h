// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_VERIFY_MARKING_STACKS_H
#define MRT_VERIFY_MARKING_STACKS_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace MapleRuntime {
namespace VerifyMarkingStacks {

// Functional counterpart of ZVerifyMarking: stack ownership is checked at
// mark start, worker termination, join, and mark end. Object-closure
// completeness is deliberately outside this verifier.
enum class MarkingGeneration : uint8_t { MAJOR, YOUNG };
enum class MarkingBoundary : uint8_t { START, SEED_PUBLISH, TASK_EXIT, TERMINATION, WORKER_EXIT, JOIN, END };
enum class MarkingContainer : uint8_t { OWNER, FOREIGN, TASK, POOL, LOCAL, STRIPE };

constexpr size_t NO_MARKING_INDEX = std::numeric_limits<size_t>::max();

struct Snapshot {
    uint64_t majorStart = 0;
    uint64_t majorTaskExit = 0;
    uint64_t majorTermination = 0;
    uint64_t majorJoin = 0;
    uint64_t majorEnd = 0;
    uint64_t youngStart = 0;
    uint64_t youngSeedPublish = 0;
    uint64_t youngTaskExit = 0;
    uint64_t youngTermination = 0;
    uint64_t youngWorkerExit = 0;
    uint64_t youngJoin = 0;
    uint64_t youngEnd = 0;
    size_t majorOwnerProducerMax = 0;
    size_t majorTaskProducerMax = 0;
    size_t youngOwnerProducerMax = 0;
    size_t youngTaskProducerMax = 0;
    size_t youngLocalProducerMax = 0;
    size_t youngStripeProducerMax = 0;
};

bool Enabled();

// Record a producer population so a zero boundary receipt can be
// distinguished from a verifier that never saw work.
void NoteProducer(MarkingGeneration generation, MarkingContainer container, size_t pending);

// Each invocation owns exactly one fatal assertion. The diagnostic always
// carries the full coordinate tuple; unused indices print as -1.
void VerifyEmpty(MarkingGeneration generation, MarkingBoundary boundary, MarkingContainer container,
                 size_t pending, size_t owner = NO_MARKING_INDEX, size_t worker = NO_MARKING_INDEX,
                 size_t stripe = NO_MARKING_INDEX);

Snapshot ReadSnapshot();

} // namespace VerifyMarkingStacks
} // namespace MapleRuntime

#endif // MRT_VERIFY_MARKING_STACKS_H
