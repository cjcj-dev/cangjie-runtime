// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_INTERIOR_WRITER_PROBE_H
#define MRT_HEAP_INTERIOR_WRITER_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Install-point probe: classify every ref-field store value as object-base vs interior.
// Reuses remsetholder/interiorfix base-first Classify (POSCTRL 405).
// Gate (default off): MRT_GCV2_INTERIOR_WRITER=1
// Optional dump cap:   MRT_GCV2_INTERIOR_WRITER_DUMP_MAX=<N> (default 64)
//
// Path tag: set via ScopedInstallPath around named GC writers; sink hooks pass "sink".
class InteriorWriterProbe {
public:
    static bool Enabled();

    // value = address about to be installed (plain object pointer or raw field bits decoded).
    // path: TryUpdateRefFieldImpl | CasInstallPlainTarget | FixMinorEvacuatedSlot | ...
    // kind: SetTargetObject | SetFieldValue | CompareExchange
    static void NoteInstall(const char* path, const char* kind, void* slot, void* value);

    static void FlushSummary(const char* site);

    // RAII path tag for named GC write paths (thread-local).
    class ScopedInstallPath {
    public:
        explicit ScopedInstallPath(const char* path);
        ~ScopedInstallPath();
        ScopedInstallPath(const ScopedInstallPath&) = delete;
        ScopedInstallPath& operator=(const ScopedInstallPath&) = delete;

    private:
        const char* prev_;
    };

    static const char* CurrentPath();
};

} // namespace MapleRuntime

#endif // MRT_HEAP_INTERIOR_WRITER_PROBE_H
