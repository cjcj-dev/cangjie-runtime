// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_EH_FRAMEINFO_H
#define MRT_EH_FRAMEINFO_H

#include "Base/MemUtils.h"
#include "Base/Types.h"
#include "CalleeSavedRegisterContext.h"
#include "Common/TypeDef.h"
#include "EhTable.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "ObjectModel/MFuncdesc.inline.h"
#include "StackMap/StackMapTable.h"

#if defined(CANGJIE_SANITIZER_SUPPORT)
#include "Sanitizer/SanitizerInterface.h"
#endif

#if defined(_WIN64)
#include "os/Windows/UnwindWin.h"
#endif
namespace MapleRuntime {

class IEHFrameInfo {
public:
    virtual ~IEHFrameInfo() = default;

    virtual bool IsCatchException() const = 0;
    virtual uint64_t GetTTypeIndex() const = 0;
    virtual uintptr_t GetLandingPad() const = 0;
    virtual void RestoreToCallerContext(CalleeSavedRegisterContext& context, uint32_t adjustedSize = 0) const = 0;

    virtual const uint32_t* GetIP() const = 0;
    virtual FrameAddress* GetFA() const = 0;
    virtual MachineFrame GetMachineFrame() const = 0;

    virtual const CString GetFunctionName() const = 0;
};

class EHFrameInfo : public FrameInfo, public IEHFrameInfo {
public:
    EHFrameInfo(const FrameInfo& info, const ExceptionWrapper& eWrapper) : FrameInfo(info)
    {
        // Abnormal EHTable layout:
        // lsdaStart:    0x55555555
        if (!EHTable::IsAbnormalEHTable(lsdaStart)) {
            auto pc = mFrame.GetIP();
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
            pc = reinterpret_cast<uint32_t*>(PtrauthAuthWithInstAkey(reinterpret_cast<Uptr>(pc),
                reinterpret_cast<Uptr>(mFrame.GetPtrAuthRAMod())));
#endif
            EHTable ehTable(pc, eWrapper, startProc, lsdaStart, result);
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
            result.landingPad = reinterpret_cast<Uptr>(
                                    PtrauthSignWithInstAkey(reinterpret_cast<Uptr>(result.landingPad),
                                    reinterpret_cast<Uptr>(mFrame.GetPtrAuthRAMod())));
#endif
        }
    }

    EHFrameInfo() = delete;
    ~EHFrameInfo() override = default;

    bool IsCatchException() const override
    {
        return result.isCaught;
    }

    uint64_t GetTTypeIndex() const override
    {
        return result.typeIndex;
    }

    uintptr_t GetLandingPad() const override
    {
        return result.landingPad;
    }

    void RestoreToCallerContext(CalleeSavedRegisterContext& context, uint32_t adjustedSize = 0) const override
    {
        ElfUnloadQuiescence::ReadScope metadataReader;
        constexpr uint8_t sizeOfAddr = sizeof(void*);       // arm32 is 4, aarch64 is 8
        constexpr uint8_t sizeOfStackHead = sizeOfAddr * 2; // callee rbp + return addr
#if defined(__x86_64__)
        context.rsp = context.rbp + sizeOfStackHead;
#elif defined(__aarch64__)
        context.sp = context.x29 + sizeOfStackHead;
        uint32_t calleeCount = 0;
#elif defined(__arm__)
        context.sp = context.r11 + sizeOfStackHead;
#endif
        Uptr calleeFrameAddress = reinterpret_cast<Uptr>(mFrame.GetFA());
        if (adjustedSize > 0) {
#if defined(__x86_64__)
            context.rbp = *reinterpret_cast<uint64_t*>(calleeFrameAddress);
#elif defined(__aarch64__)
            context.x29 = *reinterpret_cast<uint64_t*>(calleeFrameAddress);
#elif defined(__arm__)
            context.r11 = *reinterpret_cast<uint32_t*>(calleeFrameAddress);
#endif
            return;
        }
#ifdef __APPLE__
        FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(mFrame.GetFA());
#else
        FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(startProc));
#endif
        CHECK_DETAIL(funcDesc != nullptr, "managed frame missing funcdesc startPC=%p ip=%p",
                     reinterpret_cast<const void*>(startProc), reinterpret_cast<const void*>(mFrame.GetIP()));
        CHECK_DETAIL(funcDesc->GetStackMap() != nullptr, "managed frame missing stackmap startPC=%p ip=%p",
                     reinterpret_cast<const void*>(startProc), reinterpret_cast<const void*>(mFrame.GetIP()));
        const FramePrologue prologue(funcDesc->GetStackMap());
        const auto& saved = prologue.GetRegisters();
        for (size_t i = 0; i < saved.calleeSaved.size(); ++i) {
            const uint32_t idx = saved.calleeSaved[i];
            const uint32_t offset = saved.offset[i];
#if defined(_WIN64)
            if (idx < calleeSaveXMMIdxStart) {
                uint64_t* slotAddr = reinterpret_cast<uint64_t*>(calleeFrameAddress + SLOT_SIZE_FACTOR * offset);
                context.SetValueByIdx(idx, *slotAddr);
            } else {
                XMMReg* slotAddr = reinterpret_cast<XMMReg*>(calleeFrameAddress + SLOT_SIZE_FACTOR * offset);
                context.SetXMMValueByIdx(idx - calleeSaveXMMIdxStart, slotAddr);
            }
#elif defined(__aarch64__)
#if defined(__APPLE__)
            if (offset == 0 || offset == 1) {
                uint64_t* slotAddr = reinterpret_cast<uint64_t*>(calleeFrameAddress - SLOT_SIZE_FACTOR * offset);
                context.SetValueByIdx(idx, *slotAddr);
            } else {
                uint64_t* slotAddr = reinterpret_cast<uint64_t*>(calleeFrameAddress + SLOT_SIZE_FACTOR * offset);
                context.SetValueByIdx(idx, *slotAddr);
            }
#else // __APPLE__
            if (offset != 0 && offset != 1) {
                ++calleeCount;
            }
            uint64_t* slotAddr = reinterpret_cast<uint64_t*>(calleeFrameAddress + SLOT_SIZE_FACTOR * offset);
            context.SetValueByIdx(idx, *slotAddr);
#endif // __APPLE__
#elif defined(__arm__)
            constexpr uint32_t gprCount = 9; // r4-r11 and lr
            const auto* slotAddr = reinterpret_cast<const uint32_t*>(
                calleeFrameAddress + SLOT_SIZE_FACTOR * offset);
            if (idx < gprCount) {
                context.SetValueByIdx(idx, slotAddr[0]);
            } else {
                // ARM32 stackmap offsets use 4-byte slots. Read the low and high
                // halves separately to avoid a potentially unaligned 64-bit load.
                const uint64_t value = static_cast<uint64_t>(slotAddr[0]) |
                    (static_cast<uint64_t>(slotAddr[1]) << 32);
                context.SetValueByIdx(idx, value);
            }
#else // not (_WIN64 || __aarch64__)
            uint64_t* slotAddr = reinterpret_cast<uint64_t*>(calleeFrameAddress + SLOT_SIZE_FACTOR * offset);
            context.SetValueByIdx(idx, *slotAddr);
#endif
        }
#if defined(__x86_64__)
        context.rbp = *reinterpret_cast<uint64_t*>(calleeFrameAddress);
#elif defined(__aarch64__)
        context.x29 = *reinterpret_cast<uint64_t*>(calleeFrameAddress);
        constexpr uint8_t alignSize = 2;
        calleeCount = (calleeCount % alignSize != 0) ? calleeCount + 1 : calleeCount;
        context.sp += calleeCount * sizeOfAddr;
#elif defined(__arm__)
        context.r11 = *reinterpret_cast<uint32_t*>(calleeFrameAddress);
#endif

#if defined(_WIN64)
        Runtime& runtime = Runtime::Current();
        WinModuleManager& winModuleManager = runtime.GetWinModuleManager();
        context.rsp = GetCallerRsp(winModuleManager, mFrame);
#endif

#if defined(CANGJIE_TSAN_SUPPORT)
        // update tsan's function trace
        Sanitizer::TsanFuncRestoreContext(reinterpret_cast<const void*>(mFrame.GetIP()));
#endif
    }

    const uint32_t* GetIP() const override
    {
        return mFrame.GetIP();
    }

    FrameAddress* GetFA() const override
    {
        return mFrame.GetFA();
    }

    MachineFrame GetMachineFrame() const override
    {
        return mFrame;
    }

    const CString GetFunctionName() const override
    {
        return FrameInfo::GetFuncName();
    }

private:
    ScanResult result;
#if defined(__linux__) && defined(__x86_64__)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 5;
    static constexpr int64_t SLOT_SIZE_FACTOR = -8;
#elif defined(__APPLE__) && defined(__x86_64__)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 5;
    static constexpr int64_t SLOT_SIZE_FACTOR = -8;
#elif defined(__APPLE__) && defined(__aarch64__)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 20;
    static constexpr int64_t SLOT_SIZE_FACTOR = -8;
#elif defined(__aarch64__)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 20;
    static constexpr int64_t SLOT_SIZE_FACTOR = 8;
#elif defined(__arm__)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 17;
    static constexpr int64_t SLOT_SIZE_FACTOR = -4;
#elif defined(__linux__) && defined(__aarch64__)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 20;
    static constexpr int64_t SLOT_SIZE_FACTOR = 8;
#elif defined(_WIN64)
    static constexpr size_t CALLEE_SAVE_NUMBERS = 17;
    static constexpr size_t calleeSaveXMMIdxStart = 7;
    static constexpr int64_t SLOT_SIZE_FACTOR = -8;
#endif
};
} // namespace MapleRuntime
#endif // MRT_EH_FRAMEINFO_H
