// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StackManager.h"
#include <cstdint>
#include <cstring>

#include "Base/SysCall.h"
#include "Common/StackType.h"
#include "ExceptionManager.inline.h"
#include "Mutator/Mutator.h"
#include "ObjectManager.inline.h"
#include "UnwindStack/EhStackInfo.h"
#include "UnwindStack/GcStackInfo.h"
#include "UnwindStack/PrintSignalStackInfo.h"
#include "UnwindStack/PrintStackInfo.h"
#include "UnwindStack/StackGrowStackInfo.h"
#ifdef _WIN64
#include <windows.h>
#include "UnwindWin.h"
#endif
#if (defined(__linux__) || defined(__OHOS__) || defined(__ANDROID__)) && !defined(_WIN64)
#include <dlfcn.h>
#include <link.h>
#endif
#ifdef __APPLE__
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#ifndef __IOS__
#include <libproc.h>
#else
#include <mach/vm_prot.h>
#endif
#endif
#include "Inspector/CjHeapData.h"
#include "Heap/Allocator/AllocBuffer.h"
#ifdef CANGJIE_SANITIZER_SUPPORT
#include "Sanitizer/SanitizerInterface.h"
#include "StackMap/StackMap.h"
#endif
#include "CpuProfiler/CpuProfiler.h"
#include "Interpreter/InterpreterSpecific.h"
#include "Interpreter/Options.h"

#define LIBCANGJIE_RUNTIME "libcangjie-runtime"
#define LIBCANGJIE_STD_CORE "libcangjie-std-core"
#define LIBCANGJIE_CJTHREAD_TRACE "libcangjie-trace"

namespace MapleRuntime {
#ifdef __arm__
Uptr STACK_ADDR_MAX = UINT32_MAX;
#else
Uptr STACK_ADDR_MAX = ULLONG_MAX;
#endif
Uptr StackManager::rtStartAddr = STACK_ADDR_MAX;
Uptr StackManager::rtEndAddr = 0;
Uptr StackManager::cjThreadStartAddr = STACK_ADDR_MAX;
Uptr StackManager::cjThreadEndAddr = 0;
Uptr StackManager::cjcSoStartAddr = STACK_ADDR_MAX;
Uptr StackManager::cjcSoEndAddr = 0;
Uptr StackManager::traceSoStartAddr = STACK_ADDR_MAX;
Uptr StackManager::traceSoEndAddr = 0;
#ifdef INTERPRETER_ENABLED
Uptr StackManager::interpreterSoStartAddr = ULLONG_MAX;
Uptr StackManager::interpreterSoEndAddr = 0;
#endif

#if defined(COMPILE_DYNAMIC)
#if !defined(__APPLE__)
extern "C" Uptr* g_runtimeDynamicStart;
extern "C" Uptr* g_runtimeDynamicEnd;
#endif
#else
extern "C" Uptr* g_runtimeStaticStart;
extern "C" Uptr* g_runtimeStaticEnd;
#ifdef _WIN64
extern "C" uintptr_t g_cjThreadStaticStart;
extern "C" uintptr_t g_cjThreadStaticEnd;
#endif
#endif

extern "C" void MRT_LibraryOnLoad(uint64_t address, bool enableGC);

StackManager::StackManager() {}

void StackManager::Init()
{
    InitAddressScope();
    InitStackGrowConfig();
}

void StackManager::Fini() const {}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
void StackManager::PrintStackTrace(UnwindContext* uwContext)
{
    PrintStackInfo printStackInfo(uwContext);
    printStackInfo.FillInStackTrace();
    printStackInfo.PrintStackTrace();
}
#endif

void StackManager::PrintSignalStackTrace(UnwindContext* uwContext, uintptr_t pc, uintptr_t fa)
{
    PrintSignalStackInfo printSignalStackInfo(uwContext);
    if (uwContext->GetUnwindContextStatus() == UnwindContextStatus::RISKY) {
        printSignalStackInfo.GetSignalStack()[printSignalStackInfo.GetStackIndex()] = SigHandlerFrameinfo(
            MachineFrame(reinterpret_cast<FrameAddress*>(fa), reinterpret_cast<uint32_t*>(pc)), FrameType::NATIVE);
        printSignalStackInfo.SetStackIndex(printSignalStackInfo.GetStackIndex() + 1);
    }
    printSignalStackInfo.FillInStackTrace();
    printSignalStackInfo.PrintStackTrace();
}

void StackManager::PrintStackTraceForCpuProfile(UnwindContext* unContext, unsigned long long int cjThreadId)
{
    PrintStackInfo printStackInfo(unContext);
    printStackInfo.FillInStackTrace();
    auto stacks = printStackInfo.GetStack();
    std::vector<uint64_t> funcDescRefs;
    std::vector<FrameType> frameTypes;
    std::vector<uint32_t> lineNumbers;
    for (const auto& frame : stacks) {
#ifdef __APPLE__
        FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(frame.mFrame.GetFA());
#else
        FuncDescRef funcDesc = MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(frame.GetFuncStartPC()));
#endif
        StackMapBuilder stackMapBuild(reinterpret_cast<uintptr_t>(frame.GetFuncStartPC()),
            reinterpret_cast<uintptr_t>(frame.mFrame.GetIP()), 0, reinterpret_cast<uint64_t*>(funcDesc));
        MethodMap methodMap = stackMapBuild.Build<MethodMap>();
        uint32_t lineNum = methodMap.IsValid() ? methodMap.GetLineNum() : 0;
        funcDescRefs.emplace_back(reinterpret_cast<uint64_t>(funcDesc));
        frameTypes.emplace_back(frame.GetFrameType());
        lineNumbers.emplace_back(lineNum);
    }
    CpuProfiler::GetInstance().GetGenerator().Post(cjThreadId, funcDescRefs, frameTypes, lineNumbers);
}

void StackManager::RecordLiteFrameInfos(std::vector<uint64_t>& liteFrameInfos, size_t steps)
{
    PrintStackInfo printStackInfo;
    printStackInfo.FillInStackTrace();
    printStackInfo.ExtractLiteFrameInfoFromStack(liteFrameInfos, steps);
}

void StackManager::GetStackTraceByLiteFrameInfos(const std::vector<uint64_t>& liteFrameInfos,
                                                 std::vector<StackTraceElement>& stackTrace)
{
    StackInfo::GetStackTraceByLiteFrameInfos(liteFrameInfos, stackTrace);
}

void StackManager::GetStackTraceByLiteFrameInfo(const uint64_t ip, const uint64_t pc, const uint64_t funcDesc,
                                                StackTraceElement& ste)
{
    StackInfo::GetStackTraceByLiteFrameInfo(ip, pc, funcDesc, ste);
}

void StackManager::VisitStackRoots(const UnwindContext& topFrame, const RootVisitor& func, Mutator& mutator)
{
    GCStackInfo gcStackInfo(&topFrame);
    gcStackInfo.FillInStackTrace();
    gcStackInfo.VisitStackRoots(func, mutator);
}

void StackManager::VisitHeapReferencesOnStack(const UnwindContext& topFrame, const RootVisitor& rootVisitor,
    const DerivedPtrVisitor& derivedPtrVisitor, Mutator& mutator)
{
    GCStackInfo gcStackInfo(&topFrame);
    gcStackInfo.FillInStackTrace();
    gcStackInfo.VisitHeapReferencesOnStack(rootVisitor, derivedPtrVisitor, mutator);
}

void StackManager::VisitStackPtrMap(const UnwindContext& topFrame, const StackPtrVisitor& traceAndFixPtrVisitor,
                                    const StackPtrVisitor& fixPtrVisitor, const DerivedPtrVisitor& derivedPtrVisitor,
                                    Mutator& mutator)
{
    // Reuse gcStackInfo for stack unwind.
    StackGrowStackInfo stackInfo(&topFrame);
    stackInfo.FillInStackTrace();
    stackInfo.RecordStackPtrs(traceAndFixPtrVisitor, fixPtrVisitor, derivedPtrVisitor, mutator);
}

void StackManager::InitStackGrowConfig()
{
    auto cjStackGrow = std::getenv("cjStackGrow");
    if (cjStackGrow == nullptr) {
        return;
    }
    if (strlen(cjStackGrow) != 1) {
        LOG(RTLOG_ERROR, "unsupported cjStackGrow, cjStackGrow should be 0 or 1.\n");
        return;
    }

    switch (cjStackGrow[0]) {
        case '0':
            CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_OFF;
            return;
        case '1':
            CangjieRuntime::stackGrowConfig = StackGrowConfig::STACK_GROW_ON;
            return;
        default:
            LOG(RTLOG_ERROR, "unsupported cjStackGrow, cjStackGrow should be 0 or 1.\n");
    }
    return;
}

#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
std::vector<FrameInfo> GetCurrentStack(StackMode mode)
{
    switch (mode) {
        case StackMode::EH: {
            EHStackInfo ehStackInfo;
            ehStackInfo.FillInStackTrace();
            return ehStackInfo.GetStack();
        }
        case StackMode::GC: {
            GCStackInfo gcStackInfo;
            gcStackInfo.FillInStackTrace();
            return gcStackInfo.GetStack();
        }
        case StackMode::PRINT: {
            PrintStackInfo printStackInfo;
            printStackInfo.FillInStackTrace();
            return printStackInfo.GetStack();
        }
        default:
            LOG(RTLOG_FATAL, "StackMode is invalid");
    }
}
#endif

#if defined(__linux__) || defined(hongmeng) || defined(__arm__) || defined(__OHOS__) || defined(__ANDROID__)
static void GetSoAddrScope(const CString& str, Uptr& startAddr, Uptr& endAddr)
{
    int pos1 = str.Find('-');
    int pos2 = str.Find(' ');
    if (pos1 < 0 || pos2 < pos1) {
        return;
    }
    constexpr int8_t baseValue = 16;
    Uptr start = std::strtoull(str.SubStr(0, static_cast<uint64_t>(pos1)).Str(), nullptr, baseValue);
    startAddr = start < startAddr ? start : startAddr;
    Uptr end = std::strtoull(str.SubStr(pos1 + 1, static_cast<uint64_t>(pos2 - pos1)).Str(), nullptr, baseValue);
    endAddr = end > endAddr ? end : endAddr;
}

static void GetEachSoAddrScope(std::vector<CString>& soNameVec)
{
    FILE* file = fopen("/proc/self/maps", "r");
    if (file == nullptr) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressScope(): fail to open the file");
        return;
    }
    const int bufSize = 1024;
    char buf[bufSize] = {'\0'};
    while (fgets(buf, bufSize, file) != nullptr) {
        CString lineStr(buf);
        int protPos = lineStr.Find(' ');
        if (protPos < 0) {
            continue;
        }
        constexpr uint8_t protLen = 4;
        if (lineStr.SubStr(protPos + 1, protLen).Find('x') < 0) {
            continue;
        }
        char* baseName = CString::BaseName(lineStr);
        auto it = std::find(soNameVec.begin(), soNameVec.end(), baseName);
        if (it != soNameVec.end()) {
            if (strcmp(baseName, LIBCANGJIE_RUNTIME ".so\n") == 0) {
                GetSoAddrScope(lineStr, StackManager::rtStartAddr, StackManager::rtEndAddr);
            } else if (strcmp(baseName, "cjc\n") == 0) {
                GetSoAddrScope(lineStr, StackManager::cjcSoStartAddr, StackManager::cjcSoEndAddr);
#ifdef INTERPRETER_ENABLED
            } else {
                GetSoAddrScope(lineStr, StackManager::interpreterSoStartAddr, StackManager::interpreterSoEndAddr);
#endif
            }
        }
    }
    std::fclose(file);
}

#if defined(COMPILE_DYNAMIC) && (defined(__OHOS__) || defined(__ANDROID__))
// The runtime does not support static linking on OHOS and Android.
// On Android, extractNativeLibs=false lets bionic map the shared object directly
// from APK. /proc/self/maps then records the APK path instead of a plain so path,
// so basename matching cannot reliably find libcangjie-runtime.so. Use a known
// exported runtime symbol as an anchor to locate the loaded ELF image instead.
static bool GetSoTextAddrScopeFromSymbol(const void* symbol, Uptr& startAddr, Uptr& endAddr)
{
    Dl_info info;
    if (dladdr(symbol, &info) == 0 || info.dli_fbase == nullptr) {
        return false;
    }

    // dli_fbase is the runtime load base of the shared object that owns symbol.
    // Program headers are mapped at base + e_phoff for ET_DYN objects, so they can
    // be parsed directly from process memory without reopening the so file.
    const auto baseAddr = reinterpret_cast<Uptr>(info.dli_fbase);
    const auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(baseAddr);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return false;
    }

    // Runtime frame detection only needs the executable code mapping. ELF loaders
    // describe this mapping as PT_LOAD with PF_X; it corresponds to the text segment,
    // not necessarily to the section named ".text".
    Uptr textStart = STACK_ADDR_MAX;
    Uptr textEnd = 0;
    const auto* phdr = reinterpret_cast<const ElfW(Phdr)*>(baseAddr + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type != PT_LOAD || (phdr[i].p_flags & PF_X) == 0) {
            continue;
        }
        Uptr segStart = baseAddr + static_cast<Uptr>(phdr[i].p_vaddr);
        Uptr segEnd = segStart + static_cast<Uptr>(phdr[i].p_memsz);
        textStart = segStart < textStart ? segStart : textStart;
        textEnd = segEnd > textEnd ? segEnd : textEnd;
    }

    Uptr symbolAddr = reinterpret_cast<Uptr>(symbol);
#if defined(__arm__)
    // ARM32 may use the low bit to mark Thumb state. Clear it before comparing
    // the function pointer against the executable segment address range.
    symbolAddr &= ~static_cast<Uptr>(1);
#endif
    // Guard against accidentally using a symbol resolved from another object or a
    // malformed ELF image. The anchor symbol must live inside the executable load.
    if (textStart == STACK_ADDR_MAX || textEnd == 0 || symbolAddr < textStart || symbolAddr >= textEnd) {
        return false;
    }

    startAddr = textStart;
    endAddr = textEnd;
    return true;
}
#endif
#endif

#if defined(__linux__)
static bool IsCangjieExecutable()
{
    FILE* file = fopen("/proc/self/exe", "rb");
    if (file == nullptr) {
        return false;
    }

    ElfW(Ehdr) elfHeader;
    if (fread(&elfHeader, sizeof(elfHeader), 1, file) != 1 ||
        memcmp(elfHeader.e_ident, ELFMAG, SELFMAG) != 0 || elfHeader.e_shoff == 0 || elfHeader.e_shnum == 0 ||
        elfHeader.e_shentsize != sizeof(ElfW(Shdr)) || elfHeader.e_shstrndx >= elfHeader.e_shnum) {
        std::fclose(file);
        return false;
    }

    std::vector<ElfW(Shdr)> sectionHeaders(elfHeader.e_shnum);
    if (fseek(file, static_cast<long>(elfHeader.e_shoff), SEEK_SET) != 0 ||
        fread(sectionHeaders.data(), sizeof(ElfW(Shdr)), elfHeader.e_shnum, file) != elfHeader.e_shnum) {
        std::fclose(file);
        return false;
    }

    const ElfW(Shdr)& stringSection = sectionHeaders[elfHeader.e_shstrndx];
    std::vector<char> sectionNames(stringSection.sh_size);
    if (stringSection.sh_size == 0 || fseek(file, static_cast<long>(stringSection.sh_offset), SEEK_SET) != 0 ||
        fread(sectionNames.data(), 1, stringSection.sh_size, file) != stringSection.sh_size) {
        std::fclose(file);
        return false;
    }
    std::fclose(file);

    for (const auto& section : sectionHeaders) {
        if (section.sh_name < sectionNames.size() && strcmp(sectionNames.data() + section.sh_name, ".cjmetadata") == 0) {
            return true;
        }
    }
    return false;
}
#elif defined(_WIN64)
static bool IsCangjieExecutable()
{
    const HMODULE module = GetModuleHandle(nullptr);
    if (module == nullptr) {
        return false;
    }
    const auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<Uptr>(module) + static_cast<Uptr>(dosHeader->e_lfanew));
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (uint16_t i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section) {
        if (strncmp(reinterpret_cast<const char*>(section->Name), ".header", sizeof(".header") - 1) == 0) {
            return true;
        }
    }
    return false;
}
#elif defined(__APPLE__) && !defined(__IOS__)
static bool IsCangjieExecutable()
{
    const auto header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if (header == nullptr || header->magic != MH_MAGIC_64) {
        return false;
    }
    const uint8_t* commandPtr = reinterpret_cast<const uint8_t*>(header) + sizeof(mach_header_64);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        const auto command = reinterpret_cast<const load_command*>(commandPtr);
        if (command->cmd == LC_SEGMENT_64) {
            const auto segment = reinterpret_cast<const segment_command_64*>(command);
            const auto section = reinterpret_cast<const section_64*>(segment + 1);
            for (uint32_t j = 0; j < segment->nsects; ++j) {
                if (strncmp(section[j].sectname, "__cjmetaheader", sizeof("__cjmetaheader") - 1) == 0) {
                    return true;
                }
            }
        }
        commandPtr += command->cmdsize;
    }
    return false;
}
#endif

#if defined(__APPLE__) and !(defined(__IOS__) && !defined(COMPILE_DYNAMIC))
static bool EndWith(const char* str, const char* suffix)
{
    if (str == nullptr || suffix == nullptr) {
        return false;
    }
    size_t strLen = strlen(str);
    size_t suffixLen = strlen(suffix);
    if (suffixLen > strLen) {
        return false;
    }
    return strncmp(str + strLen - suffixLen, suffix, suffixLen) == 0;
}
#endif

#if defined(__APPLE__) and !defined(__IOS__)
static void InitAddressInfoOnDarwin(const char* dylib, Uptr& start, Uptr& end)
{
    int pid = GetPid();
    struct proc_regionwithpathinfo info;
    Uptr curAddr = 0;
    while (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, curAddr, &info, sizeof(info)) > 0) {
        const char* path = info.prp_vip.vip_path;
        Uptr priAddr = info.prp_prinfo.pri_address;
        Uptr priSize = info.prp_prinfo.pri_size;
        if (EndWith(path, dylib) && start == STACK_ADDR_MAX && end == 0) {
            start = priAddr;
            end = priAddr + priSize;
            break;
        }
        curAddr = priAddr + priSize;
    }
}
#endif

#if defined(__APPLE__) && defined(__IOS__)
/**
 * @brief Initialize an iOS Mach-O executable address range from a symbol inside the image.
 * @param symbol Address of a symbol resolved from the target image.
 * @param start Receives the lowest executable segment start address.
 * @param end Receives the highest executable segment end address.
 */
static void InitAddressInfoOnIOS(const void* symbol, Uptr& start, Uptr& end)
{
    if (symbol == nullptr) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressInfoOnIOS(): symbol is null");
        return;
    }

    Dl_info info;
    if (dladdr(symbol, &info) == 0 || info.dli_fbase == nullptr) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressInfoOnIOS(): failed to resolve Mach-O image");
        return;
    }

    auto rawHeader = reinterpret_cast<const mach_header*>(info.dli_fbase);
    if (rawHeader->magic != MH_MAGIC_64) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressInfoOnIOS(): unsupported Mach-O image");
        return;
    }

    intptr_t slide = 0;
    bool imageFound = false;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        if (_dyld_get_image_header(i) == rawHeader) {
            slide = _dyld_get_image_vmaddr_slide(i);
            imageFound = true;
            break;
        }
    }
    if (!imageFound) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressInfoOnIOS(): Mach-O image is not loaded");
        return;
    }

    auto header = reinterpret_cast<const mach_header_64*>(rawHeader);
    auto commandPtr = reinterpret_cast<const uint8_t*>(header) + sizeof(mach_header_64);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        auto command = reinterpret_cast<const load_command*>(commandPtr);
        if (command->cmd == LC_SEGMENT_64) {
            auto segment = reinterpret_cast<const segment_command_64*>(command);
            if ((segment->initprot & VM_PROT_EXECUTE) != 0) {
                Uptr segmentStart = static_cast<Uptr>(static_cast<int64_t>(segment->vmaddr) + slide);
                Uptr segmentEnd = segmentStart + static_cast<Uptr>(segment->vmsize);
                start = segmentStart < start ? segmentStart : start;
                end = segmentEnd > end ? segmentEnd : end;
            }
        }
        commandPtr += command->cmdsize;
    }
    if (start == STACK_ADDR_MAX || end == 0) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressInfoOnIOS(): failed to find executable segment");
    }
}
#endif

#if defined(_WIN64)
static void InitAddressInfoOnWindows(const char* lib, Uptr& start, Uptr& end)
{
    Runtime& runtime = Runtime::Current();
    WinModuleManager& winModuleManager = runtime.GetWinModuleManager();
    const WinModule* module = winModuleManager.GetWinModuleByName(lib);
    if (module != nullptr) {
        start = module->GetImageBaseStart();
        end = module->GetImageBaseEnd();
    }
}
#endif

void StackManager::InitAddressScope()
{
// Init cangjie-runtime address info.
#if defined(COMPILE_DYNAMIC)
#if defined(_WIN64) // Windows
    InitAddressInfoOnWindows(LIBCANGJIE_RUNTIME ".dll", StackManager::rtStartAddr, StackManager::rtEndAddr);
#elif defined(__APPLE__)
#if !defined(__IOS__) // MacOS
    InitAddressInfoOnDarwin("/" LIBCANGJIE_RUNTIME ".dylib", StackManager::rtStartAddr, StackManager::rtEndAddr);
#endif
// For iOS, use `dladdr` to obtain the dylib name and perform string comparison.
#elif defined(__OHOS__) || defined(__ANDROID__) // OHOS, ANDROID
    if (!GetSoTextAddrScopeFromSymbol(reinterpret_cast<const void*>(&MRT_LibraryOnLoad),
        StackManager::rtStartAddr, StackManager::rtEndAddr)) {
        std::vector<CString> rtSoNameVec = { LIBCANGJIE_RUNTIME ".so\n" };
        GetEachSoAddrScope(rtSoNameVec);
    }
#else                                                            // Linux
    StackManager::rtStartAddr = reinterpret_cast<Uptr>(&g_runtimeDynamicStart);
    StackManager::rtEndAddr = reinterpret_cast<Uptr>(&g_runtimeDynamicEnd);
#endif
#else                  // !COMPILE_DYNAMIC
#if defined(__APPLE__) || defined(_WIN64) // MacOS, iOS, Windows
    StackManager::rtStartAddr = reinterpret_cast<Uptr>(g_runtimeStaticStart);
    StackManager::rtEndAddr = reinterpret_cast<Uptr>(g_runtimeStaticEnd);
#ifdef _WIN64
    StackManager::cjThreadStartAddr = static_cast<Uptr>(g_cjThreadStaticStart);
    StackManager::cjThreadEndAddr = static_cast<Uptr>(g_cjThreadStaticEnd);
#endif
#else
    StackManager::rtStartAddr = reinterpret_cast<Uptr>(&g_runtimeStaticStart);
    StackManager::rtEndAddr = reinterpret_cast<Uptr>(&g_runtimeStaticEnd);
#endif
#endif

// Init cjc address info.
#if defined(__linux__)
    if (!IsCangjieExecutable()) {
        std::vector<CString> cjcSoNameVec = {"cjc\n"};
        GetEachSoAddrScope(cjcSoNameVec);
    }
#elif defined(_WIN64)                         // Windows
    if (!IsCangjieExecutable()) {
        InitAddressInfoOnWindows("cjc.exe", StackManager::cjcSoStartAddr, StackManager::cjcSoEndAddr);
    }
#elif defined(__APPLE__) && !defined(__IOS__) // MacOS
    if (!IsCangjieExecutable()) {
        InitAddressInfoOnDarwin("/cjc", StackManager::cjcSoStartAddr, StackManager::cjcSoEndAddr);
    }
#endif
}

#ifdef INTERPRETER_ENABLED
/**
 * @brief Initialize the interpreter code address range for stack frame classification.
 * @param libName Interpreter library name used for path-based lookup on Linux and macOS.
 * @param symbol Symbol inside the interpreter image used for Mach-O lookup on iOS.
 */
void StackManager::InitAddressScopeForInterpreter(const char* libName, const void* symbol)
{
#if defined(__APPLE__) && defined(__IOS__)
    InitAddressInfoOnIOS(symbol, StackManager::interpreterSoStartAddr, StackManager::interpreterSoEndAddr);
#elif defined(__linux__)
    std::vector<CString> interpreterSoName = {CString(libName).Combine("\n").Str()};
    GetEachSoAddrScope(interpreterSoName);
#elif defined(__APPLE__) && !defined(__IOS__) // MacOS
    InitAddressInfoOnDarwin(
        CString(libName).Str(), StackManager::interpreterSoStartAddr, StackManager::interpreterSoEndAddr);
#else
    LOG(RTLOG_FATAL, "Unsupported platform for interpreter");
#endif
}
#endif // INTERPRETER_ENABLED

void InitAddressScopeForCJthreadTrace()
{
#ifdef _WIN64
    Runtime& runtime = Runtime::Current();
    WinModuleManager& winModuleManager = runtime.GetWinModuleManager();
    const WinModule* traceModule = winModuleManager.GetWinModuleByName(LIBCANGJIE_CJTHREAD_TRACE ".dll");
    if (traceModule != nullptr) {
        StackManager::traceSoStartAddr = traceModule->GetImageBaseStart();
        StackManager::traceSoEndAddr = traceModule->GetImageBaseEnd();
    }
#elif defined(__APPLE__)
#ifndef __IOS__
    InitAddressInfoOnDarwin("/" LIBCANGJIE_CJTHREAD_TRACE ".dylib", StackManager::traceSoStartAddr,
                            StackManager::traceSoEndAddr);
#endif
#else
    CString procFileName("/proc/self/maps");
    FILE* file = fopen(procFileName.Str(), "r");
    if (file == nullptr) {
        LOG(RTLOG_ERROR, "StackManager::InitAddressScope(): fail to open the file");
        return;
    }

    const int bufSize = 1024;
    char buf[bufSize] = { '\0' };
    while (fgets(buf, bufSize, file) != nullptr) {
        CString lineStr(buf);
        int protPos = lineStr.Find(' ');
        if (protPos < 0) {
            continue;
        }
        constexpr uint8_t protLen = 4;
        if (lineStr.SubStr(protPos + 1, protLen).Find('x') < 0) {
            continue;
        }
        char* baseName = CString::BaseName(lineStr);
        if (strcmp(baseName, LIBCANGJIE_CJTHREAD_TRACE ".so\n") == 0) {
            GetSoAddrScope(lineStr, StackManager::traceSoStartAddr, StackManager::traceSoEndAddr);
        }
    }

    std::fclose(file);
    if (StackManager::traceSoStartAddr == STACK_ADDR_MAX && StackManager::traceSoEndAddr == 0) {
        LOG(RTLOG_FATAL, "can not find Runtime trace so");
    }
#endif
}

bool StackManager::IsRuntimeFrame(Uptr pc)
{
#if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
    pc = PtrauthStripInstPointer(pc);
#endif
#ifdef CANGJIE_HWASAN_SUPPORT
    pc = Sanitizer::UntagAddr(pc);
#endif
#if defined(__IOS__) && defined(COMPILE_DYNAMIC) // iOS dynamic cangjie-runtime.
    Dl_info info;
    const void* addr = reinterpret_cast<const void*>(pc);
    if (dladdr(addr, &info) &&
        (EndWith(info.dli_fname, "/" LIBCANGJIE_RUNTIME ".dylib") ||
            EndWith(info.dli_fname, "/" LIBCANGJIE_CJTHREAD_TRACE ".dylib"))) {
        return true;
    }
    return false;
#else // Otherwise.
    // For platforms that do not support macro expansion, such as iOS and HOS, second condition will always be true
    // because there is no display setting for the address info of cjc. The same logic applies to cjthread-trace.
    return (pc > rtStartAddr && pc < rtEndAddr) || (pc > cjThreadStartAddr && pc < cjThreadEndAddr) ||
        (pc > cjcSoStartAddr && pc < cjcSoEndAddr) || (pc > traceSoStartAddr && pc < traceSoEndAddr);
#endif
}

#ifdef INTERPRETER_ENABLED
bool StackManager::IsInterpreterCodeAddr(Uptr addr)
{
    return interpreterSoStartAddr <= addr && addr < interpreterSoEndAddr;
}
#endif

} // namespace MapleRuntime
