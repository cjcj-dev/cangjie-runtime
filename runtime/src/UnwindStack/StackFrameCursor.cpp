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

void StackFrameCursor::ProcessFrame(const FrameInfo& frame, const RegSlotsMap& regSlotsMap, const RootVisitor& visitor,
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
        // safepoint.cpp:794-837: return oops belong to HandleReturnSafepoint.
        // The stream updates saved-register locations; this stub has no roots.
        case FrameType::RETURN_SAFEPOINT:
        case FrameType::SAFEPOINT:
        case FrameType::C2R_STUB:
        case FrameType::C2N_STUB:
        case FrameType::EXSLUSIVE:
        default:
            // Locations are published by StackFrameStream::Next, not by the consumer.
            // oopMap.cpp:504-510 rejects a second update of the same frame.
            break;
    }
#else
    switch (frame.GetFrameType()) {
        case FrameType::MANAGED: {
            (void)young;
            StackFrameCursor::ProcessManagedFrame(visitor, derivedPtrVisitor, regSlotsMap, frame, mutator);
            break;
        }
        // safepoint.cpp:794-837: return oops belong to HandleReturnSafepoint.
        // The stream updates saved-register locations; this stub has no roots.
        case FrameType::RETURN_SAFEPOINT:
        case FrameType::SAFEPOINT:
        case FrameType::STACKGROW:
        case FrameType::C2R_STUB:
        case FrameType::C2N_STUB:
        case FrameType::EXSLUSIVE:
#ifdef INTERPRETER_ENABLED
        case FrameType::INTERPRETER_C2I:
#endif
        default:
            break;
    }
    (void)mutator;
#endif
}

void StackFrameCursor::CollectReturnRegisterRoots(const FrameInfo& frame, std::vector<ReturnRegisterRoot>& roots)
{
#if defined(__x86_64__) || defined(__aarch64__)
    RegSlotsMap saved;
    RegRoot::RecordStubAllRegister(saved, reinterpret_cast<Uptr>(frame.mFrame.GetFA()));
    saved.allRegistersSaved = false;
#if defined(__x86_64__)
    constexpr RegisterNum startRegister = R10;
    constexpr RegisterNum siteRegister = R11;
#else
    constexpr RegisterNum startRegister = X17;
    constexpr RegisterNum siteRegister = X16;
#endif
    if (saved.addrMap[startRegister] == nullptr || saved.addrMap[siteRegister] == nullptr) {
        return;
    }
    const uintptr_t startPC = *reinterpret_cast<uintptr_t*>(saved.addrMap[startRegister]);
    const uintptr_t sitePC = *reinterpret_cast<uintptr_t*>(saved.addrMap[siteRegister]);
    if (startPC == 0 || sitePC == 0) {
        return;
    }
    ElfUnloadQuiescence::ReadScope metadataReader;
    // safepoint.cpp:818-839 / codeCache.cpp:750-759: the returned frame
    // is gone; resolve its map by PC before protecting the saved return oop.
    const auto qualification = ElfUnloadQuiescence::FindFrameMetadata(sitePC, 3, startPC);
    const auto descriptor = reinterpret_cast<FuncDescRef>(qualification.descriptor);
    CHECK_DETAIL(descriptor != nullptr, "return frame missing funcdesc startPC=%p ip=%p",
                 reinterpret_cast<const void*>(startPC), reinterpret_cast<const void*>(sitePC));
    CHECK_DETAIL(qualification.match == ElfUnloadQuiescence::QualificationMatch::SAVED_SITE &&
                 qualification.kind == 3 && qualification.entry == startPC && qualification.site == sitePC &&
                 ElfUnloadQuiescence::ValidateFrameMetadata(qualification) && descriptor->HasReturnPoll(),
                 "return frame missing exact kind3 saved site");
    StackMapBuilder builder(startPC, sitePC, 0, reinterpret_cast<uint64_t*>(descriptor));
    HeapReferenceMap map = builder.Build<HeapReferenceMap>();
    CHECK_DETAIL(map.IsValid() || builder.GetInvalidReason() == StackMapInvalidReason::ZERO_ROOT_INDICES,
                 "return frame missing stackmap entry startPC=%p ip=%p",
                 reinterpret_cast<const void*>(startPC), reinterpret_cast<const void*>(sitePC));
    if (!map.IsValid()) { return; }
    RootVisitor capture = [&roots](RootSlot& slot) {
        BaseObject* object = to_object(safe(slot.LoadPlain()));
        if (object != nullptr) {
            roots.push_back(ReturnRegisterRoot { &slot, object });
        }
    };
    (void)map.VisitRegRoots(capture, nullptr, saved);
#else
    (void)frame;
    (void)roots;
#endif
}

bool StackFrameCursor::ProcessOne(const RootVisitor& visitor, Mutator& mutator,
                                  const DerivedPtrVisitor* derivedPtrVisitor, bool young)
{
    if (Done()) {
        return false;
    }

    const FrameInfo frame = *CurrentFrame();
    const bool returning = frame.GetFrameType() == FrameType::RETURN_SAFEPOINT;
    if (returning) { Advance(); }
    ProcessFrame(frame, RegMap(), visitor, mutator, derivedPtrVisitor, young);
    if (!returning) { Advance(); }
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
                                         const RegSlotsMap& regSlotsMap, const FrameInfo& frame, Mutator& mutator)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    uintptr_t startIP = reinterpret_cast<uintptr_t>(frame.GetStartProc());
    const auto descriptor = frame.GetQualifiedDescriptor();
    uintptr_t frameIP = reinterpret_cast<uintptr_t>(frame.mFrame.GetIP());
    uintptr_t frameAddress = reinterpret_cast<uintptr_t>(frame.mFrame.GetFA());
    StackMapBuilder builder = StackMapBuilder(startIP, frameIP, frameAddress, reinterpret_cast<uint64_t*>(descriptor));
    HeapReferenceMap heapMap = builder.Build<HeapReferenceMap>(true);
    CHECK_DETAIL(heapMap.IsValid() || builder.GetInvalidReason() == StackMapInvalidReason::ZERO_ROOT_INDICES,
                 "managed frame missing exact root map startPC=%p ip=%p", reinterpret_cast<void*>(startIP),
                 reinterpret_cast<void*>(frameIP));
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
}
}

