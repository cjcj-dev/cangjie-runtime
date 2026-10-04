// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/Types.h"
#include "Common/Aarch64StubLayout.h"
#include "Common/StackType.h"
#include "Common/TypeDef.h"
#include "os/Loader.h"
#include "StackMap/StackMap.h"
#include "StackMetadataHelper.h"
#include "Exception/EhFrameInfo.h"
#include "Loader/ElfUnloadQuiescence.h"

#include <cstdarg>
#include <cstring>

namespace MapleRuntime {
namespace {
void SigAppend(char* buf, size_t cap, const char* fmt, ...)
{
    size_t len = strnlen(buf, cap);
    CHECK_IN_SIG(len < cap);
    va_list args;
    va_start(args, fmt);
    int n = vsprintf_s(buf + len, cap - len, fmt, args);
    va_end(args);
    CHECK_IN_SIG(n != -1);
}
} // namespace
// The sender SP comes from the frame that is still present, never from a
// guessed frame size at an already dismantled return site.
uintptr_t FrameInfo::CallerSP() const
{
    const uintptr_t fp = reinterpret_cast<uintptr_t>(mFrame.GetFA());
#if defined(__x86_64__) && !defined(_WIN64)
    return fp + sizeof(FrameAddress);
#elif defined(__aarch64__) && !defined(__APPLE__)
    switch (GetFrameType()) {
        case FrameType::RETURN_SAFEPOINT:
        case FrameType::SAFEPOINT:
        case FrameType::STACKGROW: return fp + MRT_AARCH64_STUB_FRAME_BYTES;
        case FrameType::C2R_STUB: return fp + 8 * 14;
        case FrameType::C2N_STUB: return fp + 8 * 32;
        case FrameType::MANAGED: {
            ElfUnloadQuiescence::ReadScope reader;
            FuncDescRef desc = GetQualifiedDescriptor();
            if (desc == nullptr) { return 0; }
            CHECK_DETAIL(desc->GetStackMap() != nullptr, "managed frame missing stackmap startPC=%p ip=%p",
                         GetStartProc(), mFrame.GetIP());
            const FramePrologue prologue(desc->GetStackMap(), reinterpret_cast<Uptr>(desc->GetAOTQualification()));
            const size_t saved = prologue.GetSavedRegistersAboveFrameHead();
            return fp + sizeof(FrameAddress) + ((saved + 1) & ~size_t(1)) * sizeof(uintptr_t);
        }
        default: return 0;
    }
#else
    // No return poll ABI has been supplied for these compiler targets.
    return 0;
#endif
}

FuncDescRef FrameInfo::GetQualifiedDescriptor() const
{
    ElfUnloadQuiescence::AssertReaderActive();
    CHECK_DETAIL(ElfUnloadQuiescence::ValidateFrameMetadata(metadata), "managed frame metadata generation changed");
    return reinterpret_cast<FuncDescRef>(metadata.descriptor);
}

bool FrameInfo::ResolveProcInfo(U16 kind, bool diagnostic)
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    if (metadata.descriptor == 0) {
        metadata = ElfUnloadQuiescence::FindFrameMetadata(reinterpret_cast<Uptr>(mFrame.GetIP()), kind);
    } else {
        CHECK_DETAIL(metadata.kind == kind && ElfUnloadQuiescence::ValidateFrameMetadata(metadata),
                     "frame qualification changed before classification");
    }
    startProc = reinterpret_cast<const uint32_t*>(metadata.entry);
    lsdaStart = nullptr;
    if (metadata.descriptor == 0) { return false; }
    // frame.cpp:1158 / codeCache.cpp:750: select compiled identity before
    // consuming frame layout. A saved site, rather than pc-1, supplies the map.
    if (metadata.match == ElfUnloadQuiescence::QualificationMatch::NONE) {
        if (diagnostic) { return false; }
        CHECK_DETAIL(false, "CJ frame missing exact site qualification");
    }
    if (diagnostic && (metadata.bits & 2) == 0) { return false; }
    CHECK_DETAIL((metadata.bits & 2) != 0, "CJ frame layout is not qualified at saved PC");
#ifndef _WIN64
    if (diagnostic && ((metadata.bits & 1) == 0 || mFrame.GetFA() == nullptr)) { return false; }
    CHECK_DETAIL((metadata.bits & 1) != 0 && mFrame.GetFA() != nullptr, "CJ frame slot is not qualified");
#ifdef __APPLE__
    const Uptr savedDescriptor = *reinterpret_cast<const Uptr*>(reinterpret_cast<Uptr>(mFrame.GetFA()) - 16);
    if (diagnostic && savedDescriptor != metadata.descriptor) { return false; }
    CHECK_DETAIL(savedDescriptor == metadata.descriptor, "CJ frame descriptor disagrees with PC owner");
#else
    const Uptr savedEntry = reinterpret_cast<Uptr>(GetFuncStartPCFromFrameAddress(mFrame.GetFA()));
    if (diagnostic && savedEntry != metadata.entry) { return false; }
    CHECK_DETAIL(savedEntry == metadata.entry, "CJ frame entry disagrees with PC owner");
#endif
#endif
    const auto descriptor = GetQualifiedDescriptor();
    lsdaStart = reinterpret_cast<const uint8_t*>(descriptor->GetEHTable());
    return true;
}

void FrameInfo::PrintFrameInfo(uint32_t frameIdx) const
{
    if (frameIdx > 0 && fType == FrameType::NATIVE) {
        LOG(RTLOG_ERROR, "      ...");
        return;
    }
    CString methodName;
    CString fileName;
    uint32_t lineNumber = 0;
    CString outputStr(CString::FormatString("  #%d  %p", frameIdx, mFrame.GetIP()));
    if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        MangleNameHelper* mangleNameHelper = stackMetadataHelper.GetMangleNameHelper();
        mangleNameHelper->Demangle();
        if (mangleNameHelper->IsNeedFilt()) {
            methodName = mangleNameHelper->GetMangleName();
        } else {
            methodName = mangleNameHelper->GetDemangleName();
        }
        fileName = stackMetadataHelper.GetFilePathAndName();
        lineNumber = stackMetadataHelper.GetLineNumber();
        outputStr.Append(CString::FormatString(" in %s", methodName.IsEmpty() ? "?" : methodName.Str()));
        if (!fileName.IsEmpty()) {
            outputStr.Append(CString::FormatString(" at %s", fileName.Str()));
            if (lineNumber != 0) {
                outputStr.Append(CString::FormatString(":%d", lineNumber));
            }
        }
    } else {
        Os::Loader::BinaryInfo binInfo;
        (void)Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        fileName = CString(binInfo.filePathName);
        methodName = CString(binInfo.symbolName);
        outputStr.Append(CString::FormatString(" in %s", methodName.IsEmpty() ? "?" : methodName.Str()));
        if (!fileName.IsEmpty()) {
            outputStr.Append(CString::FormatString(" from %s", fileName.Str()));
        }
    }
    LOG(RTLOG_ERROR, outputStr.Str());
}

#if defined(__IOS__)
CString FrameInfo::GetFrameInfo(uint32_t frameIdx) const
{
    if (frameIdx > 0 && fType == FrameType::NATIVE) {
        return "";
    }
    CString methodName;
    CString fileName;
    uint32_t lineNumber = 0;
    CString outputStr(CString::FormatString("  frame #%d: %p", frameIdx, mFrame.GetIP()));
    if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        MangleNameHelper* mangleNameHelper = stackMetadataHelper.GetMangleNameHelper();
        mangleNameHelper->Demangle();
        methodName = mangleNameHelper->GetDemangleName();
        Os::Loader::BinaryInfo binInfo;
        (void)Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        CString outFileName = CString(binInfo.filePathName);
        outputStr.Append(" ");
        if (!outFileName.IsEmpty()) {
            outFileName = CString::Split(outFileName, '/').back();
            outputStr.Append(CString::FormatString("%s`", outFileName.Str()));
        }
        fileName = stackMetadataHelper.GetFileName();
        lineNumber = stackMetadataHelper.GetLineNumber();
        outputStr.Append(CString::FormatString("%s", methodName.IsEmpty() ? "?" : methodName.Str()));
        if (!fileName.IsEmpty()) {
            outputStr.Append(CString::FormatString(" at %s", fileName.Str()));
            if (lineNumber != 0) {
                outputStr.Append(CString::FormatString(":%d", lineNumber));
            }
        }
    } else {
        Os::Loader::BinaryInfo binInfo;
        (void)Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        fileName = CString(binInfo.filePathName);
        methodName = CString(binInfo.symbolName);
        outputStr.Append(" ");
        if (!fileName.IsEmpty()) {
            fileName = CString::Split(fileName, '/').back();
            outputStr.Append(CString::FormatString("%s`", fileName.Str()));
        }
        outputStr.Append(CString::FormatString("%s", methodName.IsEmpty() ? "?" : methodName.Str()));
    }
    outputStr.Append("\n");
    return outputStr;
}
#endif

FuncDescRef SigHandlerFrameinfo::GetFuncDescForSignal() const
{
    ElfUnloadQuiescence::ReadScope metadataReader;
    const auto& frame = GetMetadata();
    return ElfUnloadQuiescence::ValidateFrameMetadata(frame) ? reinterpret_cast<FuncDescRef>(frame.descriptor) : nullptr;
}

void SigHandlerFrameinfo::PrintFrameInfo(uint32_t frameIdx) const
{
    if (GetFrameType() == FrameType::UNKNOWN) {
        FLOG(RTLOG_ERROR, "  #%u %p CJ metadata/layout unavailable", frameIdx, mFrame.GetIP());
        return;
    }
    ElfUnloadQuiescence::ReadScope metadataReader;
    if (frameIdx > 0 && fType == FrameType::NATIVE) {
        FLOG(RTLOG_ERROR, "      ...");
        return;
    }

    constexpr size_t maxPrcessSize = 1024;
    char methodName[maxPrcessSize];
    char fileName[maxPrcessSize];
    uint32_t lineNumber = 0;
    char outputStr[maxPrcessSize];
    CHECK_IN_SIG(sprintf_s(outputStr, maxPrcessSize, "  #%d  %p", frameIdx, mFrame.GetIP()) != -1);
    if (fType == FrameType::MANAGED) {
        PrintManagedFrame(methodName, fileName, outputStr, lineNumber);
    } else {
        PrintNativeFrame(methodName, fileName, outputStr);
    }
    FLOG(RTLOG_ERROR, outputStr);
}

void SigHandlerFrameinfo::PrintManagedFrame(char* methodName, char* fileName, char* outputStr,
    uint32_t& lineNumber) const
{
    constexpr size_t maxPrcessSize = 1024;
    FuncDescRef funcDesc = GetFuncDescForSignal();
    if (funcDesc == nullptr) {
        return;
    }
    CHECK_IN_SIG(sprintf_s(methodName, maxPrcessSize, "%s", funcDesc->GetFuncName().Str()) != -1);
    CHECK_IN_SIG(sprintf_s(fileName, maxPrcessSize, "%s", funcDesc->GetFuncDir().Str()) != -1);
    if (*fileName != '\0') {
#ifdef _WIN64
        SigAppend(fileName, maxPrcessSize, "%s", "\\");
        SigAppend(fileName, maxPrcessSize, "%s", funcDesc->GetFuncFilename().Str());
#else
        SigAppend(fileName, maxPrcessSize, "%s", "/");
        SigAppend(fileName, maxPrcessSize, "%s", funcDesc->GetFuncFilename().Str());
#endif
    }
    StackMapBuilder stackMapBuild(reinterpret_cast<uintptr_t>(GetFuncStartPC()),
        reinterpret_cast<uintptr_t>(mFrame.GetIP()), reinterpret_cast<uintptr_t>(mFrame.GetFA()));
    MethodMap methodMap = stackMapBuild.Build<MethodMap>();
    lineNumber = methodMap.IsValid() ? methodMap.GetLineNum() : 0;
    SigAppend(outputStr, maxPrcessSize, " in %s", *methodName == '\0' ? "?" : methodName);
    if (*fileName != '\0') {
        SigAppend(outputStr, maxPrcessSize, " at %s", fileName);
        if (lineNumber != 0) {
            SigAppend(outputStr, maxPrcessSize, ":%d", lineNumber);
        }
    }
}

void SigHandlerFrameinfo::PrintNativeFrame(char* methodName, char* fileName, char* outputStr) const
{
    constexpr size_t maxPrcessSize = 1024;
    Os::Loader::BinaryInfo binInfo;
    CHECK_IN_SIG(Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo) != -1);
    CHECK_IN_SIG(sprintf_s(fileName, maxPrcessSize, "%s", binInfo.filePathName.Str()) != -1);
    CHECK_IN_SIG(sprintf_s(methodName, maxPrcessSize, "%s", binInfo.symbolName.Str()) != -1);
    SigAppend(outputStr, maxPrcessSize, " in %s", *methodName == '\0' ? "?" : methodName);
    if (*fileName != '\0') {
        SigAppend(outputStr, maxPrcessSize, " from %s", fileName);
    }
}

const CString FrameInfo::GetFuncName() const
{
    if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        stackMetadataHelper.GetMangleNameHelper()->Demangle();
        return stackMetadataHelper.GetDemangleName();
    } else {
        Os::Loader::BinaryInfo binInfo;
        Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        return CString(binInfo.symbolName);
    }
}

const CString FrameInfo::GetMethodName() const
{
    if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        stackMetadataHelper.GetMangleNameHelper()->Demangle();
        return stackMetadataHelper.GetMangleNameHelper()->GetMethodName();
    } else {
        Os::Loader::BinaryInfo binInfo;
        Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        return CString(binInfo.symbolName);
    }
}

const CString FrameInfo::GetPackClassName() const
{
    if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        stackMetadataHelper.GetMangleNameHelper()->Demangle();
        return stackMetadataHelper.GetMangleNameHelper()->GetPackClassName();
    } else {
        return CString();
    }
}

const CString FrameInfo::GetFileName() const
{
    if (fType == FrameType::RUNTIME) {
        Os::Loader::BinaryInfo binInfo;
        Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        return CString(binInfo.filePathName);
    } else {
        StackMetadataHelper stackMetadataHelper(*this);
        return stackMetadataHelper.GetFilePathAndName();
    }
}

const CString FrameInfo::GetFileNameForTrace() const
{
    if (fType == FrameType::RUNTIME) {
        return CString("libcangjie-runtime.so");
    } else if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        return stackMetadataHelper.GetFileName();
    } else {
        Os::Loader::BinaryInfo binInfo;
        Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo);
        return CString(binInfo.filePathName);
    }
}

uint32_t FrameInfo::GetFramePc() const
{
    auto pc = mFrame.GetIP();
    if (pc == nullptr) {
        return 0;
    }
    return *pc;
}

uint32_t FrameInfo::GetLineNum() const
{
    if (fType == FrameType::MANAGED) {
        StackMetadataHelper stackMetadataHelper(*this);
        return stackMetadataHelper.GetLineNumber();
    } else {
        return 0;
    }
}
} // namespace MapleRuntime
