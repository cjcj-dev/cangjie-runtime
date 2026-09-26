// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "UnwindStack/StackFrameCursor.h"

#include "Heap/z/zAddress.inline.hpp"
#include "Heap/z/zMark.hpp"
#include "Loader/ElfUnloadQuiescence.h"

namespace MapleRuntime {

StackFrameCursor::StackFrameCursor(const UnwindContext& topFrame) : stream(&topFrame)
{
    stream.Start();
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
        case FrameType::RETURN_SAFEPOINT:
            ProcessReturnFrame(visitor, derivedPtrVisitor, regSlotsMap, frame);
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
        case FrameType::RETURN_SAFEPOINT:
            ProcessReturnFrame(visitor, derivedPtrVisitor, regSlotsMap, frame);
            break;
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

void StackFrameCursor::CollectReturnRegisterRoots(const FrameInfo& frame, std::vector<ReturnRegisterRoot>& roots)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    RegSlotsMap regSlotsMap;
    RegRoot::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
#if defined(__x86_64__)
    constexpr RegisterNum startRegister = R10;
    constexpr RegisterNum siteRegister = R11;
#else
    constexpr RegisterNum startRegister = X17;
    constexpr RegisterNum siteRegister = X16;
#endif
    if (regSlotsMap.addrMap[startRegister] == nullptr || regSlotsMap.addrMap[siteRegister] == nullptr) {
        return;
    }
    const uintptr_t startPC = *reinterpret_cast<uintptr_t*>(regSlotsMap.addrMap[startRegister]);
    const uintptr_t sitePC = *reinterpret_cast<uintptr_t*>(regSlotsMap.addrMap[siteRegister]);
    if (startPC == 0 || sitePC == 0) {
        return;
    }
    ElfUnloadQuiescence::ReadScope metadataReader;
    StackMapBuilder builder(startPC, sitePC, 0);
    HeapReferenceMap map = builder.Build<HeapReferenceMap>();
    if (!map.IsValid()) {
        return;
    }
    RootVisitor capture = [&roots](RootSlot& slot) {
        BaseObject* object = to_object(safe(slot.LoadPlain()));
        if (object != nullptr) {
            roots.push_back(ReturnRegisterRoot { &slot, object });
        }
    };
    RegSlotsMap returned = regSlotsMap;
    (void)map.VisitRegRoots(capture, nullptr, returned);
#else
    (void)frame;
    (void)roots;
#endif
}

void StackFrameCursor::ProcessReturnFrame(const RootVisitor& visitor, const DerivedPtrVisitor* derivedPtrVisitor,
                                         RegSlotsMap& regSlotsMap, const FrameInfo& frame)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    ElfUnloadQuiescence::ReadScope metadataReader;
    RegRoot::RecordStubAllRegister(regSlotsMap, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
#if defined(__x86_64__)
    constexpr RegisterNum startRegister = R10;
    constexpr RegisterNum siteRegister = R11;
#else
    constexpr RegisterNum startRegister = X17;
    constexpr RegisterNum siteRegister = X16;
#endif
    const uintptr_t startPC = *reinterpret_cast<uintptr_t*>(regSlotsMap.addrMap[startRegister]);
    const uintptr_t sitePC = *reinterpret_cast<uintptr_t*>(regSlotsMap.addrMap[siteRegister]);
    StackMapBuilder builder(startPC, sitePC, 0);
    HeapReferenceMap roots = builder.Build<HeapReferenceMap>();
    // The returned frame is gone. Only the dedicated register map is legal;
    // neither spill slots nor its prologue's saved-register map may be used.
    if (roots.IsValid()) {
        RegSlotsMap returnedRegisters = regSlotsMap;
        DerivedPtrVisitor derived = derivedPtrVisitor != nullptr ? *derivedPtrVisitor :
            Mutator::MakeDerivedRootVisitor(visitor);
        roots.VisitDerivedPtr(derived, nullptr, returnedRegisters);
        roots.VisitRegRoots(visitor, nullptr, returnedRegisters);
    }
#else
    (void)visitor; (void)derivedPtrVisitor; (void)regSlotsMap; (void)frame;
#endif
}

bool StackFrameCursor::ProcessOne(const RootVisitor& visitor, Mutator& mutator,
                                  const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
    if (Done()) {
        return false;
    }

    ProcessFrame(*CurrentFrame(), regSlotsMap, visitor, mutator, derivedPtrVisitor, young);
    Advance();
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
    }
    heapMap.RecordCalleeSaved(regSlotsMap);
}
}

