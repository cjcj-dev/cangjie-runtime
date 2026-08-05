// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_HEAP_B2RING_PROBE_H
#define MRT_HEAP_B2RING_PROBE_H

#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

// Ultra-light per-thread write ring: each ref install stores only (slot, value).
// Zero classify / zero type walk. Default off: MRT_GCV2_B2RING=1
// Cap entries: MRT_GCV2_B2RING_CAP (default 262144 = 256K entries × 16B)
// Dump dir:    MRT_GCV2_B2RING_DIR (default /root/b2ring-run/rings)
//
// Path tag via ScopedInstallPath (same sites as interiorwriter); path is NOT
// stored in the ring — only (slot,value) — to keep the hot path to two stores.
class B2RingProbe {
public:
    static bool Enabled();

    // Hot path: two stores + index bump. Full coverage, no sampling.
    static void NoteInstall(const char* path, const char* kind, void* slot, void* value);

    // Dump all registered TLS rings to files under B2RING_DIR. Safe for abort path.
    static void DumpAllRings(const char* reason, uintptr_t targetObj);

    // Offline helper: print match for targetObj from already-dumped text; also
    // scans live rings and logs RING_HIT / RING_MISS to stderr before dump.
    static void ScanAndLog(const char* reason, uintptr_t targetObj);

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

#endif // MRT_HEAP_B2RING_PROBE_H
