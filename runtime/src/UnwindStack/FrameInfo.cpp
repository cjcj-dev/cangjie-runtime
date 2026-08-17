// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Base/Types.h"
#include "Common/StackType.h"
#include "Common/TypeDef.h"
#include "os/Loader.h"
#include "StackMap/StackMap.h"
#include "StackMetadataHelper.h"

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
void FrameInfo::ResolveProcInfo()
{
    startProc = GetFuncStartPC();
#ifdef __APPLE__
    FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(mFrame.GetFA());
#else
    FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(startProc));
#endif
    lsdaStart = reinterpret_cast<uint8_t*>(funcDesc->GetEHTable());
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

void SigHandlerFrameinfo::PrintFrameInfo(uint32_t frameIdx) const
{
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
        uintptr_t funcStartAddress = reinterpret_cast<uintptr_t>(GetFuncStartPC());
#ifdef __APPLE__
        FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(mFrame.GetFA());
#else
        FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(GetFuncStartPC()));
#endif
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

        StackMapBuilder stackMapBuild(funcStartAddress, reinterpret_cast<uintptr_t>(mFrame.GetIP()),
                                      reinterpret_cast<uintptr_t>(mFrame.GetFA()));
        MethodMap methodMap = stackMapBuild.Build<MethodMap>();
        lineNumber = methodMap.IsValid() ? methodMap.GetLineNum() : 0;
        SigAppend(outputStr, maxPrcessSize, " in %s", *methodName == '\0' ? "?" : methodName);
        if (*fileName != '\0') {
            SigAppend(outputStr, maxPrcessSize, " at %s", fileName);
            if (lineNumber != 0) {
                SigAppend(outputStr, maxPrcessSize, ":%d", lineNumber);
            }
        }
    } else {
        Os::Loader::BinaryInfo binInfo;
        CHECK_IN_SIG(Os::Loader::GetBinaryInfoFromAddress(mFrame.GetIP(), &binInfo) != -1);
        CHECK_IN_SIG(sprintf_s(fileName, maxPrcessSize, "%s", binInfo.filePathName.Str()) != -1);
        CHECK_IN_SIG(sprintf_s(methodName, maxPrcessSize, "%s", binInfo.symbolName.Str()) != -1);
        SigAppend(outputStr, maxPrcessSize, " in %s", *methodName == '\0' ? "?" : methodName);
        if (*fileName != '\0') {
            SigAppend(outputStr, maxPrcessSize, " from %s", fileName);
        }
    }
    FLOG(RTLOG_ERROR, outputStr);
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
