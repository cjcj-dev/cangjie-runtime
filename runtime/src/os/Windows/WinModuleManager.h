// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_WIN_MODULE_MANAGER_H
#define MRT_WIN_MODULE_MANAGER_H

#include <windows.h>
#include <psapi.h>

// This header is the only path by which <windows.h> reaches the C++ runtime
// core: Mutator.h includes UnwindWin.h, which includes this file, so 72 of the
// 138 translation units end up parsing <windows.h> before their own code.
// Many of them then say std::min(...) or std::numeric_limits<T>::max(), which
// the object-like expansion of a min/max macro would wreck.
//
// mingw-w64 already keeps those macros out of C++ -- minwindef.h:169 guards
// them with "#ifndef __cplusplus" before it even reaches "#ifndef NOMINMAX" --
// so -DNOMINMAX is a no-op here and is deliberately NOT defined (upstream has
// never defined it either).  That exemption is a property of the toolchain,
// not of this repository, so assert it rather than assume it: a toolchain that
// stops honouring it would otherwise surface as an unrelated-looking error in
// whichever GC file happened to call std::min first.
#if defined(min) || defined(max)
#error "<windows.h> defined min/max as macros in a C++ translation unit; \
this build needs -DNOMINMAX (see runtime/src/os/Windows/WinModuleManager.h)"
#endif

#include <unordered_set>

#include "Base/CString.h"
#include "Base/Types.h"
#include "Common/StackType.h"

namespace MapleRuntime {
struct RuntimeFunction {
    uint32_t startAddress;
    uint32_t endAddress;
    uint32_t unwindInfoOffset;
};

// windows module means dll or exe loaded in memory.
class WinModule {
public:
    WinModule(Uptr imageStart, Uptr imageEnd, RuntimeFunction* funcTable, uint32_t fTableCount, const char* name)
        : imageBaseStart(imageStart), imageBaseEnd(imageEnd), funcTable(funcTable), funcTableCount(fTableCount),
          moduleName(name) {}
    ~WinModule() = default;

    bool IsInModule(Uptr pc) const { return pc >= imageBaseStart && pc <= imageBaseEnd; }

    bool IsInRuntimeFunc(uint32_t index, Uptr pc) const
    {
        Uptr rvaPc = pc - imageBaseStart;
        return rvaPc >= funcTable[index].startAddress && rvaPc <= funcTable[index].endAddress;
    }

    Uptr GetImageBaseStart() const { return imageBaseStart; }

    Uptr GetImageBaseEnd() const { return imageBaseEnd; }

    CString GetModuleName() const { return moduleName; }

    RuntimeFunction* GetRuntimeFunction(Uptr rip) const;

private:
    Uptr imageBaseStart;
    Uptr imageBaseEnd;
    RuntimeFunction* funcTable;
    uint32_t funcTableCount;
    CString moduleName;
};

struct WinModuleHash {
    std::size_t operator()(const WinModule* module) const
    {
        return std::hash<std::string>()(module->GetModuleName().Str());
    }
};

struct WinModuleCmp {
    bool operator()(const WinModule* lhs, const WinModule* rhs) const
    {
        return lhs->GetModuleName() == rhs->GetModuleName();
    }
};

class WinModuleManager {
public:
    WinModuleManager() = default;
    ~WinModuleManager() = default;

    void Init();
    void Fini() const;

    WinModule* GetWinModuleByPc(Uptr pc) const;
    WinModule* GetWinModuleByName(CString name) const;

    void ReadWinModuleAtInit();
    void ReadWinModuleAtRunning();

private:
    void ReadModuleInfo(HMODULE* moduleHandler, int capacity);
    std::unordered_set<WinModule*, WinModuleHash, WinModuleCmp> winModules;
    std::unordered_set<std::string> nativeLibNames{
        "ntdll.dll",       "KERNEL32.DLL",        "KERNELBASE.dll", "msvcrt.dll",           "libgcc_s_seh-1.dll",
        "libstdc++-6.dll", "libwinpthread-1.dll", "ucrtbase.dll",   "dbghelp.dll",          "libssp-0.dll",
        "ADVAPI32.dll",    "sechost.dll",         "RPCRT4.dll",     "libsecurec.dll",       "CRYPTSP.dll",
        "rsaenh.dll",      "bcrypt.dll",          "CRYPTBASE.dll",  "bcryptPrimitives.dll", "SYSFER.DLL"
    };
};
} // namespace MapleRuntime
#endif // MRT_WIN_MODULE_MANAGER_H
