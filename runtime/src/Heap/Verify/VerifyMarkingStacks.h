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
constexpr size_t MARKING_GENERATION_COUNT = 2;
constexpr size_t MARKING_BOUNDARY_COUNT = 7;
constexpr size_t MARKING_CONTAINER_COUNT = 6;

#if defined(MRT_TESTABLE_INTERNALS)
struct Snapshot {
    uint64_t boundaryReceipts[MARKING_GENERATION_COUNT][MARKING_BOUNDARY_COUNT][MARKING_CONTAINER_COUNT]{};
    size_t producerMax[MARKING_GENERATION_COUNT][MARKING_CONTAINER_COUNT]{};

    uint64_t BoundaryCount(MarkingGeneration generation, MarkingBoundary boundary,
                           MarkingContainer container) const
    {
        return boundaryReceipts[static_cast<size_t>(generation)][static_cast<size_t>(boundary)]
                               [static_cast<size_t>(container)];
    }

    size_t ProducerMax(MarkingGeneration generation, MarkingContainer container) const
    {
        return producerMax[static_cast<size_t>(generation)][static_cast<size_t>(container)];
    }
};
#endif

bool Enabled();

// Record a producer population so a zero boundary receipt can be
// distinguished from a verifier that never saw work.
void NoteProducer(MarkingGeneration generation, MarkingContainer container, size_t pending);

// Each invocation owns exactly one fatal assertion. The diagnostic always
// carries the full coordinate tuple; unused indices print as -1.
void VerifyEmpty(MarkingGeneration generation, MarkingBoundary boundary, MarkingContainer container,
                 size_t pending, size_t owner = NO_MARKING_INDEX, size_t worker = NO_MARKING_INDEX,
                 size_t stripe = NO_MARKING_INDEX);

#if defined(MRT_TESTABLE_INTERNALS)
Snapshot ReadSnapshot();
#endif

} // namespace VerifyMarkingStacks
} // namespace MapleRuntime

#endif // MRT_VERIFY_MARKING_STACKS_H
