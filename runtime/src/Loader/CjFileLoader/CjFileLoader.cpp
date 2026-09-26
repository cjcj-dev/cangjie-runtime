// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CjFileLoader.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "ExceptionManager.inline.h"
#include "Common/ScopedObjectAccess.h"
#include "Loader/ElfUnloadQuiescence.h"
#include "schedule.h"
#include "timer.h"
#include "LoaderManager.h"
#include "Mutator/Handshake.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/ThreadSMR.h"
#include <unordered_map>
#include "ObjectManager.inline.h"
#include "TypeInfoManager.h"
#include "UnwindStack/GcStackInfo.h"
namespace MapleRuntime {

void CJFileLoader::Fini()
{
    ClearLoadedFiles();
}

void CJFileLoader::RegisterLoadFile(Uptr fileMetaAddr)
{
    ScopedEntryTrace trace("CJRT_RegisterLoadFile");
    BaseFile* file = GetBaseFileByMetaAddr(fileMetaAddr);
    if (file == nullptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    file->RegisterFile();
#ifndef __arm__
    AddPackageInfos(file);
#endif
    RegisterTypeExt(file);
    RegisterTypeInfoCreatedByFE(file);
    RegisterOuterTypeExtensions(file);
    file->SetRegistered(true);
}

BaseFile* CJFileLoader::GetBaseFileByMetaAddr(Uptr fileMetaAddr)
{
    BaseFile* file = nullptr;
    VisitBaseFile([&file, &fileMetaAddr](BaseFile* cJfile) {
        if (cJfile->GetFileMetaAddr() == fileMetaAddr) {
            file = cJfile;
            return true;
        } else {
            return false;
        }
    });
    return file;
}

void CJFileLoader::UnregisterLoadFile(Uptr fileMetaAddr)
{
    BaseFile* file = GetBaseFileByMetaAddr(fileMetaAddr);
    if (file != nullptr) {
        // zGeneration.cpp:1340-1368: unlink -> observer handshake -> purge.
        // RemoveLoadedFiles accepts an image-specific authorization from the
        // public entry, or performs the complete direct-callback protection.
        RemoveLoadedFiles(file);
    }
}
void CJFileLoader::AddLoadedFiles(BaseFile* baseFile)
{
    // Platform loader queries happen before catalog/admission/pending locks.
    baseFile->SetImageAddressMap(ElfUnloadQuiescence::LinkImage(baseFile->GetFileMetaAddr()));
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    loadedFiles.push_back(baseFile);
}

BaseFile* CJFileLoader::CreateFileRefFromAddr(Uptr fileMetaAddr)
{
    auto getBinaryInfoFromAddressFunc = GetBinaryInfoFromAddressFunc();
    CHECK(getBinaryInfoFromAddressFunc != nullptr);
    Os::Loader::BinaryInfo binInfo;
    int isGetBinInfoSuccess = getBinaryInfoFromAddressFunc(reinterpret_cast<void*>(fileMetaAddr), &binInfo);
    if (isGetBinInfoSuccess == 0) {
        isGetBinInfoSuccess = Os::Loader::GetBinaryInfoFromAddress(reinterpret_cast<void*>(fileMetaAddr), &binInfo);
    }
    CHECK(isGetBinInfoSuccess != 0);
    BaseFile* file = BaseFile::CreateCJFile(FileType::C_FILE, CString(binInfo.filePathName), fileMetaAddr);
    if (file == nullptr) {
        return nullptr;
    }
    return file;
}

void CJFileLoader::AddPackageInfos(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    Uptr packageInfoBase = baseFile->GetPackageInfoBase();
    U32 pkgTotalSize = baseFile->GetPackageInfoTotalSize();
    while (pkgTotalSize > 0) {
        PackageInfo* packageInfo = reinterpret_cast<PackageInfo*>(packageInfoBase);
        const char* pkgName = packageInfo->GetPackageName();
        auto pkgIt = packageInfos.find(pkgName);
        if (pkgIt == packageInfos.end()) {
            packageInfos.insert({ pkgName, packageInfo });
            // record the relation between file and the packageInfo,
            // identify whether multiple packages exist in a file.
            auto fileIt = filePackageMap.find(baseFile->GetBaseName().Str());
            if (fileIt == filePackageMap.end()) {
                std::vector<PackageInfo*> pkgs { packageInfo };
                filePackageMap.insert({ baseFile->GetBaseName().Str(), pkgs });
            } else {
                fileIt->second.push_back(packageInfo);
            }
        }

        size_t packageInfoSize = packageInfo->GetPackageSize();
        if (pkgTotalSize >= packageInfoSize) {
            pkgTotalSize -= packageInfoSize;
        } else {
            break;
        }
        packageInfoBase += packageInfoSize;
    }
}

bool CJFileLoader::FileHasLoaded(const char* path)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = Os::Path::GetBaseName(path);
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt != filePackageMap.end()) {
        return true;
    }
    return false;
}

bool CJFileLoader::FileHasMultiPackage(const char* path)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = Os::Path::GetBaseName(path);
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt != filePackageMap.end() && fileIt->second.size() > 1) {
        return true;
    }
    return false;
}

void CJFileLoader::GetSubPackages(PackageInfo* packageInfo, std::vector<PackageInfo*> &subPackages)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString prefix = CString(packageInfo->GetPackageName()) + ".";
    for (auto &pkgInfoPair : packageInfos) {
        PackageInfo* pkgInfo = pkgInfoPair.second;
        if (CString(pkgInfo->GetPackageName()).StartWith(prefix)) {
            subPackages.emplace_back(pkgInfo);
        }
    }
}

// Traverse outer extension data grouped by BaseFile
void CJFileLoader::VisitExtensionData(
    TypeInfo* ti, const std::function<bool(ExtensionData* ed)>& f, TypeTemplate* tt) const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    ti->TryInitMTable();
    CHECK(loadedFiles.size() >= extensionDatas.size());
    for (auto baseFile : loadedFiles) {
        auto it1 = extensionDatas.find(baseFile);
        if (it1 == extensionDatas.end()) {
            continue;
        }
        auto& extensions = it1->second;
        auto range = extensions.equal_range(tt);
        if (range.first == range.second) {
            continue;
        }
        for (auto it2 = range.first; it2 != range.second; ++it2) {
            f(it2->second);
        }
    }
}

void CJFileLoader::ParseEnumCtor(TypeInfo* ti)
{
#ifdef __arm__
    return;
#endif
    TypeInfoManager& typeInfoMgr = TypeInfoManager::GetTypeInfoManager();
    if (ti->IsGenericTypeInfo()) {
        return typeInfoMgr.ParseEnumInfo(
            ti->GetSourceGeneric(), ti->GetTypeArgNum(), ti->GetTypeArgs(), ti);
    }
    EnumInfo* ei = ti->GetEnumInfo();
    if (ei == nullptr || ei->GetNumOfEnumCtor() == 0 || ei->IsParsed()) {
        return;
    }
    U32 enumCtorNum = ei->GetNumOfEnumCtor();
    for (U32 idx = 0; idx < enumCtorNum; ++idx) {
        EnumCtorInfo* enumCtorInfo = ei->GetEnumCtor(idx);
        void* fn = static_cast<void*>(enumCtorInfo->GetCtorFn());
        if (fn == nullptr) {
            continue;
        }
        TypeInfo* enumTi = reinterpret_cast<TypeInfo*>(
            TypeTemplate::ExecuteGenericFunc(fn, ti->GetTypeArgNum(), ti->GetTypeArgs()));
        enumCtorInfo->SetTypeInfo(enumTi);
    }
    ei->SetParsed();
}

void CJFileLoader::RegisterTypeExt(BaseFile* baseFile)
{
    Uptr typeExtBase = baseFile->GetTypeExtBase();
    Uptr typeExtEnd = typeExtBase + baseFile->GetTypeExtTotalSize();
    while (typeExtBase < typeExtEnd) {
        TypeExt* typeExt = reinterpret_cast<TypeExt*>(typeExtBase);
        constexpr uint32_t typeExtAlign = 16u;
        uint32_t sizeAlign = MRT_ALIGN(typeExt->size, typeExtAlign);
        typeExtBase += sizeAlign;
        typeExts.emplace(reinterpret_cast<void*>(typeExt->ti), typeExt);
    }
}

void CJFileLoader::RegisterTypeInfoCreatedByFE(BaseFile* baseFile)
{
    TypeInfoManager& typeInfoMgr = TypeInfoManager::GetTypeInfoManager();
    Uptr typeInfoBase = baseFile->GetTypeInfoBase();
    U32 typeInfoTotalSize = baseFile->GetTypeInfoTotalSize();
    typeInfoMgr.NoteTypeInfoImage(typeInfoBase, typeInfoTotalSize);
    Uptr typeInfoEnd = typeInfoBase + typeInfoTotalSize;
    while (typeInfoBase < typeInfoEnd) {
        TypeInfo* ti = reinterpret_cast<TypeInfo*>(typeInfoBase);
        constexpr uint32_t typeInfoAlign = 16u;
        constexpr uint32_t sizeAlign = MRT_ALIGN(sizeof(TypeInfo), typeInfoAlign);
        typeInfoBase += sizeAlign;
        auto tt = ti->GetSourceGeneric();
        if (tt != nullptr) {
            ti->SetvExtensionDataStart(tt->GetvExtensionDataStart());
        }
        typeInfoMgr.AddTypeInfo(ti);
        if (ti->IsEnum() || ti->IsTempEnum()) {
            ParseEnumCtor(ti);
        }
    }
    typeInfoMgr.InitAnyAndObjectType();

    Uptr staticGIBase = baseFile->GetStaticGIBase();
    Uptr staticGIEnd = staticGIBase + baseFile->GetStaticGISize();
    staticGIs.clear();
    while (staticGIBase < staticGIEnd) {
        I32 offset = *reinterpret_cast<I32*>(staticGIBase);
#if defined(__APPLE__)
        TypeInfo* ti = reinterpret_cast<TypeInfo*>(staticGIBase - offset);
#else
        TypeInfo* ti = reinterpret_cast<TypeInfo*>(staticGIBase + offset);
#endif
        staticGIBase += sizeof(I32);
        staticGIs.push_back(ti);
        if (ti->IsEnum() || ti->IsTempEnum()) {
            continue;
        } else if (ti->IsGenericTypeInfo() && ti->ReflectInfoIsNull() && !ti->GetSourceGeneric()->ReflectInfoIsNull()) {
            typeInfoMgr.FillReflectInfo(ti->GetSourceGeneric(), ti);
        }
    }
}

void CJFileLoader::RegisterOuterTypeExtensions(BaseFile* baseFile)
{
    TypeInfoManager& typeInfoMgr = TypeInfoManager::GetTypeInfoManager();
    Uptr extensionDataRefBase = baseFile->GetOuterTypeExtensionsBase();
    Uptr extensionDataRefEnd = extensionDataRefBase + baseFile->GetOuterTypeExtensionsSize();
    while (extensionDataRefBase < extensionDataRefEnd) {
        I32 offset = *reinterpret_cast<I32*>(extensionDataRefBase);
#ifdef __APPLE__
        ExtensionData* extensionData = reinterpret_cast<ExtensionData*>(extensionDataRefBase - offset);
#else
        ExtensionData* extensionData = reinterpret_cast<ExtensionData*>(extensionDataRefBase + offset);
#endif
        extensionDataRefBase += sizeof(I32);
        // for the extension of which target is a TypeInfo, since it cannot be used
        // in subsequent processes, we add MTable for it in advance so that it won't
        // need to be collected in `extensionDatas`.
        if (extensionData->TargetIsTypeInfo()) {
            TypeInfo* itf = extensionData->GetInterfaceTypeInfo();
            typeInfoMgr.AddTypeInfo(itf);
            TypeInfo* ti = reinterpret_cast<TypeInfo*>(extensionData->GetTargetType());
            typeInfoMgr.AddTypeInfo(ti);
            ti->AddMTable(itf, extensionData);
            continue;
        }
        TypeTemplate* tt = reinterpret_cast<TypeTemplate*>(extensionData->GetTargetType());
        extensionDatas[baseFile].emplace(tt, extensionData);
    }
}

bool CJFileLoader::VisitPackageInfoByPath(
    const char* path, const std::function<void(PackageInfo*)>& visitor)
{
    ElfUnloadQuiescence::ReadScope reader;
    PackageInfo* packageInfo = nullptr;
    {
        std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
        CString baseName = Os::Path::GetBaseName(path);
        auto fileIt = filePackageMap.find(baseName.Str());
        if (fileIt == filePackageMap.end()) {
            return false;
        }
        packageInfo = fileIt->second[0];
    }
    visitor(packageInfo);
    return true;
}

void CJFileLoader::RemovePackageInfo(const char* path)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = Os::Path::GetBaseName(path);
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt != filePackageMap.end()) {
        for (auto pkgInfo : fileIt->second) {
            packageInfos.erase(pkgInfo->GetPackageName());
        }
        filePackageMap.erase(baseName.Str());
    }
}

void CJFileLoader::RemovePackageInfo(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    CString baseName = baseFile->GetBaseName();
    auto fileIt = filePackageMap.find(baseName.Str());
    if (fileIt == filePackageMap.end()) {
        return;
    }

    std::unordered_set<PackageInfo*> removedPackages(fileIt->second.begin(), fileIt->second.end());
    for (PackageInfo* pkgInfo : fileIt->second) {
        auto pkgIt = packageInfos.find(pkgInfo->GetPackageName());
        if (pkgIt != packageInfos.end() && pkgIt->second == pkgInfo) {
            packageInfos.erase(pkgIt);
        }
    }
    filePackageMap.erase(fileIt);

    for (auto it = subPackageMap.begin(); it != subPackageMap.end();) {
        if (removedPackages.count(it->first) != 0) {
            it = subPackageMap.erase(it);
            continue;
        }
        auto& children = it->second;
        children.erase(std::remove_if(children.begin(), children.end(), [&removedPackages](PackageInfo* child) {
            return removedPackages.count(child) != 0;
        }), children.end());
        ++it;
    }
}

PackageInfo* CJFileLoader::GetPackageInfo(const char* pkgName) const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    PackageInfo* pkgInfo = nullptr;
    auto it = packageInfos.find(pkgName);
    if (it != packageInfos.end()) {
        pkgInfo = it->second;
        if (!pkgInfo->IsVaild()) {
            return nullptr;
        }
        return pkgInfo;
    }
    return nullptr;
}

void CJFileLoader::UnlinkLoadedFile(BaseFile* baseFile)
{
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    extensionDatas.erase(baseFile);
    loadedFiles.remove(baseFile);

    for (auto it = typeInfoCache.begin(); it != typeInfoCache.end();) {
        if (baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->second))) {
            it = typeInfoCache.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = typeTemplateCache.begin(); it != typeTemplateCache.end();) {
        if (baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->second))) {
            it = typeTemplateCache.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = typeExts.begin(); it != typeExts.end();) {
        if (baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->second)) ||
            baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(it->first))) {
            it = typeExts.erase(it);
        } else {
            ++it;
        }
    }
    staticGIs.erase(std::remove_if(staticGIs.begin(), staticGIs.end(), [baseFile](TypeInfo* ti) {
        return baseFile->IsAddrInCJFile(reinterpret_cast<Uptr>(ti));
    }), staticGIs.end());
    RemovePackageInfo(baseFile);

    baseFile->SetRegistered(false);
    baseFile->UnregisterFile();
}

void CJFileLoader::PurgeLoadedFile(BaseFile* baseFile)
{
    Uptr imageAddress = baseFile->GetFileMetaAddr();
    CHECK_DETAIL(ElfUnloadQuiescence::IsPurgeAuthorized(imageAddress),
                 "ELF metadata purge requires pending/active authorization");
    TypeInfoManager::GetTypeInfoManager().RemoveTypeInfosInRange(
        baseFile->GetTypeInfoBase(), baseFile->GetTypeInfoTotalSize());
    ElfUnloadQuiescence::UnlinkImage(imageAddress);
    delete baseFile;
}

void CJFileLoader::RemoveLoadedFiles(BaseFile* baseFile)
{
    Uptr imageAddress = baseFile->GetFileMetaAddr();

    // A callback with image-specific authorization reuses its caller's
    // protection. Otherwise establish pending-task and active-frame protection
    // before unlinking the image.
    bool authorizedByCaller = ElfUnloadQuiescence::IsPurgeAuthorized(imageAddress);
    std::unique_ptr<ElfUnloadQuiescence::PurgeAuthorizationScope> callbackAuthorization;
    if (!authorizedByCaller && ElfUnloadQuiescence::HasCallerPurgeProtection()) {
        CHECK_DETAIL(!ElfUnloadQuiescence::CallerProtectionHasPendingForImage(imageAddress),
                     "ELF dependent image has a pending task during public unload");
        CHECK_DETAIL(!HasActiveImageFrames(baseFile),
                     "ELF dependent image has an active frame during public unload");
        callbackAuthorization =
            std::make_unique<ElfUnloadQuiescence::PurgeAuthorizationScope>(imageAddress);
        authorizedByCaller = true;
    }
    std::unique_ptr<ElfUnloadQuiescence::TaskAdmissionScope> directAdmission;
    if (!authorizedByCaller) {
        // Recheck BOTH pending entries and active frames under each acquired
        // admission. Never hold exclusive admission while an initializer may
        // need to enter a dependency to finish its current pending task.
        for (;;) {
            directAdmission = std::make_unique<ElfUnloadQuiescence::TaskAdmissionScope>();
            if (directAdmission->HasPendingForImage(imageAddress)) {
                directAdmission.reset();
                ElfUnloadQuiescence::WaitForPendingTasks(imageAddress);
                continue;
            }
            if (!HasActiveImageFrames(baseFile)) {
                break;
            }
            directAdmission.reset();
            if (CJThreadGetHandle() != nullptr) {
                ScopedEnterSaferegion safe(false);
                TimerSleep(1000000); // Native scheduler timer; no stdlib bootstrap.
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    ElfUnloadQuiescence::UnloadScope unload(imageAddress);

    // Unlink every discovery surface while pre-cut readers retain their use
    // right. StaticRootTable::UnregisterRoots also waits for an in-flight GC
    // root visitor because both operations hold gcRootsLock.
    UnlinkLoadedFile(baseFile);

    // GC code lookup readers are not mutators, so drain them explicitly before
    // the mutator rendezvous. Admission remains closed through purge.
    unload.Synchronize();

    if (!authorizedByCaller) {
        ScopedEnterSaferegion enterSaferegion(false);
        // zGeneration.cpp:1352-1358: after unlink, rendezvous without
        // inspecting metadata whose reader admission is now closed.
        class UnloadRendezvousClosure final : public HandshakeClosure {
        public:
            UnloadRendezvousClosure() : HandshakeClosure("ELF unload quiescence") {}
            void do_thread(ThreadLocalData*) override {}
        };
        UnloadRendezvousClosure rendezvous;
        Handshake::execute(&rendezvous);
        ElfUnloadQuiescence::PurgeAuthorizationScope authorization(imageAddress);
        PurgeLoadedFile(baseFile);
        unload.OpenAdmission();
        return;
    }
    PurgeLoadedFile(baseFile);
    unload.OpenAdmission();
}

void CJFileLoader::VisitBaseFile(const std::function<bool(BaseFile*)>& f) const
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    for (auto file : loadedFiles) {
        if (f(file)) {
            return;
        }
    }
}

TypeInfo* CJFileLoader::FindTypeInfoFromLoadedFiles(const char* typeInfoName)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    auto it = typeInfoCache.find(typeInfoName);
    if (it != typeInfoCache.end()) {
        return it->second;
    }
    CString pkgName;
    CString typeInfoNameStr = CString(typeInfoName);
    int idx = typeInfoNameStr.RFind(":");
    if (idx < 0) {
        pkgName = "std.core";
    } else {
        pkgName = typeInfoNameStr.SubStr(0, idx);
    }
    auto pkgIt = packageInfos.find(pkgName.Str());
    if (pkgIt != packageInfos.end()) {
        PackageInfo* pkgInfo = pkgIt->second;
        TypeInfo* ti = pkgInfo->GetTypeInfo(typeInfoName);
        if (ti == nullptr) {
            return nullptr;
        }
        typeInfoCache.insert({ ti->GetName(), ti });
        return ti;
    }
    return nullptr;
}

TypeTemplate* CJFileLoader::FindTypeTemplateFromLoadedFiles(const char* typeTemplateName)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    auto it = typeTemplateCache.find(typeTemplateName);
    if (it != typeTemplateCache.end()) {
        return it->second;
    }
    CString pkgName;
    CString typeTemplateNameStr = CString(typeTemplateName);
    int idx = typeTemplateNameStr.RFind(":");
    if (idx < 0) {
        pkgName = "std.core";
    } else {
        pkgName = typeTemplateNameStr.SubStr(0, idx);
    }
    auto pkgIt = packageInfos.find(pkgName.Str());
    if (pkgIt != packageInfos.end()) {
        PackageInfo* pkgInfo = pkgIt->second;
        TypeTemplate* tt = pkgInfo->GetTypeTemplate(typeTemplateName);
        if (tt == nullptr) {
            return nullptr;
        }
        typeTemplateCache.insert({ tt->GetName(), tt });
        return tt;
    }
    return nullptr;
}

void CJFileLoader::RecordTypeInfo(TypeInfo* ti)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    typeInfoCache.insert({ ti->GetName(), ti });
}

void CJFileLoader::ClearLoadedFiles()
{
    // CangjieRuntime clears Runtime::runtime before module finalization, so a
    // shutdown cleanup cannot create ScopedStopTheWorld or query
    // MutatorManager::Instance(). Both runtime exits retire their entry mutator
    // and stop the scheduler before FiniAndDelete, so no active image frame can
    // be created or remain here. Close task admission for the complete cleanup
    // and prove the remaining pending side explicitly for every image.
    CHECK_DETAIL(Runtime::CurrentRef() == nullptr,
                 "ELF shutdown cleanup requires a stopped runtime");
    ElfUnloadQuiescence::TaskAdmissionScope shutdownAdmission;
    for (;;) {
        BaseFile* file = nullptr;
        {
            std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
            if (loadedFiles.empty()) {
                return;
            }
            file = loadedFiles.front();
        }
        Uptr imageAddress = file->GetFileMetaAddr();
        shutdownAdmission.WaitUntilNoPendingForImage(imageAddress);
        ElfUnloadQuiescence::PurgeAuthorizationScope authorization(
            imageAddress, shutdownAdmission);
        RemoveLoadedFiles(file);
    }
}

bool CJFileLoader::LibInit(const char* libName)
{
    BaseFile* baseFile = nullptr;
    std::unique_ptr<ElfUnloadQuiescence::PendingTask> pending;
    {
        ScopedEnterSaferegion safe(false);
        ElfUnloadQuiescence::SharedTaskAdmissionScope admission;
        ElfUnloadQuiescence::ReadScope reader;
        baseFile = GetBaseFile(libName);
        if (baseFile == nullptr) { return false; }
        std::vector<Uptr> entries;
        baseFile->GetGlobalInitFunc(entries);
        // An empty image has no initializer body to execute or protect.
        if (entries.empty()) { return true; }
        pending = std::make_unique<ElfUnloadQuiescence::PendingTask>(entries.front(), admission);
    }
    // OS TLS reader/short admission never spans managed code or CJThread park.
    return DoInitImage(baseFile);
}

#ifdef __OHOS__
void CJFileLoader::RegisterLoadFunc(void* loadFunc, void* loadLibraryFunc)
{
    binLoadApi.binLoad = (void*(*)(const char*))(loadFunc);
    binLoadApi.binLoadLib = (void*(*)(LibraryKind, const char*))(loadLibraryFunc);
}
#endif

void* CJFileLoader::LoadCJLibrary(const char* libName)
{
    CString baseName = Os::Path::GetBaseName(libName);
    {
        std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
        auto handlerIt =
            std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
                return baseName == Os::Path::GetBaseName(info.baseName.Str());
            });
        if (handlerIt != cjLibHandlers.end() && handlerIt->closing) {
            return nullptr;
        }
    }
    void* handler = binLoadApi.binLoad(libName);
    if (handler != nullptr) {
        std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
        auto handlerIt =
            std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
                return baseName == Os::Path::GetBaseName(info.baseName.Str());
            });
        if (handlerIt == cjLibHandlers.end()) {
            cjLibHandlers.push_back({ baseName, handler, 0, false });
        } else if (handlerIt->closing) {
            binLoadApi.binUnload(handler);
            return nullptr;
        }
    }
    return handler;
}

#ifdef INTERPRETER_ENABLED
void* CJFileLoader::LoadInterpreter(const char* libName)
{
    if (binLoadApi.binLoadLib == nullptr) {
        return nullptr;
    }
    
    return binLoadApi.binLoadLib(LibraryKind::APP, libName);
}
#endif

int CJFileLoader::UnloadLibrary(const char* libName)
{
    if (libName == nullptr) {
        return -1;
    }
    CString baseName = Os::Path::GetBaseName(libName);
    void* handler = nullptr;
    U64 generation = 0;
    {
        std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
        auto handlerIt =
            std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
                return baseName == Os::Path::GetBaseName(info.baseName.Str());
            });
        if (handlerIt == cjLibHandlers.end() || handlerIt->closing) {
            return -1;
        }
        handler = handlerIt->handler;
        generation = ++handlerIt->generation;
        handlerIt->closing = true;
    }

    auto rollbackHandler = [this, &baseName, generation]() {
        std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
        auto handlerIt =
            std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName, generation](const LibNameToHandler& info) {
                return baseName == Os::Path::GetBaseName(info.baseName.Str()) && info.generation == generation;
            });
        if (handlerIt != cjLibHandlers.end()) {
            handlerIt->closing = false;
        }
    };

    int ret = -1;
    Uptr imageAddress = 0;
    bool imageClosed = false;
    LibNameToHandler handlerItStorage { baseName, handler, generation, true };
    const LibNameToHandler* handlerIt = &handlerItStorage;
    if (LoaderManager::GetInstance()->GetInitStatus()) {
        BaseFile* baseFile = GetBaseFile(baseName);
        if (baseFile == nullptr) {
            rollbackHandler();
            return -1;
        }
        imageAddress = baseFile->GetFileMetaAddr();
        {
            ElfUnloadQuiescence::TaskAdmissionScope taskAdmission;
            if (taskAdmission.HasPendingForImage(imageAddress)) {
                LOG(RTLOG_WARNING, "refuse to unload queued Cangjie image %s", baseName.Str());
                rollbackHandler();
                return -1;
            }
            if (HasActiveImageFrames(baseFile)) {
                LOG(RTLOG_WARNING, "refuse to unload active Cangjie image %s", baseName.Str());
                rollbackHandler();
                return -1;
            }
            (void)ElfUnloadQuiescence::BeginImageClosing(imageAddress);
        }
        // zUnload.cpp:166: platform reclamation runs outside a safepoint.
        ret = binLoadApi.binUnload(handlerIt->handler);
        imageClosed = GetBaseFile(baseName) == nullptr || !ElfUnloadQuiescence::IsLinkedAddress(imageAddress);
    } else {
        ret = binLoadApi.binUnload(handlerIt->handler);
    }

    std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
    auto commitIt =
        std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName, generation](const LibNameToHandler& info) {
            return baseName == Os::Path::GetBaseName(info.baseName.Str()) && info.generation == generation;
        });
    const bool shouldErase = imageClosed || (imageAddress == 0 && ret == 0);
    if (commitIt == cjLibHandlers.end()) {
        if (imageAddress != 0) {
            if (imageClosed) {
                ElfUnloadQuiescence::CommitImageClosing(imageAddress);
            } else {
                ElfUnloadQuiescence::AbortImageClosing(imageAddress);
            }
        }
        return ret;
    }
    if (shouldErase) {
        if (imageClosed) {
            ElfUnloadQuiescence::CommitImageClosing(imageAddress);
        } else if (imageAddress != 0) {
            ElfUnloadQuiescence::AbortImageClosing(imageAddress);
        }
        cjLibHandlers.erase(commitIt);
    } else {
        commitIt->closing = false;
        if (imageAddress != 0) {
            ElfUnloadQuiescence::AbortImageClosing(imageAddress);
        }
    }
    return ret;
}

bool CJFileLoader::HasActiveImageFrames(BaseFile* baseFile) const
{
    class ActiveImageClosure final : public HandshakeClosure {
    public:
        ActiveImageClosure(Uptr image, const ThreadsListHandle& threads)
            : HandshakeClosure("ELF active-image preflight"), imageAddress(image)
        {
            for (size_t i = 0; i < threads.length(); ++i) {
                scanned.emplace(threads.thread_at(i), false);
            }
            remaining = scanned.size();
        }

        void do_thread(ThreadLocalData* tls) override
        {
            // The handshake protects a running participant on its carrier.
            // Membership comes from the logical task snapshot, never from TLS.
            Scan(tls->mutator, false);
        }

        void ScanSafeThreads()
        {
            for (const auto& entry : scanned) {
                Scan(entry.first, true);
            }
        }

        bool IsComplete() const { return active || remaining == 0; }
        bool HasActiveFrames() const { return active; }

    private:
        void Scan(Mutator* mutator, bool requireSafe)
        {
            std::lock_guard<std::mutex> resultLock(lock);
            auto it = scanned.find(mutator);
            if (active || it == scanned.end() || it->second) { return; }
            mutator->MutatorLock();
            // Pairs with DoLeaveSaferegion: a parked participant cannot resume
            // while its saved frames are being inspected by this executor.
            if (requireSafe && !mutator->InSaferegion()) {
                mutator->MutatorUnlock();
                return;
            }
            if (mutator->IsManagedContext()) {
                ElfUnloadQuiescence::ReadScope metadataReader;
                GCStackInfo stackInfo(&mutator->GetUnwindContext());
                stackInfo.SetProcessingOwner(mutator);
                stackInfo.FillInStackTrace();
                for (const FrameInfo& frame : stackInfo.GetStack()) {
                    Uptr startPC = reinterpret_cast<Uptr>(frame.GetFuncStartPC());
                    Uptr framePC = reinterpret_cast<Uptr>(frame.mFrame.GetIP());
                    if (ElfUnloadQuiescence::IsAddressInImage(startPC, imageAddress) ||
                        ElfUnloadQuiescence::IsAddressInImage(framePC, imageAddress)) {
                        active = true;
                        break;
                    }
                }
            }
            it->second = true;
            --remaining;
            mutator->MutatorUnlock();
        }

        const Uptr imageAddress;
        std::unordered_map<Mutator*, bool> scanned;
        std::mutex lock;
        size_t remaining = 0;
        bool active = false;
    };

    // zStackWatermark.cpp:43 / zNMethod.cpp:388: on-stack code stays alive,
    // including unmounted tasks. Cangjie saves these stacks on logical Mutators
    // rather than heap stackChunks, so retain their complete SMR membership.
    ScopedEnterSaferegion enterSaferegion(false);
    ThreadsListHandle threads;
    ActiveImageClosure closure(baseFile->GetFileMetaAddr(), threads);
    for (;;) {
        closure.ScanSafeThreads();
        if (closure.IsComplete()) { break; }
        Handshake::execute(&closure);
        if (closure.IsComplete()) { break; }
    }
    return closure.HasActiveFrames();
}

Uptr CJFileLoader::FindSymbol(const CString libName, const CString symName) const
{
    CString baseName = Os::Path::GetBaseName(libName.Str());
    void* handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(libCjsoHandlersMutex);
        auto handlerIt =
            std::find_if(cjLibHandlers.begin(), cjLibHandlers.end(), [&baseName](const LibNameToHandler& info) {
                return baseName == Os::Path::GetBaseName(info.baseName.Str());
            });
        if (handlerIt == cjLibHandlers.end() || handlerIt->closing) {
            return 0;
        }
        handler = handlerIt->handler;
    }
    return reinterpret_cast<Uptr>(binLoadApi.findSymbol(handler, symName.Str()));
}


bool CJFileLoader::DoInitImage(BaseFile* baseFile) const
{
    // The caller may arrive from a native saferegion. The actual initializer
    // accesses the managed heap; SetManagedContext alone is not that transition.
    // Retain the image with PendingTask, never with an OS-TLS reader here.
    ScopedObjectAccess access;
    ScopedEntryTrace trace((CString("CJRT_INIT_LIBRARY_") + baseFile->GetBaseName()).Str());
    std::vector<Uptr> funcs;
    baseFile->GetGlobalInitFunc(funcs);
    for (Uptr func : funcs) {
        if (reinterpret_cast<void*>(func) != nullptr) {
            using FuncType = void (*)();
            FuncType initAddr = reinterpret_cast<FuncType>(func);
#if defined(__OHOS__) || defined(__IOS__)
            InitCJLibraryStub(reinterpret_cast<void*>(initAddr));
#else
            Mutator* mutator = ThreadLocal::GetMutator();
            if (mutator != nullptr) {
                mutator->SetManagedContext(true);
            }
            uintptr_t threadData = MapleRuntime::MRT_GetThreadLocalData();
            ExecuteCangjieStub(0, 0, 0, reinterpret_cast<void*>(initAddr), reinterpret_cast<void*>(threadData), 0);
            if (mutator != nullptr) {
                mutator->SetManagedContext(false);
            }
            if (ExceptionManager::HasPendingException()) {
                ExceptionRef ex = ExceptionManager::GetPendingException();
                LOG(RTLOG_ERROR, "Init Image fail! exception occurrence when init image, exception:%s ",
                    ex->GetTypeInfo()->GetName());
                ExceptionManager::ClearPendingException();
                return false;
            }
#endif
        }
    }
    return true;
}

BaseFile* CJFileLoader::GetBaseFile(CString fileName) const
{
    BaseFile* baseFile = nullptr;
    CString baseName = Os::Path::GetBaseName(fileName.Str());
    VisitBaseFile([&baseName, &baseFile](BaseFile* file) {
        if (file->GetBaseName() == baseName) {
            baseFile = file;
            return true;
        } else {
            return false;
        }
    });
    return baseFile;
}

bool CJFileLoader::CheckPackageCompatibility(BaseFile* file)
{
    if (file == nullptr) {
        return false;
    }
    CString packageName = file->GetRealPath();
    CString packageVersion = file->GetSDKVersion();
    bool isCompatible = compatibility.CheckPackageCompatibility(packageName, packageVersion);
    file->SetFileCompatibility(isCompatible);
    AddLoadedFiles(file);
    return isCompatible;
}

void CJFileLoader::TryThrowException(Uptr fileMetaAddr)
{
    BaseFile* file = GetBaseFileByMetaAddr(fileMetaAddr);
    if (file == nullptr || file->IsCompatible()) {
        return;
    }
    CString packageName = file->GetRealPath();
    CString packageVersion = file->GetSDKVersion();
    CString msg = "executable cangjie file ";
    msg.Append(packageName);
    msg.Append(CString::FormatString(" version %s is not compatible with deployed cangjie runtime version %s",
        packageVersion.Str(), compatibility.GetRuntimeSDKVersion()));
#ifndef DISABLE_VERSION_CHECK
    ExceptionManager::IncompatiblePackageExpection(msg);
    RemoveLoadedFiles(file);
#else
    LOG(RTLOG_WARNING, "%s", msg.Str());
#endif
}

U32 CJFileLoader::GetNumOfInterface(TypeInfo* ti)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::vector<TypeInfo*> itfs;
    ti->GetInterfaces(itfs);
    return itfs.size();
}

TypeInfo* CJFileLoader::GetInterface(TypeInfo* ti, U32 idx)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::vector<TypeInfo*> itfs;
    ti->GetInterfaces(itfs);
    if (idx >= itfs.size()) {
        return nullptr;
    }
    return itfs[idx];
}

TypeExt* CJFileLoader::GetTypeExt(void* type)
{
    ElfUnloadQuiescence::ReadScope reader;
    std::lock_guard<std::recursive_mutex> catalogLock(catalogMutex);
    auto it = typeExts.find(type);
    return it == typeExts.end() ? nullptr : it->second;
}

} // namespace MapleRuntime
