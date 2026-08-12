// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Heap/Verify/StackFrameOracle.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <vector>

#include "Base/LogFile.h"
#include "Common/BaseObject.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "StackManager.h"
#include "UnwindStack/StackFrameCursor.h"

namespace MapleRuntime {
namespace {
std::atomic<size_t> g_compareCount{ 0 };
std::atomic<size_t> g_mismatchCount{ 0 };
std::atomic<size_t> g_matchCount{ 0 };

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// Product visitor only yields ObjectRef. Key = (slot_addr, object_bits).
// Under STW the stack is frozen, so &root is a stable occurrence identity.
using RootKey = std::pair<uintptr_t, uintptr_t>;

RootVisitor MakeCollector(std::vector<RootKey>& keys)
{
    return [&keys](ObjectRef& root) {
        keys.emplace_back(reinterpret_cast<uintptr_t>(&root), raw(root.LoadPlain()));
    };
}
} // namespace

bool StackFrameOracle::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_STACK_FRAME_ORACLE");
    return on;
}

bool StackFrameOracle::SkipFirstManagedEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_STACK_FRAME_ORACLE_SKIP");
    return on;
}

bool StackFrameOracle::FatalEnabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_STACK_FRAME_ORACLE_FATAL");
    return on;
}

void StackFrameOracle::CompareWithLegacy(const UnwindContext& topFrame, Mutator& mutator)
{
    if (!Enabled()) {
        return;
    }
    if (!MutatorManager::Instance().WorldStopped()) {
        LOG(RTLOG_ERROR,
            "[GCV2][stack-frame-oracle] refused: world not stopped env=MRT_GCV2_STACK_FRAME_ORACLE=1");
        return;
    }

    std::vector<RootKey> legacyKeys;
    std::vector<RootKey> cursorKeys;
    RootVisitor legacyVisitor = MakeCollector(legacyKeys);
    RootVisitor cursorVisitor = MakeCollector(cursorKeys);

    // Product arm: the live full-stack visitor (StackManager → GCStackInfo).
    StackManager::VisitStackRoots(topFrame, legacyVisitor, mutator);

    // Cursor arm: frame-scoped ProcessOne drain (optional skip for positive control).
    StackFrameCursor cursor(topFrame);
    bool skip = SkipFirstManagedEnabled();
    bool skipped = !skip;
    size_t managedSeen = 0;
    while (!cursor.Done()) {
        const FrameInfo* frame = cursor.CurrentFrame();
        if (frame == nullptr) {
            break;
        }
        if (frame->GetFrameType() == FrameType::MANAGED) {
            ++managedSeen;
            if (!skipped) {
                (void)cursor.SkipNextManagedFrame();
                skipped = true;
                continue;
            }
        }
        (void)cursor.ProcessOne(cursorVisitor, mutator);
    }

    std::sort(legacyKeys.begin(), legacyKeys.end());
    std::sort(cursorKeys.begin(), cursorKeys.end());

    g_compareCount.fetch_add(1, std::memory_order_relaxed);
    bool match = legacyKeys == cursorKeys;
    if (match) {
        g_matchCount.fetch_add(1, std::memory_order_relaxed);
        DLOG(ENUM,
             "[GCV2][stack-frame-oracle] MATCH mutator=%p legacy_roots=%zu cursor_roots=%zu "
             "managed_frames=%zu skip=%d env=MRT_GCV2_STACK_FRAME_ORACLE=1",
             &mutator, legacyKeys.size(), cursorKeys.size(), managedSeen, skip ? 1 : 0);
        // Positive control must fire when we skipped a MANAGED frame and legacy saw roots.
        // If skip was requested, a MANAGED frame existed, and multisets still match with
        // non-empty legacy roots, the skip path is silent — fail closed.
        if (skip && managedSeen > 0 && !legacyKeys.empty() && match) {
            LOG(RTLOG_ERROR,
                "[GCV2][stack-frame-oracle] POSITIVE_CONTROL_SILENT mutator=%p roots=%zu managed=%zu "
                "env=MRT_GCV2_STACK_FRAME_ORACLE_SKIP=1 (expected mismatch)",
                &mutator, legacyKeys.size(), managedSeen);
            g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
            g_matchCount.fetch_sub(1, std::memory_order_relaxed);
            if (FatalEnabled()) {
                LOG(RTLOG_FATAL, "[GCV2][stack-frame-oracle] fatal: positive control silent");
            }
        }
        return;
    }

    g_mismatchCount.fetch_add(1, std::memory_order_relaxed);
    size_t i = 0;
    size_t n = std::min(legacyKeys.size(), cursorKeys.size());
    while (i < n && legacyKeys[i] == cursorKeys[i]) {
        ++i;
    }
    LOG(RTLOG_ERROR,
        "[GCV2][stack-frame-oracle] MISMATCH mutator=%p legacy_roots=%zu cursor_roots=%zu first_diff_idx=%zu "
        "managed_frames=%zu skip=%d env=MRT_GCV2_STACK_FRAME_ORACLE=1",
        &mutator, legacyKeys.size(), cursorKeys.size(), i, managedSeen, skip ? 1 : 0);
    // skip arm expects mismatch → never FATAL on that arm.
    if (FatalEnabled() && !skip) {
        LOG(RTLOG_FATAL, "[GCV2][stack-frame-oracle] fatal multiset mismatch");
    }
}

size_t StackFrameOracle::CompareCount() { return g_compareCount.load(std::memory_order_relaxed); }
size_t StackFrameOracle::MismatchCount() { return g_mismatchCount.load(std::memory_order_relaxed); }
size_t StackFrameOracle::MatchCount() { return g_matchCount.load(std::memory_order_relaxed); }
void StackFrameOracle::ResetStats()
{
    g_compareCount.store(0, std::memory_order_relaxed);
    g_mismatchCount.store(0, std::memory_order_relaxed);
    g_matchCount.store(0, std::memory_order_relaxed);
}

} // namespace MapleRuntime
