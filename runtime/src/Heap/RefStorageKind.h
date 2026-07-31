// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_REF_STORAGE_KIND_H
#define MRT_REF_STORAGE_KIND_H

#include <cstdint>

namespace MapleRuntime {
// Long-lived reference storage outside managed-object fields. Stack slots,
// registers, and collector-local work stacks are intentionally not represented.
enum class RefStorageKind : uint8_t {
    MUTATOR_EXCEPTION,
    MUTATOR_RAW_OBJECT,
    MUTATOR_LOCAL_FINALIZER_QUEUE,
    MUTATOR_DEFERRED_LOG_RING,
    SATB_BUFFER,
    STICKY_LOG_BUFFER,
    WEAK_REFERENCE_QUEUE,
    ALLOC_BUFFER_ROOTS,
    STATIC_ROOT_TABLE,
    EXPORT_ROOT_TABLE,
    CONCURRENCY_MODEL_ROOTS,
    FINALIZER_QUEUES,
    FIX_EDGE_SET,
    FORWARD_FACT_TABLE,
    RELOCATION_DIAGNOSTIC_TABLE,
    DISCOVERED_EXTERN_OBJECTS,
    CYCLE_REFERENCE_WORK_STACK,
    RESURRECTED_EXPORT_OBJECTS,
};

enum class RefStorageDisposition : uint8_t {
    FORWARDED,
    NOT_NEEDED_PROVEN,
    NOT_NEEDED_ASSUMED,
};

// This is a compile-time responsibility dispatcher. Every FORWARDED case names
// an existing visit, rebase, transfer, or clear site; it emits no runtime code.
constexpr RefStorageDisposition GetRefStorageDisposition(RefStorageKind kind)
{
    switch (kind) {
        // Mutator.cpp:323-325,328-332,687-693: existing root visits/forwarding.
        case RefStorageKind::MUTATOR_EXCEPTION:
        case RefStorageKind::MUTATOR_RAW_OBJECT:
        case RefStorageKind::MUTATOR_LOCAL_FINALIZER_QUEUE:
            return RefStorageDisposition::FORWARDED;

        // REPORT-framegap: smoke_full + sticky minor + stress minor, two valid
        // samples: NORMAL=980052 each; FORWARDED/LOCKED/OTHER/INVALID_REGION=0.
        // The evidence is limited to that recipe and must be revisited if a
        // different workload observes a non-NORMAL entry.
        case RefStorageKind::MUTATOR_DEFERRED_LOG_RING:
            return RefStorageDisposition::NOT_NEEDED_PROVEN;

        // WCollector.cpp:937 and SatbBuffer.h:280-289: cleared after tracing.
        case RefStorageKind::SATB_BUFFER:
            return RefStorageDisposition::FORWARDED;

        // SatbBuffer.h:47-53: this buffer stores sticky line addresses, not
        // object references; SatbBuffer.cpp:68-76 clears retained nodes.
        case RefStorageKind::STICKY_LOG_BUFFER:
            return RefStorageDisposition::NOT_NEEDED_PROVEN;

        // WCollector.cpp:934-937: consumed and cleared before forwarding.
        case RefStorageKind::WEAK_REFERENCE_QUEUE:
            return RefStorageDisposition::FORWARDED;

        // AllocBuffer.h:54-63: transferred to the tracing stack and cleared.
        case RefStorageKind::ALLOC_BUFFER_ROOTS:
            return RefStorageDisposition::FORWARDED;

        // WCollector.cpp:850-856: existing post-forward root visits.
        case RefStorageKind::STATIC_ROOT_TABLE:
        case RefStorageKind::EXPORT_ROOT_TABLE:
            return RefStorageDisposition::FORWARDED;

        // WCollector.cpp:394-404: existing preforward root visits.
        case RefStorageKind::CONCURRENCY_MODEL_ROOTS:
        case RefStorageKind::FINALIZER_QUEUES:
            return RefStorageDisposition::FORWARDED;

        // WCollector.cpp:858-865: visit/rebase followed by table clears.
        case RefStorageKind::FIX_EDGE_SET:
        case RefStorageKind::FORWARD_FACT_TABLE:
        case RefStorageKind::RELOCATION_DIAGNOSTIC_TABLE:
            return RefStorageDisposition::FORWARDED;

        // WCollector.h:61-72 and WCollector.cpp:406-455: transfer, clear,
        // and preforward the long-lived cross-language reference containers.
        case RefStorageKind::DISCOVERED_EXTERN_OBJECTS:
        case RefStorageKind::CYCLE_REFERENCE_WORK_STACK:
        case RefStorageKind::RESURRECTED_EXPORT_OBJECTS:
            return RefStorageDisposition::FORWARDED;
    }
    return RefStorageDisposition::NOT_NEEDED_ASSUMED;
}

static_assert(GetRefStorageDisposition(RefStorageKind::MUTATOR_EXCEPTION) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::MUTATOR_RAW_OBJECT) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::MUTATOR_LOCAL_FINALIZER_QUEUE) ==
                      RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::SATB_BUFFER) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::WEAK_REFERENCE_QUEUE) ==
                      RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::ALLOC_BUFFER_ROOTS) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::STATIC_ROOT_TABLE) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::EXPORT_ROOT_TABLE) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::CONCURRENCY_MODEL_ROOTS) ==
                      RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::FINALIZER_QUEUES) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::FIX_EDGE_SET) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::FORWARD_FACT_TABLE) == RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::RELOCATION_DIAGNOSTIC_TABLE) ==
                      RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::DISCOVERED_EXTERN_OBJECTS) ==
                      RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::CYCLE_REFERENCE_WORK_STACK) ==
                      RefStorageDisposition::FORWARDED &&
                  GetRefStorageDisposition(RefStorageKind::RESURRECTED_EXPORT_OBJECTS) ==
                      RefStorageDisposition::FORWARDED,
              "every reference storage kind needs an existing forwarding, rebase, transfer, or clear site");
static_assert(GetRefStorageDisposition(RefStorageKind::MUTATOR_DEFERRED_LOG_RING) ==
                      RefStorageDisposition::NOT_NEEDED_PROVEN &&
                  GetRefStorageDisposition(RefStorageKind::STICKY_LOG_BUFFER) ==
                      RefStorageDisposition::NOT_NEEDED_PROVEN,
              "reference storage kinds that need no forwarding must carry bounded evidence");
} // namespace MapleRuntime

#endif // MRT_REF_STORAGE_KIND_H
