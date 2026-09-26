// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_STACKINFO_H
#define MRT_STACKINFO_H

#include <vector>

#include "Common/BaseObject.h"
#include "Common/StackType.h"
#include "Interpreter/Options.h"

namespace MapleRuntime {

#ifdef INTERPRETER_ENABLED
constexpr int INTERPRETED_FRAME_FDESC = 0;
#endif

class Mutator;
// Shared frame stream and classifier. The watermark retains only the current
// unwind context; diagnostic consumers may still collect a vector of frames.
// HotSpot stackFrameStream.hpp and stackWatermark.cpp:44-63.
class StackFrameStream {
public:
    explicit StackFrameStream(const UnwindContext* context = nullptr)
        : n2cCount(0), lastFrameType(FrameType::UNKNOWN), topContext(context), isReliableN2CStub(false)
    {
        anchorFA = context == nullptr ? GetAnchorFAFromMutatorContext() : context->anchorFA;
    }
    void CheckTopUnwindContextAndInit(UnwindContext& context);
    bool IsN2CContext(const UnwindContext& context) const;
    void AnalyseAndSetFrameType(UnwindContext& context);
    void Start();
    void Next();
    void Rebase(intptr_t offset);
    bool IsDone() const { return done; }
    const FrameInfo& Current() const { return current.frameInfo; }

protected:
    uint32_t n2cCount;
    uint32_t* anchorFA = nullptr;
    FrameType lastFrameType;
#ifdef _WIN64
    UnwindContextStatus uwCtxStatus;
#endif

private:
    uint32_t* GetAnchorFAFromMutatorContext() const;
    const UnwindContext* topContext;
    bool isReliableN2CStub;
    UnwindContext current;
    bool done = true;
};

class StackInfo : public StackFrameStream {
public:
    explicit StackInfo(const UnwindContext* context = nullptr) : StackFrameStream(context)
    {
        constexpr int presetStackLength = 32;
        stack.reserve(presetStackLength);
    }
    virtual ~StackInfo() = default;
    void SetProcessingOwner(Mutator* owner) { processingOwner = owner; }
    std::vector<FrameInfo>& GetStack() { return stack; }
    void ExtractLiteFrameInfoFromStack(std::vector<uint64_t>& liteFrameInfos,
                                      size_t steps = STACK_UNWIND_STEP_MAX) const;
    static void GetStackTraceByLiteFrameInfos(const std::vector<uint64_t>& liteFrameInfos,
                                            std::vector<StackTraceElement>& stackTrace);
    static void GetStackTraceByLiteFrameInfo(uint64_t ip, uint64_t pc, uint64_t fa,
                                           StackTraceElement& ste);
    virtual void FillInStackTrace() = 0;
    static const int NEED_FILTED_FLAG;

protected:
    void ProcessOnIteration(const FrameInfo& frame);
    Mutator* processingOwner = nullptr;
    std::vector<FrameInfo> stack;
};

// Get current context frame info and fill to FrameInfo struct object.
#define MRT_UNW_GETCALLERFRAME(frame) \
    do { \
        void* ip = __builtin_return_address(0); \
        void* fa = __builtin_frame_address(0); \
        FrameAddress* thisFrame = reinterpret_cast<FrameAddress*>(fa); \
        (frame).mFrame.SetIP(reinterpret_cast<uint32_t*>(ip)); \
        (frame).mFrame.SetFA(thisFrame->callerFrameAddress); \
    } while (0)
} // namespace MapleRuntime
#endif // MRT_STACKINFO_H
