// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// ⛔ HOLLOWED — the implementation in the matching .cpp is all no-ops: Enabled() returns false and
// every sink body is empty.  The gate documented below therefore emits nothing, so a zero taken from
// it is a false negative, not evidence that the arm never fires.  The contract, the gate name and the
// product call sites were all left intact when the bodies were removed, which is precisely what makes
// this readable as a live instrument.  Restore the sink you need first -- PermWhoAdmit.cpp shows the
// shape: a compile-time constant gate (the campaign cut MRT_GCV2_* from 190 to 3) plus a line on the
// zero case so a zero cannot be read as a dead probe.  Guard: runtime/tests/check_diag_not_hollow.py
#ifndef MRT_START_WHO_DIAG_H
#define MRT_START_WHO_DIAG_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {
class BaseObject;

// startwho: caller-side breadcrumb around BaseObject::GetSize.
// Gate: MRT_GCV2_STARTWHO=1 or MRT_GCV2_DIAG token startwho. Default off.
namespace StartWhoDiag {

enum class Source : uint8_t {
    ROOT_DERIVED = 1U,
    HEAP_FIELD = 2U,
    REMSET = 4U,
};

bool Enabled();

// GcPhaseEnum roots are staged through AllocBuffer before the young work stack.
// Remember the stack-map producer here, then commit it only if the object is
// actually admitted to the young work stack.
void NoteRootCandidate(BaseObject* object, const char* site, const void* slot,
                       BaseObject* base, BaseObject* derived);
void NoteProducedRootIfPending(BaseObject* object);
void DiscardRootCandidate(BaseObject* object);

void NoteProduced(BaseObject* object, Source source, const char* site,
                  const void* slot = nullptr, BaseObject* holder = nullptr);

class ScopedCaller {
public:
    ScopedCaller(const char* caller, BaseObject* object);
    ~ScopedCaller();

    ScopedCaller(const ScopedCaller&) = delete;
    ScopedCaller& operator=(const ScopedCaller&) = delete;

private:
    bool active_;
    const char* previousCaller_;
    uintptr_t previousObject_;
    uint8_t previousSourceMask_;
    const char* previousProducerSite_;
    uintptr_t previousProducerSlot_;
    uintptr_t previousProducerHolder_;
    uintptr_t previousRootBase_;
    uintptr_t previousRootDerived_;
    size_t previousRootOffset_;
};

void NoteCrash();

void Report(const char* point);

} // namespace StartWhoDiag
} // namespace MapleRuntime

#endif // MRT_START_WHO_DIAG_H
