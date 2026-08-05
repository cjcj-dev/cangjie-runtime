// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_NEXT_FIELD2_PROBE_H
#define MRT_HEAP_NEXT_FIELD2_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Node@+16 (Node.next) full-coverage install probe — default off.
// Gate: MRT_GCV2_NEXTFIELD2=1
// Optional: MRT_GCV2_NEXTFIELD2_DUMP_MAX=<N> (default 256)
//
// Only classifies installs whose slot is holder+16 and holder TypeInfo name is
// "default:Node" (cheap filter; 100% coverage on that field, zero sampling).
// Reuses remsetholder base-first Classify (POSCTRL 405).
class NextField2Probe {
public:
    static bool Enabled();

    static void NoteInstall(const char* path, const char* kind, void* slot, void* value);

    static void FlushSummary(const char* site);

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

#endif // MRT_HEAP_NEXT_FIELD2_PROBE_H
