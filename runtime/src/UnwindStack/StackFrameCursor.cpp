// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "UnwindStack/StackFrameCursor.h"

#include "Heap/z/zMark.hpp"
#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {

namespace {
// Local fill that mirrors GCStackInfo::FillInStackTrace without depending on GCStackInfo layout.
class CursorFillStackInfo : public StackInfo {
public:
    explicit CursorFillStackInfo(const UnwindContext* context) : StackInfo(context) {}
    void FillInStackTrace() override
    {
        ElfUnloadQuiescence::ReadScope metadataReader;
        UnwindContext uwContext;
        CheckTopUnwindContextAndInit(uwContext);
        while (!uwContext.frameInfo.mFrame.IsAnchorFrame(anchorFA)) {
            AnalyseAndSetFrameType(uwContext);
            stack.emplace_back(uwContext.frameInfo);
            UnwindContext caller;
            lastFrameType = uwContext.frameInfo.GetFrameType();
#ifndef _WIN64
            if (uwContext.UnwindToCallerContext(caller) == false) {
#else
            if (uwContext.UnwindToCallerContext(caller, uwCtxStatus) == false) {
#endif
                LOG(RTLOG_ERROR,
                    "StackFrameCursor unwind truncated at frames=%zu ip=%p fa=%p",
                    stack.size(), uwContext.frameInfo.mFrame.GetIP(), uwContext.frameInfo.mFrame.GetFA());
                return;
            }
            uwContext = caller;
        }
    }
    using StackInfo::stack;
};
} // namespace

StackFrameCursor::StackFrameCursor(const UnwindContext& topFrame)
{
    CursorFillStackInfo filler(&topFrame);
    filler.FillInStackTrace();
    frames = std::move(filler.stack);
}

void StackFrameCursor::ProcessFrame(const FrameInfo& frame, RegSlotsMap& regSlotsMap, const RootVisitor& visitor,
                                    Mutator& mutator, const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
#ifdef __arm__
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            (void)young;
            StackFrameCursor::ProcessManagedFrame(visitor, derivedPtrVisitor, regSlotsMap, frame, mutator);
            break;
        }
        case FrameType::STACKGROW:
            LOG(RTLOG_FATAL, "STACKGROW frame is not supported in Process");
            break;
        case FrameType::SAFEPOINT:
            RegRoot::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2R_STUB:
            RegRoot::RecordStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2N_STUB:
            RegRoot::RecordC2NStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::EXSLUSIVE:
            RegRoot::RecordExclusiveStubCalleeSaved(regSlotsMap,
                                                             reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        default:
            break;
    }
#else
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            (void)young;
            StackFrameCursor::ProcessManagedFrame(visitor, derivedPtrVisitor, regSlotsMap, frame, mutator);
            break;
        }
        case FrameType::SAFEPOINT:
        case FrameType::STACKGROW:
            RegRoot::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        case FrameType::C2R_STUB:
        case FrameType::C2N_STUB:
        case FrameType::EXSLUSIVE:
#ifdef INTERPRETER_ENABLED
        case FrameType::INTERPRETER_C2I:
#endif
            RegRoot::RecordStubCalleeSaved(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
            break;
        default:
            break;
    }
    (void)mutator;
#endif
}

bool StackFrameCursor::ProcessOne(const RootVisitor& visitor, Mutator& mutator,
                                  const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
    if (Done()) {
        return false;
    }

    ProcessFrame(frames[index], regSlotsMap, visitor, mutator, derivedPtrVisitor, young);
    ++index;
    return true;
}

void StackFrameCursor::ProcessAll(const RootVisitor& visitor, Mutator& mutator,
                                  const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
    while (ProcessOne(visitor, mutator, derivedPtrVisitor, young)) {
    }
}

} // namespace MapleRuntime

namespace MapleRuntime {
void RecordRootMapMiss(StackMapInvalidReason reason, const FrameInfo& frame, uintptr_t startIP,
                      uintptr_t frameIP, const Mutator& mutator);

// HotSpot frame::oops_do_internal (frame.cpp:1166-1177) dispatches the
// managed frame map. Cangjie uses StackMapBuilder and a RegSlotsMap instead.
void StackFrameCursor::ProcessManagedFrame(const RootVisitor& visitor,
                                         const DerivedPtrVisitor* derivedPtrVisitor,
                                         RegSlotsMap& regSlotsMap, const FrameInfo& frame, Mutator& mutator)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
#ifdef __APPLE__
    if (MFuncDesc::GetFuncDesc(frame.mFrame.GetFA()) == nullptr) {
#else
    if (MFuncDesc::GetFuncDesc(startIP) == nullptr) {
#endif
        return;
    }
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackMapBuilder builder = StackMapBuilder(startIP, frameIP, frameAddress);
    HeapReferenceMap heapMap = builder.Build<HeapReferenceMap>(false);
    SlotDebugVisitor slotDebugFunc = nullptr;
    RegDebugVisitor regDebugFunc = nullptr;
    DerivedPtrVisitor derived =
        derivedPtrVisitor != nullptr ? *derivedPtrVisitor : Mutator::MakeDerivedRootVisitor(visitor);
    if (heapMap.IsValid()) {
        heapMap.VisitDerivedPtr(derived, nullptr, regSlotsMap);
        heapMap.VisitSlotRoots(visitor, slotDebugFunc);
        if (!heapMap.VisitRegRoots(visitor, regDebugFunc, regSlotsMap)) {
            LOG(RTLOG_FATAL, "wrong reg info, start ip: %p frame pc: %p", reinterpret_cast<void*>(startIP),
                reinterpret_cast<void*>(frameIP));
        }
    } else {
        RecordRootMapMiss(builder.GetInvalidReason(), frame, startIP, frameIP, mutator);
    }
    heapMap.RecordCalleeSaved(regSlotsMap);
}
}

namespace MapleRuntime {
namespace {
struct SkippedStackMapCounts {
    std::atomic<size_t> zeroEntries{ 0 };
    std::atomic<size_t> pcMiss{ 0 };
    std::atomic<size_t> zeroRootIndices{ 0 };
};

SkippedStackMapCounts g_skippedStackMapCounts;
thread_local size_t g_currentThreadRootMapMissCount = 0;

} // namespace

const char* StackMapInvalidReasonName(StackMapInvalidReason reason)
{
    switch (reason) {
        case StackMapInvalidReason::NONE:
            return "none";
        case StackMapInvalidReason::ZERO_ENTRIES:
            return "present-but-zero-entries";
        case StackMapInvalidReason::PC_MISS:
            return "pc-miss-exact";
        case StackMapInvalidReason::ZERO_ROOT_INDICES:
            return "zero-root-indices";
    }
    return "unknown";
}

void ResetSkippedStackMapCounts()
{
    g_skippedStackMapCounts.zeroEntries.store(0, std::memory_order_relaxed);
    g_skippedStackMapCounts.pcMiss.store(0, std::memory_order_relaxed);
    g_skippedStackMapCounts.zeroRootIndices.store(0, std::memory_order_relaxed);
}

void RecordRootMapMiss(StackMapInvalidReason reason, const FrameInfo& frame, uintptr_t startIP, uintptr_t frameIP,
                       const Mutator& mutator)
{
    (void)reason;
    (void)frame;
    (void)startIP;
    (void)frameIP;
    (void)mutator;
    ++g_currentThreadRootMapMissCount;
}

ATTR_NO_INLINE void RecordSkippedStackMap(StackMapInvalidReason reason, const FrameInfo&, uintptr_t,
                                          uintptr_t)
{
    std::atomic<size_t>* skippedCount = &g_skippedStackMapCounts.zeroRootIndices;
    switch (reason) {
        case StackMapInvalidReason::ZERO_ENTRIES:
            skippedCount = &g_skippedStackMapCounts.zeroEntries;
            break;
        case StackMapInvalidReason::PC_MISS:
            skippedCount = &g_skippedStackMapCounts.pcMiss;
            break;
        case StackMapInvalidReason::NONE:
        case StackMapInvalidReason::ZERO_ROOT_INDICES:
            skippedCount = &g_skippedStackMapCounts.zeroRootIndices;
            break;
    }
    skippedCount->fetch_add(1, std::memory_order_relaxed);

}

void ReportSkippedStackMapCounts()
{
    size_t zeroEntries = g_skippedStackMapCounts.zeroEntries.load(std::memory_order_relaxed);
    size_t pcMiss = g_skippedStackMapCounts.pcMiss.load(std::memory_order_relaxed);
    size_t zeroRootIndices = g_skippedStackMapCounts.zeroRootIndices.load(std::memory_order_relaxed);
    if (zeroEntries != 0 || pcMiss != 0 || zeroRootIndices != 0) {
        LOG(RTLOG_ERROR,
            "GC stack map warning: SKIPPED_ZERO_ENTRIES=%zu SKIPPED_PC_MISS=%zu "
            "SKIPPED_OTHER_ZERO_ROOT_INDICES=%zu",
            zeroEntries, pcMiss, zeroRootIndices);
    }
}




}

